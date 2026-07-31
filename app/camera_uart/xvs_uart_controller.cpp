#include "xvs_uart_controller.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
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
    unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
    if (errno || end == text.c_str() || *end != '\0' ||
        parsed > UINT32_MAX) {
        return false;
    }
    *value = static_cast<uint32_t>(parsed);
    return true;
}

bool parse_u64(const std::string &text, uint64_t *value)
{
    if (!value || text.empty() || text[0] == '-')
        return false;
    errno = 0;
    char *end = nullptr;
    unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (errno || end == text.c_str() || *end != '\0')
        return false;
    *value = static_cast<uint64_t>(parsed);
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

    const std::string checksum_text = frame.substr(star + 1);
    char *end = nullptr;
    errno = 0;
    const unsigned long received =
        std::strtoul(checksum_text.c_str(), &end, 16);
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
    if (kind == "ACK") {
        response->ack = true;
    } else if (kind == "NACK") {
        response->ack = false;
    } else {
        return false;
    }
    response->fields = payload;
    return true;
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

int read_line(int fd, std::string *line)
{
    if (!line)
        return XVS_UART_ERR_ARGUMENT;
    line->clear();
    int remaining_ms = kResponseTimeoutMs;
    while (remaining_ms > 0) {
        struct pollfd pfd = {fd, POLLIN, 0};
        const int slice_ms = remaining_ms > 100 ? 100 : remaining_ms;
        const int ready = poll(&pfd, 1, slice_ms);
        remaining_ms -= slice_ms;
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return XVS_UART_ERR_IO;
        }
        if (ready == 0)
            continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            return XVS_UART_ERR_IO;

        char buffer[64];
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return XVS_UART_ERR_IO;
        }
        for (ssize_t index = 0; index < count; ++index) {
            const char byte = buffer[index];
            if (byte == '\r')
                continue;
            if (byte == '\n')
                return line->empty() ? XVS_UART_ERR_PROTOCOL : XVS_UART_OK;
            if (line->size() + 1 >= kFrameMax)
                return XVS_UART_ERR_PROTOCOL;
            line->push_back(byte);
        }
    }
    return XVS_UART_ERR_TIMEOUT;
}

}  // namespace

struct xvs_uart_controller {
    int fd = -1;
    struct termios saved = {};
    bool saved_valid = false;
    uint32_t next_sequence = 1;
    std::string device;
};

namespace {

int transact(xvs_uart_controller_t *controller, const std::string &command,
             ParsedResponse *response)
{
    if (!controller || controller->fd < 0 || !response)
        return XVS_UART_ERR_ARGUMENT;

    const uint32_t sequence = controller->next_sequence++;
    const std::string payload =
        "XVS," + std::to_string(sequence) + "," + command;
    const std::string request = add_crc_and_terminator(payload);
    int result = write_all(controller->fd, request.data(), request.size());
    if (result != XVS_UART_OK)
        return result;
    if (tcdrain(controller->fd) < 0)
        return XVS_UART_ERR_IO;

    std::string line;
    result = read_line(controller->fd, &line);
    if (result != XVS_UART_OK)
        return result;
    if (!parse_response(line, response) || response->sequence != sequence)
        return XVS_UART_ERR_PROTOCOL;
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

    xvs_uart_controller_t *controller = new xvs_uart_controller_t;
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
    *controller_out = controller;
    return XVS_UART_OK;
}

extern "C" void xvs_uart_destroy(xvs_uart_controller_t *controller)
{
    if (!controller)
        return;
    if (controller->fd >= 0) {
        if (controller->saved_valid)
            tcsetattr(controller->fd, TCSANOW, &controller->saved);
        close(controller->fd);
    }
    delete controller;
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
    if ((frequency_hz != 2 && frequency_hz != 4) || low_pulse_us == 0 ||
        low_pulse_us > 1000) {
        return XVS_UART_ERR_ARGUMENT;
    }
    const std::string command =
        "START," + std::to_string(frequency_hz * 1000U) + "," +
        std::to_string(low_pulse_us);
    return simple_command(controller, command, "START");
}

extern "C" int xvs_uart_count(xvs_uart_controller_t *controller,
                              uint32_t frequency_hz,
                              uint32_t low_pulse_us,
                              uint32_t pulse_count)
{
    if ((frequency_hz != 2 && frequency_hz != 4) || low_pulse_us == 0 ||
        low_pulse_us > 1000 || pulse_count == 0) {
        return XVS_UART_ERR_ARGUMENT;
    }
    const std::string command =
        "COUNT," + std::to_string(frequency_hz * 1000U) + "," +
        std::to_string(low_pulse_us) + "," + std::to_string(pulse_count);
    return simple_command(controller, command, "COUNT");
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
    const std::string request = add_crc_and_terminator("XVS,7,START,4000,10");
    std::string checked_request_payload;
    if (!parse_checked_frame(request.substr(0, request.size() - 2),
                             &checked_request_payload) ||
        checked_request_payload != request_payload) {
        return XVS_UART_ERR_PROTOCOL;
    }

    const std::string response_frame = add_crc_and_terminator(
        "ACK,7,STATUS,STATE=IDLE,FREQ_MHZ=4000,LOW_US=10,"
        "PULSE_COUNT=1000,LAST_TRIGGER_ID=1000");
    ParsedResponse response;
    if (!parse_response(response_frame.substr(0, response_frame.size() - 2),
                        &response) ||
        !response.ack || response.sequence != 7 ||
        response.command != "STATUS") {
        return XVS_UART_ERR_PROTOCOL;
    }
    std::string value;
    if (!find_field(response.fields, "PULSE_COUNT", &value) ||
        value != "1000") {
        return XVS_UART_ERR_PROTOCOL;
    }
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
