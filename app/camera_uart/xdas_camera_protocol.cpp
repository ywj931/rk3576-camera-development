#include "xdas_camera_protocol.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <termios.h>
#include <unistd.h>

namespace xdas_camera_protocol {
namespace {

constexpr std::uint8_t kRequestHeader = 0xaa;
constexpr std::uint8_t kResponseHeader = 0x55;
constexpr std::size_t kMinimumRequestSize = 5;
constexpr std::size_t kMinimumResponseSize = 6;
constexpr std::size_t kMaximumRequestSize = 64;

std::vector<std::uint8_t> make_response(std::uint8_t command0,
                                        std::uint8_t command1,
                                        std::uint8_t error,
                                        const std::vector<std::uint8_t> &payload)
{
    if (payload.size() > 255U - kMinimumResponseSize)
        return {};
    std::vector<std::uint8_t> response;
    response.reserve(kMinimumResponseSize + payload.size());
    response.push_back(kResponseHeader);
    response.push_back(static_cast<std::uint8_t>(kMinimumResponseSize +
                                                 payload.size()));
    response.push_back(command0);
    response.push_back(command1);
    response.push_back(error);
    response.insert(response.end(), payload.begin(), payload.end());
    response.push_back(crc8(response.data(), response.size()));
    return response;
}

int configure(int fd, struct termios *saved)
{
    if (tcgetattr(fd, saved) < 0)
        return ERR_CONFIGURE;
    struct termios settings = *saved;
    cfmakeraw(&settings);
    cfsetispeed(&settings, B115200);
    cfsetospeed(&settings, B115200);
    settings.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
    settings.c_cflag |= CS8 | CLOCAL | CREAD;
    settings.c_iflag &= ~(IXON | IXOFF | IXANY);
    settings.c_cc[VMIN] = 0;
    settings.c_cc[VTIME] = 1;
    if (tcsetattr(fd, TCSANOW, &settings) < 0 || tcflush(fd, TCIOFLUSH) < 0)
        return ERR_CONFIGURE;
    return OK;
}

int write_all(int fd, const std::vector<std::uint8_t> &data)
{
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written =
            write(fd, data.data() + offset, data.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd descriptor = {fd, POLLOUT, 0};
            if (poll(&descriptor, 1, 1000) > 0)
                continue;
        }
        return ERR_IO;
    }
    return OK;
}

int send_length_error(int fd)
{
    const std::vector<std::uint8_t> response =
        make_response(0xff, 0xff, ERROR_BAD_LENGTH, {});
    return response.empty() ? ERR_PROTOCOL : write_all(fd, response);
}

}  // namespace

std::uint8_t crc8(const std::uint8_t *data, std::size_t size)
{
    std::uint8_t crc = 0x00;
    if (!data && size)
        return crc;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (std::uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80U) != 0
                      ? static_cast<std::uint8_t>((crc << 1U) ^ 0xd5U)
                      : static_cast<std::uint8_t>(crc << 1U);
        }
    }
    return crc;
}

int process_frame(const std::uint8_t *frame, std::size_t frame_size,
                  const command_handler &handler,
                  std::vector<std::uint8_t> *response)
{
    if (!frame || !frame_size || !handler || !response)
        return ERR_ARGUMENT;

    std::uint8_t command0 = frame_size > 2 ? frame[2] : 0xff;
    std::uint8_t command1 = frame_size > 3 ? frame[3] : 0xff;
    std::uint8_t error = ERROR_NONE;
    reply command_reply;

    if (frame[0] != kRequestHeader) {
        error = ERROR_MISSING_HEADER;
    } else if (frame_size < kMinimumRequestSize || frame_size > 255U ||
               frame[1] != frame_size) {
        error = ERROR_BAD_LENGTH;
    } else if (crc8(frame, frame_size - 1U) != frame[frame_size - 1U]) {
        error = ERROR_BAD_CRC;
    } else {
        request command_request;
        command_request.command0 = command0;
        command_request.command1 = command1;
        command_request.payload.assign(frame + 4, frame + frame_size - 1U);
        handler(command_request, &command_reply);
        error = command_reply.error;
    }

    // The xdas response contract uses FF/FF when framing itself is invalid.
    if (error == ERROR_MISSING_HEADER || error == ERROR_BAD_LENGTH ||
        error == ERROR_BAD_CRC) {
        command0 = 0xff;
        command1 = 0xff;
    }

    *response = make_response(command0, command1, error,
                              error == ERROR_NONE ? command_reply.payload
                                                  : std::vector<std::uint8_t>{});
    return response->empty() ? ERR_PROTOCOL : OK;
}

int run(const std::string &device, const command_handler &handler,
        const stop_requested &should_stop)
{
    if (device.empty() || !handler || !should_stop)
        return ERR_ARGUMENT;
    const int fd = open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return ERR_OPEN;
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        close(fd);
        return ERR_BUSY;
    }

    struct termios saved = {};
    int result_value = configure(fd, &saved);
    if (result_value != OK) {
        flock(fd, LOCK_UN);
        close(fd);
        return result_value;
    }

    std::vector<std::uint8_t> input;
    input.reserve(256);
    while (!should_stop()) {
        struct pollfd descriptor = {fd, POLLIN, 0};
        const int ready = poll(&descriptor, 1, 200);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            result_value = ERR_IO;
            break;
        }
        if (ready == 0) {
            if (!input.empty()) {
                input.clear();
                if (send_length_error(fd) != OK) {
                    result_value = ERR_IO;
                    break;
                }
            }
            continue;
        }
        if ((descriptor.revents & (POLLERR | POLLNVAL)) ||
            ((descriptor.revents & POLLHUP) &&
             !(descriptor.revents & POLLIN))) {
            result_value = ERR_IO;
            break;
        }

        std::uint8_t bytes[256];
        const ssize_t count = read(fd, bytes, sizeof(bytes));
        if (count < 0 && (errno == EINTR || errno == EAGAIN ||
                         errno == EWOULDBLOCK))
            continue;
        if (count < 0) {
            result_value = ERR_IO;
            break;
        }
        if (count == 0)
            continue;
        input.insert(input.end(), bytes, bytes + count);

        while (!input.empty()) {
            const auto header = std::find(input.begin(), input.end(),
                                          kRequestHeader);
            if (header == input.end()) {
                input.clear();
                break;
            }
            input.erase(input.begin(), header);
            if (input.size() < 2U)
                break;
            const std::size_t frame_size = input[1];
            if (frame_size < kMinimumRequestSize ||
                frame_size > kMaximumRequestSize) {
                input.erase(input.begin());
                if (send_length_error(fd) != OK) {
                    result_value = ERR_IO;
                    break;
                }
                continue;
            }
            if (input.size() < frame_size)
                break;

            std::vector<std::uint8_t> response;
            const int process_result =
                process_frame(input.data(), frame_size, handler, &response);
            input.erase(input.begin(), input.begin() + frame_size);
            if (process_result != OK || write_all(fd, response) != OK) {
                result_value = process_result == OK ? ERR_IO : process_result;
                break;
            }
        }
        if (result_value != OK)
            break;
    }

    tcsetattr(fd, TCSANOW, &saved);
    flock(fd, LOCK_UN);
    close(fd);
    return result_value;
}

int protocol_self_test(std::string *report)
{
    const std::uint8_t version[] = {0xaa, 0x05, 0x00, 0x00};
    const std::uint8_t save_on[] = {0xaa, 0x05, 0x01, 0x11};
    const std::uint8_t save_off[] = {0xaa, 0x05, 0x01, 0x12};
    const std::uint8_t shutter[] = {0xaa, 0x06, 0x01, 0x19, 0x0f};
    if (crc8(version, sizeof(version)) != 0xff ||
        crc8(save_on, sizeof(save_on)) != 0x73 ||
        crc8(save_off, sizeof(save_off)) != 0xd9 ||
        crc8(shutter, sizeof(shutter)) != 0x2b) {
        if (report)
            *report = "CRC8/0xD5 reference vector mismatch";
        return ERR_PROTOCOL;
    }

    const std::vector<std::uint8_t> request_frame =
        {0xaa, 0x05, 0x01, 0x11, 0x73};
    std::vector<std::uint8_t> response;
    const auto handler = [](const request &command, reply *command_reply) {
        if (command.command0 != 0x01 || command.command1 != 0x11 ||
            !command.payload.empty()) {
            command_reply->error = ERROR_BAD_COMMAND;
        }
    };
    if (process_frame(request_frame.data(), request_frame.size(), handler,
                      &response) != OK ||
        response != std::vector<std::uint8_t>(
                        {0x55, 0x06, 0x01, 0x11, 0x00, 0xa7})) {
        if (report)
            *report = "save-on request/ACK mismatch";
        return ERR_PROTOCOL;
    }

    std::vector<std::uint8_t> invalid = request_frame;
    invalid.back() ^= 0x01;
    if (process_frame(invalid.data(), invalid.size(), handler, &response) != OK ||
        response.size() != kMinimumResponseSize ||
        response[2] != 0xff || response[3] != 0xff ||
        response[4] != ERROR_BAD_CRC ||
        crc8(response.data(), response.size() - 1U) != response.back()) {
        if (report)
            *report = "bad CRC response mismatch";
        return ERR_PROTOCOL;
    }

    if (report)
        *report = "crc_vectors=4 save_ack=1 bad_crc=1";
    return OK;
}

const char *strerror(int value)
{
    switch (value) {
    case OK:
        return "success";
    case ERR_ARGUMENT:
        return "invalid argument";
    case ERR_OPEN:
        return "failed to open UART device";
    case ERR_BUSY:
        return "UART device is already in use";
    case ERR_CONFIGURE:
        return "failed to configure UART";
    case ERR_IO:
        return "UART input/output error";
    case ERR_PROTOCOL:
        return "protocol error";
    default:
        return "unknown error";
    }
}

}  // namespace xdas_camera_protocol
