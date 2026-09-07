#include "gnss_time_source.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/pps.h>
#include <mutex>
#include <new>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <thread>
#include <time.h>
#include <unistd.h>

namespace {

constexpr uint32_t kDefaultBaud = 115200;
constexpr uint32_t kDefaultMaxRmcDelayMs = 300;
constexpr size_t kMaximumNmeaBuffer = 4096;
constexpr size_t kMaximumNmeaSentence = 1024;

uint64_t timespec_ns(const struct timespec &value)
{
    if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000L)
        return 0;
    return static_cast<uint64_t>(value.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(value.tv_nsec);
}

uint64_t pps_time_ns(const struct pps_ktime &value)
{
    if ((value.flags & PPS_TIME_INVALID) || value.sec < 0 || value.nsec < 0 ||
        value.nsec >= 1000000000)
        return 0;
    return static_cast<uint64_t>(value.sec) * 1000000000ULL +
           static_cast<uint64_t>(value.nsec);
}

bool monotonic_now_ns(uint64_t *value)
{
    struct timespec now = {};
    if (!value || clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return false;
    *value = timespec_ns(now);
    return *value != 0;
}

bool sample_realtime_minus_monotonic(int64_t *offset_ns,
                                     uint64_t *sample_span_ns)
{
    if (!offset_ns || !sample_span_ns)
        return false;
    struct timespec monotonic_before = {};
    struct timespec realtime = {};
    struct timespec monotonic_after = {};
    if (clock_gettime(CLOCK_MONOTONIC, &monotonic_before) < 0 ||
        clock_gettime(CLOCK_REALTIME, &realtime) < 0 ||
        clock_gettime(CLOCK_MONOTONIC, &monotonic_after) < 0)
        return false;
    const uint64_t before_ns = timespec_ns(monotonic_before);
    const uint64_t after_ns = timespec_ns(monotonic_after);
    const uint64_t realtime_ns = timespec_ns(realtime);
    if (!before_ns || after_ns < before_ns || !realtime_ns)
        return false;
    *sample_span_ns = after_ns - before_ns;
    const uint64_t midpoint_ns = before_ns + *sample_span_ns / 2;
    if (realtime_ns >= midpoint_ns) {
        const uint64_t difference = realtime_ns - midpoint_ns;
        if (difference > static_cast<uint64_t>(INT64_MAX))
            return false;
        *offset_ns = static_cast<int64_t>(difference);
    } else {
        const uint64_t difference = midpoint_ns - realtime_ns;
        if (difference > static_cast<uint64_t>(INT64_MAX) + 1ULL)
            return false;
        *offset_ns = difference == static_cast<uint64_t>(INT64_MAX) + 1ULL
                         ? INT64_MIN
                         : -static_cast<int64_t>(difference);
    }
    return true;
}

bool realtime_to_monotonic(uint64_t realtime_ns, int64_t offset_ns,
                           uint64_t *monotonic_ns)
{
    if (!realtime_ns || !monotonic_ns)
        return false;
    if (offset_ns >= 0) {
        const uint64_t offset = static_cast<uint64_t>(offset_ns);
        if (realtime_ns <= offset)
            return false;
        *monotonic_ns = realtime_ns - offset;
    } else {
        const uint64_t magnitude =
            static_cast<uint64_t>(-(offset_ns + 1)) + 1ULL;
        if (realtime_ns > UINT64_MAX - magnitude)
            return false;
        *monotonic_ns = realtime_ns + magnitude;
    }
    return true;
}

bool baud_to_termios(uint32_t baud, speed_t *speed)
{
    if (!speed)
        return false;
    switch (baud) {
    case 4800:
        *speed = B4800;
        return true;
    case 9600:
        *speed = B9600;
        return true;
    case 19200:
        *speed = B19200;
        return true;
    case 38400:
        *speed = B38400;
        return true;
    case 57600:
        *speed = B57600;
        return true;
    case 115200:
        *speed = B115200;
        return true;
#ifdef B230400
    case 230400:
        *speed = B230400;
        return true;
#endif
#ifdef B460800
    case 460800:
        *speed = B460800;
        return true;
#endif
#ifdef B921600
    case 921600:
        *speed = B921600;
        return true;
#endif
    default:
        return false;
    }
}

bool is_rmc_sentence(const std::string &sentence)
{
    if (sentence.size() < 7 || sentence.front() != '$')
        return false;
    const size_t comma = sentence.find(',');
    return comma != std::string::npos && comma >= 4 &&
           sentence.compare(comma - 3, 3, "RMC") == 0;
}

std::string make_nmea(const std::string &payload)
{
    unsigned char checksum = 0;
    for (unsigned char byte : payload)
        checksum ^= byte;
    char suffix[8] = {};
    std::snprintf(suffix, sizeof(suffix), "*%02X", checksum);
    return "$" + payload + suffix;
}

void set_error_locked(gnss_time_source_status_t *status, int error,
                      int saved_errno)
{
    status->last_error = error;
    status->last_errno = saved_errno;
}

}  // namespace

struct gnss_time_source {
    std::mutex mutex;
    std::atomic<bool> stop_requested{false};
    std::thread uart_thread;
    std::thread pps_thread;
    time_sync_service_t *time_sync = nullptr;
    gnss_time_source_config_t config = {};
    gnss_time_source_status_t status = {};
    std::string uart_device;
    std::string pps_device;
    std::string nmea_buffer;
    uint64_t next_pps_id = 1;
};

namespace {

bool wait_for_retry(gnss_time_source_t *source)
{
    for (int part = 0; part < 20; ++part) {
        if (source->stop_requested.load())
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return !source->stop_requested.load();
}

int configure_uart(int fd, uint32_t baud)
{
    speed_t speed = 0;
    if (!baud_to_termios(baud, &speed))
        return GNSS_TIME_ERR_BAUD;
    struct termios settings = {};
    if (tcgetattr(fd, &settings) < 0)
        return GNSS_TIME_ERR_IO;
    cfmakeraw(&settings);
    settings.c_cflag |= CLOCAL | CREAD;
    settings.c_cflag &= ~(CSTOPB | CRTSCTS | PARENB | CSIZE);
    settings.c_cflag |= CS8;
    /* PARMRK: deliver line breaks as "\377\0\0" so a BREAK on the wire
       can be used as the PPS edge (see consume_uart_bytes). */
    settings.c_iflag |= PARMRK;
    settings.c_iflag &= ~(IGNBRK | BRKINT | ISTRIP | IXON);
    settings.c_cc[VMIN] = 0;
    settings.c_cc[VTIME] = 0;
    if (cfsetispeed(&settings, speed) < 0 ||
        cfsetospeed(&settings, speed) < 0 ||
        tcsetattr(fd, TCSANOW, &settings) < 0) {
        return GNSS_TIME_ERR_IO;
    }
    tcflush(fd, TCIFLUSH);
    return GNSS_TIME_OK;
}

int inject_pps_locked(gnss_time_source_t *source, uint32_t kernel_sequence,
                      uint64_t local_realtime_ns, uint64_t monotonic_ns)
{
    if (!monotonic_ns ||
        (source->status.last_pps_monotonic_ns &&
         monotonic_ns <= source->status.last_pps_monotonic_ns)) {
        ++source->status.time_mapping_errors;
        set_error_locked(&source->status, GNSS_TIME_ERR_PPS, ERANGE);
        return GNSS_TIME_ERR_PPS;
    }
    const uint64_t pps_id = source->next_pps_id++;
    const int result =
        time_sync_on_pps(source->time_sync, pps_id, monotonic_ns);
    if (result != TIME_SYNC_OK) {
        ++source->status.time_mapping_errors;
        set_error_locked(&source->status, result, 0);
        return result;
    }
    ++source->status.pps_events;
    source->status.last_pps_id = pps_id;
    source->status.last_kernel_pps_sequence = kernel_sequence;
    source->status.last_pps_monotonic_ns = monotonic_ns;
    source->status.last_pps_local_realtime_ns = local_realtime_ns;
    source->status.last_error = GNSS_TIME_OK;
    source->status.last_errno = 0;
    return GNSS_TIME_OK;
}

int inject_nmea_locked(gnss_time_source_t *source, const char *sentence,
                       uint64_t arrival_monotonic_ns)
{
    if (!sentence)
        return GNSS_TIME_ERR_ARGUMENT;
    const std::string frame(sentence);
    if (!is_rmc_sentence(frame)) {
        ++source->status.ignored_nmea_events;
        return GNSS_TIME_OK;
    }
    ++source->status.rmc_events;
    source->status.last_rmc_arrival_monotonic_ns = arrival_monotonic_ns;
    if (!source->status.last_pps_id || !arrival_monotonic_ns ||
        arrival_monotonic_ns < source->status.last_pps_monotonic_ns ||
        arrival_monotonic_ns - source->status.last_pps_monotonic_ns >
            static_cast<uint64_t>(source->config.max_rmc_delay_ms) *
                1000000ULL) {
        ++source->status.unpaired_rmc_events;
        set_error_locked(&source->status, GNSS_TIME_ERR_NOT_LOCKED, 0);
        return GNSS_TIME_ERR_NOT_LOCKED;
    }
    const int result = time_sync_on_nmea_rmc(
        source->time_sync, source->status.last_pps_id, sentence);
    if (result == TIME_SYNC_ERR_NMEA)
        ++source->status.invalid_rmc_events;
    if (result != TIME_SYNC_OK) {
        set_error_locked(&source->status, result, 0);
        return result;
    }
    source->status.last_error = GNSS_TIME_OK;
    source->status.last_errno = 0;
    return GNSS_TIME_OK;
}

void consume_uart_bytes(gnss_time_source_t *source, const char *bytes,
                        size_t length, uint64_t test_arrival_monotonic_ns = 0)
{
    std::lock_guard<std::mutex> lock(source->mutex);
    source->nmea_buffer.append(bytes, length);
    /* BREAK-based PPS: the termios PARMRK delivers a line break as the
       3-byte sequence "\377\0\0".  Treat each occurrence as one PPS edge
       timestamped at its arrival. */
    for (;;) {
        const size_t mark = source->nmea_buffer.find("\377\0\0", 0, 3);
        if (mark == std::string::npos)
            break;
        source->nmea_buffer.erase(mark, 3);
        uint64_t arrival_ns = test_arrival_monotonic_ns;
        if (!arrival_ns && !monotonic_now_ns(&arrival_ns)) {
            ++source->status.time_mapping_errors;
            continue;
        }
        uint64_t realtime_ns = 0;
        struct timespec rt;
        if (clock_gettime(CLOCK_REALTIME, &rt) == 0) {
            realtime_ns = static_cast<uint64_t>(rt.tv_sec) *
                              1000000000ULL +
                          static_cast<uint64_t>(rt.tv_nsec);
        }
        inject_pps_locked(source, 0, realtime_ns, arrival_ns);
    }
    if (source->nmea_buffer.size() > kMaximumNmeaBuffer) {
        const size_t last_dollar = source->nmea_buffer.rfind('$');
        if (last_dollar == std::string::npos)
            source->nmea_buffer.clear();
        else
            source->nmea_buffer.erase(0, last_dollar);
        ++source->status.invalid_rmc_events;
    }
    for (;;) {
        const size_t newline = source->nmea_buffer.find('\n');
        if (newline == std::string::npos)
            break;
        std::string sentence = source->nmea_buffer.substr(0, newline);
        source->nmea_buffer.erase(0, newline + 1);
        while (!sentence.empty() &&
               (sentence.back() == '\r' || sentence.back() == '\n'))
            sentence.pop_back();
        const size_t dollar = sentence.find('$');
        if (dollar != std::string::npos)
            sentence.erase(0, dollar);
        if (sentence.empty())
            continue;
        if (sentence.size() > kMaximumNmeaSentence) {
            ++source->status.invalid_rmc_events;
            continue;
        }
        uint64_t arrival_ns = test_arrival_monotonic_ns;
        if (!arrival_ns && !monotonic_now_ns(&arrival_ns)) {
            ++source->status.time_mapping_errors;
            continue;
        }
        inject_nmea_locked(source, sentence.c_str(), arrival_ns);
    }
}

void uart_worker(gnss_time_source_t *source)
{
    while (!source->stop_requested.load()) {
        const int fd = open(source->uart_device.c_str(),
                            O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            {
                std::lock_guard<std::mutex> lock(source->mutex);
                ++source->status.uart_open_errors;
                set_error_locked(&source->status, GNSS_TIME_ERR_IO, errno);
            }
            if (!wait_for_retry(source))
                break;
            continue;
        }
        const int setup_result = configure_uart(fd, source->config.uart_baud);
        if (setup_result != GNSS_TIME_OK) {
            const int saved_errno = errno;
            close(fd);
            {
                std::lock_guard<std::mutex> lock(source->mutex);
                ++source->status.uart_open_errors;
                set_error_locked(&source->status, setup_result, saved_errno);
            }
            if (!wait_for_retry(source))
                break;
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(source->mutex);
            source->status.uart_connected = 1;
        }
        while (!source->stop_requested.load()) {
            struct pollfd descriptor = {fd, POLLIN, 0};
            const int poll_result = poll(&descriptor, 1, 250);
            if (poll_result < 0) {
                if (errno == EINTR)
                    continue;
                std::lock_guard<std::mutex> lock(source->mutex);
                ++source->status.uart_read_errors;
                set_error_locked(&source->status, GNSS_TIME_ERR_IO, errno);
                break;
            }
            if (poll_result == 0)
                continue;
            if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
                break;
            char buffer[512];
            const ssize_t received = read(fd, buffer, sizeof(buffer));
            if (received > 0) {
                consume_uart_bytes(source, buffer,
                                   static_cast<size_t>(received));
            } else if (received < 0 && errno != EAGAIN && errno != EINTR) {
                std::lock_guard<std::mutex> lock(source->mutex);
                ++source->status.uart_read_errors;
                set_error_locked(&source->status, GNSS_TIME_ERR_IO, errno);
                break;
            }
        }
        close(fd);
        {
            std::lock_guard<std::mutex> lock(source->mutex);
            source->status.uart_connected = 0;
        }
        if (!source->stop_requested.load())
            wait_for_retry(source);
    }
}

int configure_pps(int fd)
{
    int capabilities = 0;
    if (ioctl(fd, PPS_GETCAP, &capabilities) < 0 ||
        !(capabilities & PPS_CAPTUREASSERT) ||
        !(capabilities & PPS_TSFMT_TSPEC)) {
        return GNSS_TIME_ERR_PPS;
    }
    struct pps_kparams parameters = {};
    parameters.api_version = PPS_API_VERS;
    parameters.mode = PPS_CAPTUREASSERT | PPS_TSFMT_TSPEC;
    if (ioctl(fd, PPS_SETPARAMS, &parameters) < 0)
        return GNSS_TIME_ERR_PPS;
    return GNSS_TIME_OK;
}

void pps_worker(gnss_time_source_t *source)
{
    uint32_t last_kernel_sequence = 0;
    while (!source->stop_requested.load()) {
        const int fd =
            open(source->pps_device.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            {
                std::lock_guard<std::mutex> lock(source->mutex);
                ++source->status.pps_open_errors;
                set_error_locked(&source->status, GNSS_TIME_ERR_PPS, errno);
            }
            if (!wait_for_retry(source))
                break;
            continue;
        }
        const int setup_result = configure_pps(fd);
        if (setup_result != GNSS_TIME_OK) {
            const int saved_errno = errno;
            close(fd);
            {
                std::lock_guard<std::mutex> lock(source->mutex);
                ++source->status.pps_open_errors;
                set_error_locked(&source->status, setup_result, saved_errno);
            }
            if (!wait_for_retry(source))
                break;
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(source->mutex);
            source->status.pps_connected = 1;
        }
        while (!source->stop_requested.load()) {
            struct pps_fdata data = {};
            data.timeout.sec = 0;
            data.timeout.nsec = 250000000;
            data.timeout.flags = 0;
            if (ioctl(fd, PPS_FETCH, &data) < 0) {
                if (errno == EINTR || errno == ETIMEDOUT)
                    continue;
                std::lock_guard<std::mutex> lock(source->mutex);
                ++source->status.pps_fetch_errors;
                set_error_locked(&source->status, GNSS_TIME_ERR_PPS, errno);
                break;
            }
            if (!data.info.assert_sequence ||
                data.info.assert_sequence == last_kernel_sequence)
                continue;
            const uint64_t local_realtime_ns =
                pps_time_ns(data.info.assert_tu);
            int64_t clock_offset_ns = 0;
            uint64_t sample_span_ns = 0;
            uint64_t monotonic_ns = 0;
            if (!local_realtime_ns ||
                !sample_realtime_minus_monotonic(&clock_offset_ns,
                                                  &sample_span_ns) ||
                !realtime_to_monotonic(local_realtime_ns, clock_offset_ns,
                                       &monotonic_ns)) {
                std::lock_guard<std::mutex> lock(source->mutex);
                ++source->status.time_mapping_errors;
                set_error_locked(&source->status, GNSS_TIME_ERR_PPS, ERANGE);
                continue;
            }
            last_kernel_sequence = data.info.assert_sequence;
            std::lock_guard<std::mutex> lock(source->mutex);
            source->status.last_clock_sample_span_ns = sample_span_ns;
            inject_pps_locked(source, data.info.assert_sequence,
                              local_realtime_ns, monotonic_ns);
        }
        close(fd);
        {
            std::lock_guard<std::mutex> lock(source->mutex);
            source->status.pps_connected = 0;
        }
        if (!source->stop_requested.load())
            wait_for_retry(source);
    }
}

}  // namespace

extern "C" void
gnss_time_source_default_config(gnss_time_source_config_t *config)
{
    if (!config)
        return;
    std::memset(config, 0, sizeof(*config));
    config->uart_baud = kDefaultBaud;
    config->max_rmc_delay_ms = kDefaultMaxRmcDelayMs;
}

extern "C" int gnss_time_source_create(
    const gnss_time_source_config_t *config, time_sync_service_t *time_sync,
    gnss_time_source_t **source_out)
{
    if (!config || !time_sync || !source_out || !config->uart_baud ||
        !config->max_rmc_delay_ms || config->max_rmc_delay_ms > 5000)
        return GNSS_TIME_ERR_ARGUMENT;
    *source_out = nullptr;
    speed_t ignored = 0;
    if (!baud_to_termios(config->uart_baud, &ignored))
        return GNSS_TIME_ERR_BAUD;
    gnss_time_source_t *source = new (std::nothrow) gnss_time_source;
    if (!source)
        return GNSS_TIME_ERR_ALLOCATE;
    source->time_sync = time_sync;
    source->config = *config;
    source->uart_device = config->uart_device ? config->uart_device : "";
    source->pps_device = config->pps_device ? config->pps_device : "";
    source->config.uart_device = source->uart_device.c_str();
    source->config.pps_device = source->pps_device.c_str();
    source->status.uart_baud = config->uart_baud;
    source->status.max_rmc_delay_ms = config->max_rmc_delay_ms;
    std::snprintf(source->status.uart_device,
                  sizeof(source->status.uart_device), "%s",
                  source->uart_device.c_str());
    std::snprintf(source->status.pps_device,
                  sizeof(source->status.pps_device), "%s",
                  source->pps_device.c_str());
    *source_out = source;
    return GNSS_TIME_OK;
}

extern "C" void gnss_time_source_destroy(gnss_time_source_t *source)
{
    if (!source)
        return;
    gnss_time_source_stop(source);
    delete source;
}

extern "C" int gnss_time_source_start(gnss_time_source_t *source)
{
    /* An empty uart_device means "feed-only" mode: the NMEA sentences are
       injected by an external dispatcher (e.g. the shared XDAS+GNSS UART)
       through gnss_time_source_inject_nmea().  Only the PPS worker runs. */
    if (!source || source->pps_device.empty())
        return GNSS_TIME_ERR_ARGUMENT;
    {
        std::lock_guard<std::mutex> lock(source->mutex);
        if (source->status.running)
            return GNSS_TIME_OK;
        source->stop_requested.store(false);
        source->status.running = 1;
    }
    try {
        if (!source->uart_device.empty())
            source->uart_thread = std::thread(uart_worker, source);
        source->pps_thread = std::thread(pps_worker, source);
    } catch (...) {
        source->stop_requested.store(true);
        if (source->uart_thread.joinable())
            source->uart_thread.join();
        if (source->pps_thread.joinable())
            source->pps_thread.join();
        std::lock_guard<std::mutex> lock(source->mutex);
        source->status.running = 0;
        set_error_locked(&source->status, GNSS_TIME_ERR_THREAD, 0);
        return GNSS_TIME_ERR_THREAD;
    }
    return GNSS_TIME_OK;
}

extern "C" int gnss_time_source_stop(gnss_time_source_t *source)
{
    if (!source)
        return GNSS_TIME_ERR_ARGUMENT;
    source->stop_requested.store(true);
    if (source->uart_thread.joinable())
        source->uart_thread.join();
    if (source->pps_thread.joinable())
        source->pps_thread.join();
    std::lock_guard<std::mutex> lock(source->mutex);
    source->status.running = 0;
    source->status.uart_connected = 0;
    source->status.pps_connected = 0;
    return GNSS_TIME_OK;
}

extern "C" int gnss_time_source_get_status(
    gnss_time_source_t *source, gnss_time_source_status_t *status)
{
    if (!source || !status)
        return GNSS_TIME_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(source->mutex);
    *status = source->status;
    return GNSS_TIME_OK;
}

extern "C" int gnss_time_source_resolve_monotonic_ns(
    gnss_time_source_t *source, uint64_t monotonic_ns,
    time_sync_resolution_t *resolution)
{
    if (!source || !monotonic_ns || !resolution)
        return GNSS_TIME_ERR_ARGUMENT;
    uint64_t pps_id = 0;
    {
        std::lock_guard<std::mutex> lock(source->mutex);
        pps_id = source->status.last_pps_id;
    }
    if (!pps_id)
        return GNSS_TIME_ERR_NOT_LOCKED;
    const int result = time_sync_resolve_tick(source->time_sync, pps_id,
                                              monotonic_ns, resolution);
    return result == TIME_SYNC_OK ? GNSS_TIME_OK : result;
}

extern "C" int gnss_time_source_trigger_utc_ns(
    gnss_time_source_t *source, uint64_t frame_monotonic_ns,
    uint64_t *trigger_utc_ns)
{
    constexpr uint64_t kNsPerSecond = 1000000000ULL;
    if (!source || !frame_monotonic_ns || !trigger_utc_ns)
        return GNSS_TIME_ERR_ARGUMENT;
    time_sync_status_t status = {};
    const int result = time_sync_get_status(source->time_sync, &status);
    if (result != TIME_SYNC_OK || !status.utc_valid ||
        !status.reference_utc_ns || frame_monotonic_ns < status.reference_tick)
        return GNSS_TIME_ERR_NOT_LOCKED;
    const uint64_t seconds_since_reference =
        (frame_monotonic_ns - status.reference_tick) / kNsPerSecond;
    *trigger_utc_ns = status.reference_utc_ns +
                      seconds_since_reference * kNsPerSecond;
    return GNSS_TIME_OK;
}

extern "C" int gnss_time_source_inject_pps(
    gnss_time_source_t *source, uint32_t kernel_sequence,
    uint64_t local_realtime_ns, uint64_t monotonic_ns)
{
    if (!source || !kernel_sequence || !local_realtime_ns || !monotonic_ns)
        return GNSS_TIME_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(source->mutex);
    return inject_pps_locked(source, kernel_sequence, local_realtime_ns,
                             monotonic_ns);
}

extern "C" int gnss_time_source_inject_nmea(
    gnss_time_source_t *source, const char *sentence,
    uint64_t arrival_monotonic_ns)
{
    if (!source || !sentence || !arrival_monotonic_ns)
        return GNSS_TIME_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(source->mutex);
    return inject_nmea_locked(source, sentence, arrival_monotonic_ns);
}

extern "C" int gnss_time_source_self_test(char *report,
                                            size_t report_capacity)
{
    if (!report || !report_capacity)
        return GNSS_TIME_ERR_ARGUMENT;
    report[0] = '\0';
    time_sync_config_t time_config = {};
    time_sync_default_config(&time_config);
    time_config.timer_frequency_hz = 1000000000ULL;
    time_config.max_holdover_pps = 2;
    time_sync_service_t *time_sync = nullptr;
    if (time_sync_create(&time_config, &time_sync) != TIME_SYNC_OK)
        return GNSS_TIME_ERR_ALLOCATE;
    gnss_time_source_config_t config = {};
    gnss_time_source_default_config(&config);
    gnss_time_source_t *source = nullptr;
    int result = gnss_time_source_create(&config, time_sync, &source);
    const std::string rmc = make_nmea(
        "GPRMC,123519.00,A,4807.038,N,01131.000,E,0.0,0.0,230394,,,A");
    int64_t utc_sec = 0;
    int valid = 0;
    if (result == GNSS_TIME_OK)
        result = time_sync_parse_nmea_rmc(rmc.c_str(), &utc_sec, &valid);
    if (result == GNSS_TIME_OK)
        result = gnss_time_source_inject_pps(source, 100, 9000000000ULL,
                                             1000000000ULL);
    if (result == GNSS_TIME_OK) {
        const std::string first = "ignored-prefix" + rmc.substr(0, 9);
        const std::string second = rmc.substr(9) + "\r\n";
        consume_uart_bytes(source, first.data(), first.size(), 1100000000ULL);
        consume_uart_bytes(source, second.data(), second.size(), 1100000000ULL);
    }
    if (result == GNSS_TIME_OK)
        result = gnss_time_source_inject_pps(source, 101, 10000000000ULL,
                                             2000000000ULL);
    time_sync_resolution_t resolution = {};
    if (result == GNSS_TIME_OK)
        result = gnss_time_source_resolve_monotonic_ns(
            source, 2250000000ULL, &resolution);
    const uint64_t expected_ns =
        static_cast<uint64_t>(utc_sec + 1) * 1000000000ULL + 250000000ULL;
    gnss_time_source_status_t status = {};
    if (result == GNSS_TIME_OK)
        result = gnss_time_source_get_status(source, &status);
    std::string damaged = rmc;
    if (!damaged.empty())
        damaged.back() = damaged.back() == '0' ? '1' : '0';
    const int damaged_result = gnss_time_source_inject_nmea(
        source, damaged.c_str(), 2100000000ULL);
    const int unpaired_result = gnss_time_source_inject_nmea(
        source, rmc.c_str(), 3000000001ULL);
    const bool passed =
        result == GNSS_TIME_OK && valid && resolution.valid &&
        resolution.utc_ns == expected_ns && resolution.pps_id == 2 &&
        damaged_result == TIME_SYNC_ERR_NMEA &&
        unpaired_result == GNSS_TIME_ERR_NOT_LOCKED &&
        gnss_time_source_get_status(source, &status) == GNSS_TIME_OK &&
        status.pps_events == 2 && status.rmc_events == 3 &&
        status.invalid_rmc_events == 1 && status.unpaired_rmc_events == 1;
    std::snprintf(report, report_capacity,
                  "direct_gprmc_115200=%s fragmented=verified pps=%llu rmc=%llu invalid=%llu unpaired=%llu utc_ns=%llu",
                  passed ? "verified" : "failed",
                  static_cast<unsigned long long>(status.pps_events),
                  static_cast<unsigned long long>(status.rmc_events),
                  static_cast<unsigned long long>(status.invalid_rmc_events),
                  static_cast<unsigned long long>(status.unpaired_rmc_events),
                  static_cast<unsigned long long>(resolution.utc_ns));
    gnss_time_source_destroy(source);
    time_sync_destroy(time_sync);
    return passed ? GNSS_TIME_OK : GNSS_TIME_ERR_NOT_LOCKED;
}

extern "C" const char *gnss_time_source_strerror(int result)
{
    switch (result) {
    case GNSS_TIME_OK:
        return "success";
    case GNSS_TIME_ERR_ARGUMENT:
        return "invalid GNSS time source argument";
    case GNSS_TIME_ERR_ALLOCATE:
        return "unable to allocate GNSS time source";
    case GNSS_TIME_ERR_THREAD:
        return "unable to start GNSS time source threads";
    case GNSS_TIME_ERR_IO:
        return "GNSS UART input/output error";
    case GNSS_TIME_ERR_BAUD:
        return "unsupported GNSS UART baud rate";
    case GNSS_TIME_ERR_PPS:
        return "Linux PPS device error";
    case GNSS_TIME_ERR_NOT_LOCKED:
        return "GNSS UTC is not locked to PPS";
    default:
        return time_sync_strerror(result);
    }
}
