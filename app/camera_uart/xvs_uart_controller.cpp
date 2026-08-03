#include "xvs_uart_controller.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
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

constexpr int kResponseTimeoutMs = 1500;
constexpr size_t kFrameMax = 512;

struct ParsedResponse {
    bool ack = false;
    uint32_t sequence = 0;
    std::string command;
    std::string fields;
};

uint16_t crc16_ccitt(const char *data, size_t size)
{
    uint16_t crc = 0xffff;
    for (size_t index = 0; index < size; ++index) {
        crc ^= static_cast<uint16_t>(
                   static_cast<unsigned char>(data[index]))
               << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

bool parse_u32(const std::string &text, uint32_t *value)
{
    if (!value || text.empty() || text[0] == '-')
        return false;
    errno = 0;
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
    if (errno || end == text.c_str() || *end != '\0' || parsed > UINT32_MAX)
        return false;
    *value = static_cast<uint32_t>(parsed);
    return true;
}

bool parse_u64(const std::string &text, uint64_t *value)
{
    if (!value || text.empty() || text[0] == '-')
        return false;
    errno = 0;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (errno || end == text.c_str() || *end != '\0')
        return false;
    *value = static_cast<uint64_t>(parsed);
    return true;
}

bool parse_i64(const std::string &text, int64_t *value)
{
    if (!value || text.empty())
        return false;
    errno = 0;
    char *end = nullptr;
    const long long parsed = std::strtoll(text.c_str(), &end, 10);
    if (errno || end == text.c_str() || *end != '\0')
        return false;
    *value = static_cast<int64_t>(parsed);
    return true;
}

std::string add_crc_and_terminator(const std::string &payload)
{
    char suffix[16];
    const uint16_t crc = crc16_ccitt(payload.data(), payload.size());
    std::snprintf(suffix, sizeof(suffix), "*%04X\r\n", crc);
    return "$" + payload + suffix;
}

bool parse_checked_frame(const std::string &frame, std::string *payload)
{
    if (!payload || frame.size() < 8 || frame[0] != '$')
        return false;
    const size_t star = frame.rfind('*');
    if (star == std::string::npos || star + 5 != frame.size())
        return false;
    errno = 0;
    char *end = nullptr;
    const unsigned long received =
        std::strtoul(frame.substr(star + 1).c_str(), &end, 16);
    if (errno || !end || *end != '\0' || received > UINT16_MAX)
        return false;
    *payload = frame.substr(1, star - 1);
    return crc16_ccitt(payload->data(), payload->size()) == received;
}

bool split_first(std::string *input, std::string *field)
{
    if (!input || !field)
        return false;
    const size_t comma = input->find(',');
    if (comma == std::string::npos) {
        *field = *input;
        input->clear();
    } else {
        *field = input->substr(0, comma);
        input->erase(0, comma + 1);
    }
    return !field->empty();
}

bool parse_response(const std::string &frame, ParsedResponse *response)
{
    if (!response)
        return false;
    std::string payload;
    if (!parse_checked_frame(frame, &payload))
        return false;
    std::string kind;
    std::string sequence_text;
    if (!split_first(&payload, &kind) ||
        !split_first(&payload, &sequence_text) ||
        !split_first(&payload, &response->command) ||
        !parse_u32(sequence_text, &response->sequence)) {
        return false;
    }
    if (kind == "ACK")
        response->ack = true;
    else if (kind == "NACK")
        response->ack = false;
    else
        return false;
    response->fields = payload;
    return true;
}

bool split_event_fields(const std::string &payload,
                        std::string *event_name, std::string *remaining)
{
    if (!event_name || !remaining)
        return false;
    *remaining = payload;
    std::string kind;
    return split_first(remaining, &kind) && kind == "EVT" &&
           split_first(remaining, event_name);
}

bool parse_event(const std::string &frame, xvs_uart_event_t *event)
{
    if (!event)
        return false;
    std::memset(event, 0, sizeof(*event));
    std::string payload;
    if (!parse_checked_frame(frame, &payload))
        return false;
    std::string event_name;
    std::string remaining;
    if (!split_event_fields(payload, &event_name, &remaining))
        return false;

    std::string first;
    std::string second;
    std::string third;
    if (event_name == "PPS") {
        if (!split_first(&remaining, &first) ||
            !split_first(&remaining, &second) || !remaining.empty() ||
            !parse_u64(first, &event->pps_id) ||
            !parse_u64(second, &event->timer_tick)) {
            return false;
        }
        event->type = XVS_UART_EVENT_PPS;
        return true;
    }
    if (event_name == "RMC") {
        uint32_t valid = 0;
        if (!split_first(&remaining, &first) ||
            !split_first(&remaining, &second) ||
            !split_first(&remaining, &third) || !remaining.empty() ||
            !parse_u64(first, &event->pps_id) ||
            !parse_i64(second, &event->utc_sec) ||
            !parse_u32(third, &valid) || valid > 1) {
            return false;
        }
        event->type = XVS_UART_EVENT_RMC;
        event->valid = static_cast<int>(valid);
        return true;
    }
    if (event_name == "NMEA") {
        if (!split_first(&remaining, &first) ||
            !parse_u64(first, &event->pps_id) || remaining.empty() ||
            remaining.size() >= sizeof(event->nmea) || remaining[0] != '$') {
            return false;
        }
        event->type = XVS_UART_EVENT_NMEA;
        std::snprintf(event->nmea, sizeof(event->nmea), "%s",
                      remaining.c_str());
        return true;
    }
    if (event_name == "XVS") {
        if (!split_first(&remaining, &first) ||
            !split_first(&remaining, &second) ||
            !split_first(&remaining, &third) || !remaining.empty() ||
            !parse_u64(first, &event->trigger_id) ||
            !parse_u64(second, &event->pps_id) ||
            !parse_u64(third, &event->timer_tick)) {
            return false;
        }
        event->type = XVS_UART_EVENT_XVS;
        return true;
    }
    return false;
}

bool find_field(const std::string &fields, const char *name,
                std::string *value)
{
    if (!name || !value)
        return false;
    const std::string prefix = std::string(name) + '=';
    size_t start = 0;
    while (start <= fields.size()) {
        const size_t end = fields.find(',', start);
        const std::string field = fields.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (field.compare(0, prefix.size(), prefix) == 0) {
            *value = field.substr(prefix.size());
            return true;
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return false;
}

uint64_t clock_ns(clockid_t clock_id)
{
    struct timespec now = {};
    if (clock_gettime(clock_id, &now) != 0)
        return 0;
    return static_cast<uint64_t>(now.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(now.tv_nsec);
}

int configure_uart(int fd, struct termios *saved)
{
    if (tcgetattr(fd, saved) < 0)
        return XVS_UART_ERR_CONFIGURE;
    struct termios tio = *saved;
    cfmakeraw(&tio);
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    tio.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
    tio.c_cflag |= CS8 | CLOCAL | CREAD;
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 1;
    if (tcsetattr(fd, TCSANOW, &tio) < 0 || tcflush(fd, TCIOFLUSH) < 0)
        return XVS_UART_ERR_CONFIGURE;
    return XVS_UART_OK;
}

int write_all(int fd, const char *data, size_t size)
{
    size_t written = 0;
    while (written < size) {
        const ssize_t count = write(fd, data + written, size - written);
        if (count > 0) {
            written += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = {fd, POLLOUT, 0};
            if (poll(&pfd, 1, kResponseTimeoutMs) > 0)
                continue;
        }
        return XVS_UART_ERR_IO;
    }
    return XVS_UART_OK;
}

}  // namespace

struct xvs_uart_controller {
    int fd = -1;
    struct termios saved = {};
    bool saved_valid = false;
    std::string device;
    std::atomic<bool> stop_reader{false};
    std::thread reader_thread;

    std::mutex command_mutex;
    uint32_t next_sequence = 1;
    std::mutex response_mutex;
    std::condition_variable response_condition;
    bool response_waiting = false;
    bool response_ready = false;
    uint32_t expected_sequence = 0;
    int reader_error = XVS_UART_OK;
    ParsedResponse response;

    std::mutex callback_mutex;
    xvs_uart_event_callback_t event_callback = nullptr;
    void *event_user_data = nullptr;
};

namespace {

void set_reader_error(xvs_uart_controller_t *controller, int result)
{
    std::lock_guard<std::mutex> lock(controller->response_mutex);
    if (controller->reader_error == XVS_UART_OK)
        controller->reader_error = result;
    controller->response_condition.notify_all();
}

void dispatch_line(xvs_uart_controller_t *controller, const std::string &line)
{
    ParsedResponse response;
    if (parse_response(line, &response)) {
        std::lock_guard<std::mutex> lock(controller->response_mutex);
        if (controller->response_waiting &&
            response.sequence == controller->expected_sequence) {
            controller->response = response;
            controller->response_ready = true;
            controller->response_condition.notify_all();
        }
        return;
    }

    xvs_uart_event_t event = {};
    if (!parse_event(line, &event))
        return;
    event.uart_receive_monotonic_ns = clock_ns(CLOCK_MONOTONIC);
    event.uart_receive_realtime_ns = clock_ns(CLOCK_REALTIME);
    xvs_uart_event_callback_t callback = nullptr;
    void *user_data = nullptr;
    {
        std::lock_guard<std::mutex> lock(controller->callback_mutex);
        callback = controller->event_callback;
        user_data = controller->event_user_data;
    }
    if (callback)
        callback(&event, user_data);
}

void reader_main(xvs_uart_controller_t *controller)
{
    std::string line;
    while (!controller->stop_reader.load()) {
        struct pollfd pfd = {controller->fd, POLLIN, 0};
        const int ready = poll(&pfd, 1, 100);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            set_reader_error(controller, XVS_UART_ERR_IO);
            return;
        }
        if (ready == 0)
            continue;
        if (pfd.revents & POLLIN) {
            char buffer[128];
            const ssize_t count = read(controller->fd, buffer, sizeof(buffer));
            if (count < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                set_reader_error(controller, XVS_UART_ERR_IO);
                return;
            }
            for (ssize_t index = 0; index < count; ++index) {
                const char byte = buffer[index];
                if (byte == '\r')
                    continue;
                if (byte == '\n') {
                    if (!line.empty())
                        dispatch_line(controller, line);
                    line.clear();
                    continue;
                }
                if (line.size() + 1 >= kFrameMax) {
                    line.clear();
                    continue;
                }
                line.push_back(byte);
            }
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            set_reader_error(controller, XVS_UART_ERR_IO);
            return;
        }
    }
}

int transact(xvs_uart_controller_t *controller, const std::string &command,
             ParsedResponse *response)
{
    if (!controller || controller->fd < 0 || !response)
        return XVS_UART_ERR_ARGUMENT;
    std::lock_guard<std::mutex> command_lock(controller->command_mutex);
    const uint32_t sequence = controller->next_sequence++;
    {
        std::lock_guard<std::mutex> lock(controller->response_mutex);
        if (controller->reader_error != XVS_UART_OK)
            return controller->reader_error;
        controller->response_waiting = true;
        controller->response_ready = false;
        controller->expected_sequence = sequence;
        controller->response = {};
    }

    const std::string request = add_crc_and_terminator(
        "XVS," + std::to_string(sequence) + "," + command);
    int result = write_all(controller->fd, request.data(), request.size());
    if (result == XVS_UART_OK && tcdrain(controller->fd) < 0)
        result = XVS_UART_ERR_IO;
    if (result != XVS_UART_OK) {
        std::lock_guard<std::mutex> lock(controller->response_mutex);
        controller->response_waiting = false;
        return result;
    }

    std::unique_lock<std::mutex> lock(controller->response_mutex);
    const bool signaled = controller->response_condition.wait_for(
        lock, std::chrono::milliseconds(kResponseTimeoutMs), [&] {
            return controller->response_ready ||
                   controller->reader_error != XVS_UART_OK ||
                   controller->stop_reader.load();
        });
    controller->response_waiting = false;
    if (!signaled)
        return XVS_UART_ERR_TIMEOUT;
    if (controller->reader_error != XVS_UART_OK)
        return controller->reader_error;
    if (!controller->response_ready)
        return XVS_UART_ERR_IO;
    *response = controller->response;
    return response->ack ? XVS_UART_OK : XVS_UART_ERR_MCU;
}

int simple_command(xvs_uart_controller_t *controller,
                   const std::string &command, const char *expected)
{
    ParsedResponse response;
    const int result = transact(controller, command, &response);
    if (result != XVS_UART_OK)
        return result;
    return response.command == expected ? XVS_UART_OK
                                        : XVS_UART_ERR_PROTOCOL;
}

}  // namespace

extern "C" int xvs_uart_create(const char *device,
                               xvs_uart_controller_t **controller_out)
{
    if (!device || !*device || !controller_out)
        return XVS_UART_ERR_ARGUMENT;
    *controller_out = nullptr;
    xvs_uart_controller_t *controller =
        new (std::nothrow) xvs_uart_controller_t;
    if (!controller)
        return XVS_UART_ERR_IO;
    controller->device = device;
    controller->fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (controller->fd < 0) {
        delete controller;
        return XVS_UART_ERR_OPEN;
    }
    if (ioctl(controller->fd, TIOCEXCL) < 0) {
        close(controller->fd);
        delete controller;
        return XVS_UART_ERR_OPEN;
    }
    const int result = configure_uart(controller->fd, &controller->saved);
    if (result != XVS_UART_OK) {
        close(controller->fd);
        delete controller;
        return result;
    }
    controller->saved_valid = true;
    try {
        controller->reader_thread = std::thread(reader_main, controller);
    } catch (...) {
        tcsetattr(controller->fd, TCSANOW, &controller->saved);
        close(controller->fd);
        delete controller;
        return XVS_UART_ERR_IO;
    }
    *controller_out = controller;
    return XVS_UART_OK;
}

extern "C" void xvs_uart_destroy(xvs_uart_controller_t *controller)
{
    if (!controller)
        return;
    controller->stop_reader.store(true);
    controller->response_condition.notify_all();
    if (controller->reader_thread.joinable())
        controller->reader_thread.join();
    if (controller->fd >= 0) {
        if (controller->saved_valid)
            tcsetattr(controller->fd, TCSANOW, &controller->saved);
        close(controller->fd);
    }
    delete controller;
}

extern "C" int xvs_uart_set_event_callback(
    xvs_uart_controller_t *controller, xvs_uart_event_callback_t callback,
    void *user_data)
{
    if (!controller)
        return XVS_UART_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(controller->callback_mutex);
    controller->event_callback = callback;
    controller->event_user_data = user_data;
    return XVS_UART_OK;
}

extern "C" int xvs_uart_ping(xvs_uart_controller_t *controller)
{
    return simple_command(controller, "PING", "PONG");
}

extern "C" int xvs_uart_idle(xvs_uart_controller_t *controller)
{
    return simple_command(controller, "IDLE", "IDLE");
}

extern "C" int xvs_uart_start(xvs_uart_controller_t *controller,
                              uint32_t frequency_hz,
                              uint32_t low_pulse_us)
{
    if ((frequency_hz != 2 && frequency_hz != 4) || !low_pulse_us ||
        low_pulse_us > 1000)
        return XVS_UART_ERR_ARGUMENT;
    return simple_command(
        controller,
        "START," + std::to_string(frequency_hz * 1000U) + "," +
            std::to_string(low_pulse_us),
        "START");
}

extern "C" int xvs_uart_count(xvs_uart_controller_t *controller,
                              uint32_t frequency_hz,
                              uint32_t low_pulse_us,
                              uint32_t pulse_count)
{
    if ((frequency_hz != 2 && frequency_hz != 4) || !low_pulse_us ||
        low_pulse_us > 1000 || !pulse_count)
        return XVS_UART_ERR_ARGUMENT;
    return simple_command(
        controller,
        "COUNT," + std::to_string(frequency_hz * 1000U) + "," +
            std::to_string(low_pulse_us) + "," + std::to_string(pulse_count),
        "COUNT");
}

extern "C" int xvs_uart_stop(xvs_uart_controller_t *controller)
{
    return simple_command(controller, "STOP", "STOP");
}

extern "C" int xvs_uart_get_status(xvs_uart_controller_t *controller,
                                   xvs_uart_status_t *status)
{
    if (!controller || !status)
        return XVS_UART_ERR_ARGUMENT;
    std::memset(status, 0, sizeof(*status));
    status->connected = controller->fd >= 0;
    std::snprintf(status->device, sizeof(status->device), "%s",
                  controller->device.c_str());
    ParsedResponse response;
    const int result = transact(controller, "STATUS", &response);
    if (result != XVS_UART_OK)
        return result;
    if (response.command != "STATUS")
        return XVS_UART_ERR_PROTOCOL;

    std::string state;
    std::string frequency;
    std::string low_us;
    std::string pulse_count;
    std::string trigger_id;
    if (!find_field(response.fields, "STATE", &state) ||
        !find_field(response.fields, "FREQ_MHZ", &frequency) ||
        !find_field(response.fields, "LOW_US", &low_us) ||
        !find_field(response.fields, "PULSE_COUNT", &pulse_count) ||
        !find_field(response.fields, "LAST_TRIGGER_ID", &trigger_id) ||
        state.size() >= sizeof(status->state) ||
        !parse_u32(frequency, &status->frequency_millihz) ||
        !parse_u32(low_us, &status->low_pulse_us) ||
        !parse_u64(pulse_count, &status->pulse_count) ||
        !parse_u64(trigger_id, &status->last_trigger_id)) {
        return XVS_UART_ERR_PROTOCOL;
    }
    std::snprintf(status->state, sizeof(status->state), "%s", state.c_str());
    status->valid = 1;
    return XVS_UART_OK;
}

extern "C" int xvs_uart_protocol_self_test(void)
{
    const std::string request_payload = "XVS,7,START,4000,10";
    const std::string request = add_crc_and_terminator(request_payload);
    std::string checked_payload;
    if (!parse_checked_frame(request.substr(0, request.size() - 2),
                             &checked_payload) ||
        checked_payload != request_payload)
        return XVS_UART_ERR_PROTOCOL;

    const std::string response_frame = add_crc_and_terminator(
        "ACK,7,STATUS,STATE=IDLE,FREQ_MHZ=4000,LOW_US=10,"
        "PULSE_COUNT=1000,LAST_TRIGGER_ID=1000");
    ParsedResponse response;
    if (!parse_response(response_frame.substr(0, response_frame.size() - 2),
                        &response) ||
        !response.ack || response.sequence != 7 ||
        response.command != "STATUS")
        return XVS_UART_ERR_PROTOCOL;
    std::string value;
    if (!find_field(response.fields, "PULSE_COUNT", &value) ||
        value != "1000")
        return XVS_UART_ERR_PROTOCOL;

    const std::string event_frame =
        add_crc_and_terminator("EVT,XVS,101,20,20500000");
    xvs_uart_event_t event = {};
    if (!parse_event(event_frame.substr(0, event_frame.size() - 2), &event) ||
        event.type != XVS_UART_EVENT_XVS || event.trigger_id != 101 ||
        event.pps_id != 20 || event.timer_tick != 20500000)
        return XVS_UART_ERR_PROTOCOL;
    return XVS_UART_OK;
}

extern "C" const char *xvs_uart_strerror(int result)
{
    switch (result) {
    case XVS_UART_OK:
        return "success";
    case XVS_UART_ERR_ARGUMENT:
        return "invalid argument";
    case XVS_UART_ERR_OPEN:
        return "failed to open UART device";
    case XVS_UART_ERR_CONFIGURE:
        return "failed to configure UART";
    case XVS_UART_ERR_IO:
        return "UART input/output error";
    case XVS_UART_ERR_TIMEOUT:
        return "MCU response timeout";
    case XVS_UART_ERR_PROTOCOL:
        return "invalid MCU protocol response";
    case XVS_UART_ERR_MCU:
        return "MCU rejected command";
    default:
        return "unknown XVS UART error";
    }
}
