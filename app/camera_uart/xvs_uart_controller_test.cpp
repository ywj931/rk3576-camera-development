#include "xvs_uart_controller.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pty.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

uint16_t crc16_ccitt(const std::string &data)
{
    uint16_t crc = 0xffff;
    for (unsigned char byte : data) {
        crc ^= static_cast<uint16_t>(byte) << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

std::string make_frame(const std::string &payload)
{
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "*%04X\r\n", crc16_ccitt(payload));
    return "$" + payload + suffix;
}

std::string make_nmea(const std::string &payload)
{
    unsigned char checksum = 0;
    for (unsigned char byte : payload)
        checksum ^= byte;
    char suffix[8];
    std::snprintf(suffix, sizeof(suffix), "*%02X", checksum);
    return "$" + payload + suffix;
}

bool checked_payload(const std::string &frame, std::string *payload)
{
    if (!payload || frame.empty() || frame[0] != '$')
        return false;
    const size_t star = frame.rfind('*');
    if (star == std::string::npos || star + 5 != frame.size())
        return false;
    const std::string crc_text = frame.substr(star + 1);
    char *end = nullptr;
    errno = 0;
    const unsigned long received = std::strtoul(crc_text.c_str(), &end, 16);
    *payload = frame.substr(1, star - 1);
    return !errno && end && *end == '\0' && received <= UINT16_MAX &&
           crc16_ccitt(*payload) == received;
}

std::vector<std::string> split(const std::string &text)
{
    std::vector<std::string> fields;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        fields.push_back(text.substr(
            start, comma == std::string::npos ? std::string::npos
                                               : comma - start));
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return fields;
}

bool write_all(int fd, const std::string &data)
{
    size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t count = write(fd, data.data() + offset,
                                    data.size() - offset);
        if (count > 0) {
            offset += static_cast<size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

void mock_mcu(int master_fd, std::atomic<bool> *failed)
{
    std::string frame;
    std::string state = "IDLE";
    uint32_t frequency_millihz = 0;
    uint32_t low_pulse_us = 0;
    uint64_t pulse_count = 0;
    uint64_t trigger_id = 0;

    for (;;) {
        char byte = 0;
        const ssize_t count = read(master_fd, &byte, 1);
        if (count == 0 || (count < 0 && errno == EIO))
            break;
        if (count < 0) {
            if (errno == EINTR)
                continue;
            *failed = true;
            break;
        }
        if (byte == '\r')
            continue;
        if (byte != '\n') {
            frame.push_back(byte);
            continue;
        }

        std::string payload;
        const std::vector<std::string> fields =
            checked_payload(frame, &payload) ? split(payload)
                                             : std::vector<std::string>();
        frame.clear();
        if (fields.size() < 3 || fields[0] != "XVS") {
            *failed = true;
            break;
        }

        const std::string &sequence = fields[1];
        const std::string &command = fields[2];
        std::string response;
        if (command == "PING" && fields.size() == 3) {
            if (!write_all(master_fd,
                           make_frame("EVT,PPS,10,10000000"))) {
                *failed = true;
                break;
            }
            response = "ACK," + sequence + ",PONG";
        } else if (command == "IDLE" && fields.size() == 3) {
            const std::string nmea = make_nmea(
                "GNRMC,123519.00,A,4807.038,N,01131.000,E,0.0,0.0,"
                "230394,,,A");
            if (!write_all(master_fd,
                           make_frame("EVT,NMEA,10," + nmea))) {
                *failed = true;
                break;
            }
            state = "IDLE";
            response = "ACK," + sequence + ",IDLE";
        } else if (command == "START" && fields.size() == 5) {
            if (!write_all(master_fd,
                           make_frame("EVT,PPS,11,11000000")) ||
                !write_all(master_fd,
                           make_frame("EVT,XVS,101,11,11250000"))) {
                *failed = true;
                break;
            }
            frequency_millihz = std::strtoul(fields[3].c_str(), nullptr, 10);
            low_pulse_us = std::strtoul(fields[4].c_str(), nullptr, 10);
            state = "RUNNING";
            response = "ACK," + sequence + ",START";
        } else if (command == "COUNT" && fields.size() == 6) {
            if (!write_all(master_fd,
                           make_frame("EVT,RMC,11,764426120,1"))) {
                *failed = true;
                break;
            }
            frequency_millihz = std::strtoul(fields[3].c_str(), nullptr, 10);
            low_pulse_us = std::strtoul(fields[4].c_str(), nullptr, 10);
            const uint64_t requested =
                std::strtoull(fields[5].c_str(), nullptr, 10);
            pulse_count += requested;
            trigger_id += requested;
            state = "COUNTING";
            response = "ACK," + sequence + ",COUNT";
        } else if (command == "STOP" && fields.size() == 3) {
            state = "IDLE";
            response = "ACK," + sequence + ",STOP";
        } else if (command == "STATUS" && fields.size() == 3) {
            response = "ACK," + sequence + ",STATUS,STATE=" + state +
                       ",FREQ_MHZ=" + std::to_string(frequency_millihz) +
                       ",LOW_US=" + std::to_string(low_pulse_us) +
                       ",PULSE_COUNT=" + std::to_string(pulse_count) +
                       ",LAST_TRIGGER_ID=" + std::to_string(trigger_id);
        } else {
            response = "NACK," + sequence + "," + command +
                       ",ERROR=BAD_COMMAND";
        }
        if (!write_all(master_fd, make_frame(response))) {
            *failed = true;
            break;
        }
    }
}

bool expect_ok(const char *operation, int result)
{
    if (result == XVS_UART_OK)
        return true;
    std::fprintf(stderr, "%s failed: %d (%s)\n", operation, result,
                 xvs_uart_strerror(result));
    return false;
}

struct EventCounters {
    std::atomic<uint32_t> pps{0};
    std::atomic<uint32_t> rmc{0};
    std::atomic<uint32_t> nmea{0};
    std::atomic<uint32_t> xvs{0};
    std::atomic<uint64_t> last_trigger_id{0};
};

void on_event(const xvs_uart_event_t *event, void *user_data)
{
    EventCounters *counters = static_cast<EventCounters *>(user_data);
    if (!event || !counters)
        return;
    if (event->type == XVS_UART_EVENT_PPS)
        ++counters->pps;
    else if (event->type == XVS_UART_EVENT_RMC)
        ++counters->rmc;
    else if (event->type == XVS_UART_EVENT_NMEA)
        ++counters->nmea;
    else if (event->type == XVS_UART_EVENT_XVS) {
        ++counters->xvs;
        counters->last_trigger_id.store(event->trigger_id);
    }
}

}  // namespace

int main()
{
    if (!expect_ok("protocol self-test", xvs_uart_protocol_self_test()))
        return EXIT_FAILURE;

    int master_fd = -1;
    int slave_fd = -1;
    char slave_name[128] = {};
    if (openpty(&master_fd, &slave_fd, slave_name, nullptr, nullptr) < 0) {
        std::perror("openpty");
        return EXIT_FAILURE;
    }
    close(slave_fd);

    std::atomic<bool> mock_failed(false);
    std::thread mock(mock_mcu, master_fd, &mock_failed);

    xvs_uart_controller_t *controller = nullptr;
    EventCounters events;
    bool success = expect_ok("create", xvs_uart_create(slave_name, &controller));
    success = success && expect_ok(
                             "set event callback",
                             xvs_uart_set_event_callback(controller, on_event,
                                                         &events));
    success = success && expect_ok("ping", xvs_uart_ping(controller));
    success = success && expect_ok("idle", xvs_uart_idle(controller));
    success = success && expect_ok("start", xvs_uart_start(controller, 4, 10));

    xvs_uart_status_t status = {};
    success = success &&
              expect_ok("running status",
                        xvs_uart_get_status(controller, &status));
    success = success && !std::strcmp(status.state, "RUNNING") &&
              status.frequency_millihz == 4000 && status.low_pulse_us == 10;

    success = success &&
              expect_ok("count", xvs_uart_count(controller, 2, 8, 1000));
    success = success &&
              expect_ok("counting status",
                        xvs_uart_get_status(controller, &status));
    success = success && !std::strcmp(status.state, "COUNTING") &&
              status.frequency_millihz == 2000 && status.low_pulse_us == 8 &&
              status.pulse_count == 1000 && status.last_trigger_id == 1000;

    success = success && expect_ok("stop", xvs_uart_stop(controller));
    success = success &&
              expect_ok("idle status", xvs_uart_get_status(controller, &status));
    success = success && !std::strcmp(status.state, "IDLE");
    success = success && events.pps.load() == 2 && events.rmc.load() == 1 &&
              events.nmea.load() == 1 && events.xvs.load() == 1 &&
              events.last_trigger_id.load() == 101;

    xvs_uart_destroy(controller);
    close(master_fd);
    mock.join();
    success = success && !mock_failed.load();

    if (!success) {
        std::fprintf(stderr, "XVS_UART_MOCK_TEST_FAILED\n");
        return EXIT_FAILURE;
    }
    std::printf("XVS_UART_MOCK_TEST_OK async_events=PPS,RMC,NMEA,XVS\n");
    return EXIT_SUCCESS;
}
