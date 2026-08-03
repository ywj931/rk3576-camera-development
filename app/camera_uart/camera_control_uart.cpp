#include "camera_control_uart.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/file.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

namespace camera_control_uart {
namespace {

constexpr uint32_t kGlobalCameraId = 255;
constexpr size_t kMaximumFrameLength = 1024;

struct request {
    uint32_t sequence = 0;
    uint32_t camera_id = kGlobalCameraId;
    std::string operation;
    std::vector<std::string> arguments;
};

std::vector<std::string> split_csv(const std::string &text)
{
    std::vector<std::string> fields;
    size_t begin = 0;
    for (;;) {
        const size_t comma = text.find(',', begin);
        fields.emplace_back(text.substr(begin, comma - begin));
        if (comma == std::string::npos)
            break;
        begin = comma + 1;
    }
    return fields;
}

bool parse_u32(const std::string &text, uint32_t *value)
{
    if (!value || text.empty())
        return false;
    uint64_t parsed = 0;
    for (const unsigned char byte : text) {
        if (!std::isdigit(byte))
            return false;
        parsed = parsed * 10 + (byte - '0');
        if (parsed > UINT32_MAX)
            return false;
    }
    *value = static_cast<uint32_t>(parsed);
    return true;
}

bool is_unsigned_number(const std::string &text)
{
    uint32_t ignored = 0;
    return parse_u32(text, &ignored);
}

bool is_signed_number(const std::string &text)
{
    if (text.empty())
        return false;
    size_t position = text[0] == '-' ? 1 : 0;
    if (position == text.size())
        return false;
    return std::all_of(text.begin() + position, text.end(),
                       [](unsigned char byte) { return std::isdigit(byte); });
}

std::string upper(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char byte) {
                       return static_cast<char>(std::toupper(byte));
                   });
    return value;
}

bool valid_output_directory(const std::string &path)
{
    if (path.empty() || path[0] != '/')
        return false;
    return std::none_of(path.begin(), path.end(), [](unsigned char byte) {
        return std::isspace(byte) || byte == ',' || byte == '\'' ||
               byte == '"';
    });
}

std::string percent_encode(const std::string &input)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(input.size());
    for (const unsigned char byte : input) {
        if (std::isalnum(byte) || byte == '.' || byte == '_' || byte == ':' ||
            byte == '/' || byte == '=' || byte == '-') {
            output.push_back(static_cast<char>(byte));
        } else {
            output.push_back('%');
            output.push_back(digits[byte >> 4]);
            output.push_back(digits[byte & 0x0f]);
        }
    }
    return output;
}

bool output_contains_error(const std::string &output)
{
    size_t begin = 0;
    while (begin < output.size()) {
        const size_t end = output.find('\n', begin);
        const size_t length = (end == std::string::npos ? output.size() : end) -
                              begin;
        if (output.compare(begin, std::min<size_t>(length, 6), "ERROR ") == 0)
            return true;
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return false;
}

std::string nack(uint32_t sequence, uint32_t camera_id,
                 const std::string &reason)
{
    return "$NACK," + std::to_string(sequence) + "," +
           std::to_string(camera_id) + "," + reason + "\r\n";
}

int parse_request(const std::string &line, request *parsed,
                  std::string *reason)
{
    if (!parsed || !reason)
        return ERR_ARGUMENT;
    if (line.size() >= kMaximumFrameLength) {
        *reason = "FRAME_TOO_LONG";
        return ERR_PROTOCOL;
    }
    const std::vector<std::string> fields = split_csv(line);
    if (fields.size() < 4 || fields[0] != "$CAM") {
        *reason = "BAD_FORMAT";
        return ERR_PROTOCOL;
    }
    if (!parse_u32(fields[1], &parsed->sequence)) {
        *reason = "BAD_SEQUENCE";
        return ERR_PROTOCOL;
    }
    if (!parse_u32(fields[2], &parsed->camera_id) ||
        (parsed->camera_id > 1 && parsed->camera_id != kGlobalCameraId)) {
        *reason = "BAD_CAMERA_ID";
        return ERR_PROTOCOL;
    }
    parsed->operation = upper(fields[3]);
    parsed->arguments.assign(fields.begin() + 4, fields.end());
    return OK;
}

int translate(const request &input, std::string *command,
              std::string *reason)
{
    if (!command || !reason)
        return ERR_ARGUMENT;
    const bool global = input.camera_id == kGlobalCameraId;
    const std::string target = global ? "all" : std::to_string(input.camera_id);
    const auto require_arguments = [&](size_t count) {
        if (input.arguments.size() == count)
            return true;
        *reason = "BAD_ARGUMENT_COUNT";
        return false;
    };
    const auto require_camera = [&]() {
        if (!global)
            return true;
        *reason = "CAMERA_ID_REQUIRED";
        return false;
    };
    const auto require_global = [&]() {
        if (global)
            return true;
        *reason = "GLOBAL_CAMERA_ID_REQUIRED";
        return false;
    };

    if (input.operation == "PING") {
        if (!require_arguments(0)) return ERR_PROTOCOL;
        *command = "PING";
    } else if (input.operation == "GET_STATUS" ||
               input.operation == "STATUS") {
        if (!require_arguments(0)) return ERR_PROTOCOL;
        *command = "status " + target;
    } else if (input.operation == "CAPTURE_STATUS") {
        if (!require_arguments(0)) return ERR_PROTOCOL;
        *command = "capture-status " + target;
    } else if (input.operation == "STREAM_START") {
        if (!require_arguments(0)) return ERR_PROTOCOL;
        *command = "stream-start " + target;
    } else if (input.operation == "STREAM_STOP") {
        if (!require_arguments(0)) return ERR_PROTOCOL;
        *command = "stream-stop " + target;
    } else if (input.operation == "SAVE_START") {
        if (!require_camera() || !require_arguments(1)) return ERR_PROTOCOL;
        if (!valid_output_directory(input.arguments[0])) {
            *reason = "BAD_OUTPUT_DIRECTORY";
            return ERR_PROTOCOL;
        }
        *command = "save-start " + target + " " + input.arguments[0];
    } else if (input.operation == "SAVE_STOP") {
        if (!require_camera() || !require_arguments(0)) return ERR_PROTOCOL;
        *command = "save-stop " + target;
    } else if (input.operation == "UVC_START") {
        if (!require_arguments(0)) return ERR_PROTOCOL;
        *command = "uvc-start " + target;
    } else if (input.operation == "UVC_STOP") {
        if (!require_arguments(0)) return ERR_PROTOCOL;
        *command = "uvc-stop " + target;
    } else if (input.operation == "UVC_STATUS") {
        if (!require_arguments(0)) return ERR_PROTOCOL;
        *command = "uvc-status " + target;
    } else if (input.operation == "NET_START") {
        if (!require_camera() || !require_arguments(0)) return ERR_PROTOCOL;
        *command = "net-start " + target;
    } else if (input.operation == "NET_STOP") {
        if (!require_arguments(0)) return ERR_PROTOCOL;
        *command = "net-stop " + target;
    } else if (input.operation == "NET_STATUS") {
        if (!require_arguments(0)) return ERR_PROTOCOL;
        *command = "net-status " + target;
    } else if (input.operation == "PHOTO_START") {
        if (!require_camera() || !require_arguments(1)) return ERR_PROTOCOL;
        if (!valid_output_directory(input.arguments[0])) {
            *reason = "BAD_OUTPUT_DIRECTORY";
            return ERR_PROTOCOL;
        }
        *command = "photo-start " + target + " " + input.arguments[0];
    } else if (input.operation == "PHOTO_STOP") {
        if (!require_camera() || !require_arguments(0)) return ERR_PROTOCOL;
        *command = "photo-stop " + target;
    } else if (input.operation == "PHOTO_STATUS") {
        if (!require_arguments(0)) return ERR_PROTOCOL;
        *command = "photo-status " + target;
    } else if (input.operation == "PHOTO_OFFSET") {
        if (!require_camera() || !require_arguments(1)) return ERR_PROTOCOL;
        if (!is_signed_number(input.arguments[0])) {
            *reason = "BAD_NUMBER";
            return ERR_PROTOCOL;
        }
        *command = "photo-offset " + target + " " + input.arguments[0];
    } else if (input.operation == "AUTO") {
        if (!require_camera() || !require_arguments(0)) return ERR_PROTOCOL;
        *command = "auto " + target;
    } else if (input.operation == "EXPOSURE") {
        if (!require_camera() || !require_arguments(1)) return ERR_PROTOCOL;
        if (!is_unsigned_number(input.arguments[0])) {
            *reason = "BAD_NUMBER";
            return ERR_PROTOCOL;
        }
        *command = "exposure " + target + " " + input.arguments[0];
    } else if (input.operation == "GAIN") {
        if (!require_camera() || !require_arguments(1)) return ERR_PROTOCOL;
        if (!is_unsigned_number(input.arguments[0])) {
            *reason = "BAD_NUMBER";
            return ERR_PROTOCOL;
        }
        *command = "gain " + target + " " + input.arguments[0];
    } else if (input.operation == "FPS") {
        if (!require_camera() || !require_arguments(1)) return ERR_PROTOCOL;
        if (!is_unsigned_number(input.arguments[0])) {
            *reason = "BAD_NUMBER";
            return ERR_PROTOCOL;
        }
        *command = "fps " + target + " " + input.arguments[0];
    } else if (input.operation == "SYNC_START") {
        if (!require_global() || input.arguments.empty() ||
            input.arguments.size() > 2) {
            if (reason->empty()) *reason = "BAD_ARGUMENT_COUNT";
            return ERR_PROTOCOL;
        }
        if (!is_unsigned_number(input.arguments[0]) ||
            (input.arguments.size() == 2 &&
             !is_unsigned_number(input.arguments[1]))) {
            *reason = "BAD_NUMBER";
            return ERR_PROTOCOL;
        }
        *command = "sync-start " + input.arguments[0];
        if (input.arguments.size() == 2)
            *command += " " + input.arguments[1];
    } else if (input.operation == "SYNC_STOP") {
        if (!require_global() || !require_arguments(0)) return ERR_PROTOCOL;
        *command = "sync-stop";
    } else if (input.operation == "SYNC_STATUS") {
        if (!require_global() || !require_arguments(0)) return ERR_PROTOCOL;
        *command = "sync-status";
    } else {
        *reason = "UNKNOWN_COMMAND";
        return ERR_PROTOCOL;
    }
    return OK;
}

std::string process_frame(const std::string &line,
                          const command_handler &handler)
{
    request parsed;
    std::string reason;
    if (parse_request(line, &parsed, &reason) != OK)
        return nack(parsed.sequence, parsed.camera_id, reason);

    std::string command;
    if (translate(parsed, &command, &reason) != OK)
        return nack(parsed.sequence, parsed.camera_id, reason);
    if (command == "PING") {
        return "$ACK," + std::to_string(parsed.sequence) + "," +
               std::to_string(parsed.camera_id) + ",PONG\r\n";
    }

    std::string output;
    handler(command, &output);
    const bool failed = output_contains_error(output);
    const std::string prefix = failed ? "$NACK," : "$ACK,";
    return prefix + std::to_string(parsed.sequence) + "," +
           std::to_string(parsed.camera_id) + "," + parsed.operation + "," +
           percent_encode(output) + "\r\n";
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

int write_all(int fd, const std::string &data)
{
    size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written = write(fd, data.data() + offset,
                                      data.size() - offset);
        if (written > 0) {
            offset += static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd poll_fd = {fd, POLLOUT, 0};
            if (poll(&poll_fd, 1, 1000) > 0)
                continue;
        }
        return ERR_IO;
    }
    return OK;
}

}  // namespace

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

    std::string frame;
    bool discarding = false;
    while (!should_stop()) {
        struct pollfd poll_fd = {fd, POLLIN, 0};
        const int ready = poll(&poll_fd, 1, 200);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            result_value = ERR_IO;
            break;
        }
        if (ready == 0)
            continue;
        if (poll_fd.revents & (POLLERR | POLLNVAL)) {
            result_value = ERR_IO;
            break;
        }
        char bytes[256];
        const ssize_t count = read(fd, bytes, sizeof(bytes));
        if (count < 0 && (errno == EINTR || errno == EAGAIN ||
                         errno == EWOULDBLOCK))
            continue;
        if (count < 0) {
            result_value = ERR_IO;
            break;
        }
        for (ssize_t index = 0; index < count; ++index) {
            const char byte = bytes[index];
            if (byte == '\r')
                continue;
            if (byte == '\n') {
                std::string response;
                if (discarding) {
                    response = nack(0, kGlobalCameraId, "FRAME_TOO_LONG");
                } else if (!frame.empty()) {
                    response = process_frame(frame, handler);
                }
                frame.clear();
                discarding = false;
                if (!response.empty() && write_all(fd, response) != OK) {
                    result_value = ERR_IO;
                    break;
                }
                continue;
            }
            if (!discarding) {
                if (frame.size() + 1 >= kMaximumFrameLength) {
                    frame.clear();
                    discarding = true;
                } else {
                    frame.push_back(byte);
                }
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
    struct test_case {
        const char *frame;
        const char *expected_command;
        bool valid;
    };
    const test_case tests[] = {
        {"$CAM,1,0,UVC_START", "uvc-start 0", true},
        {"$CAM,2,1,UVC_STOP", "uvc-stop 1", true},
        {"$CAM,3,255,UVC_STATUS", "uvc-status all", true},
        {"$CAM,4,0,SAVE_START,/mnt/emmc/cam0", "save-start 0 /mnt/emmc/cam0", true},
        {"$CAM,5,1,NET_START", "net-start 1", true},
        {"$CAM,6,0,EXPOSURE,30000", "exposure 0 30000", true},
        {"$CAM,7,1,GAIN,8000", "gain 1 8000", true},
        {"$CAM,8,255,SYNC_START,2,10", "sync-start 2 10", true},
        {"$CAM,9,255,SAVE_START,/tmp/bad", "", false},
        {"$CAM,10,7,UVC_START", "", false},
    };

    size_t passed = 0;
    for (const test_case &test : tests) {
        request parsed;
        std::string reason;
        const int parse_result = parse_request(test.frame, &parsed, &reason);
        std::string command;
        const int translate_result = parse_result == OK
                                         ? translate(parsed, &command, &reason)
                                         : parse_result;
        const bool valid = translate_result == OK;
        if (valid != test.valid ||
            (valid && command != test.expected_command)) {
            if (report) {
                *report = std::string("frame=") + test.frame +
                          " command=" + command + " reason=" + reason;
            }
            return ERR_PROTOCOL;
        }
        ++passed;
    }

    const command_handler handler = [](const std::string &command,
                                       std::string *output) {
        *output = command == "uvc-status 0"
                      ? "UVC_STATUS camera_id=0 enabled=1\n"
                      : "ERROR command=test reason=failed\n";
    };
    const std::string ack = process_frame("$CAM,11,0,UVC_STATUS", handler);
    const std::string error = process_frame("$CAM,12,0,NET_STATUS", handler);
    if (ack.find("$ACK,11,0,UVC_STATUS,") != 0 ||
        error.find("$NACK,12,0,NET_STATUS,") != 0) {
        if (report)
            *report = "ACK/NACK response test failed";
        return ERR_PROTOCOL;
    }
    passed += 2;
    if (report)
        *report = "cases=" + std::to_string(passed);
    return OK;
}

const char *strerror(int value)
{
    switch (value) {
    case OK: return "ok";
    case ERR_ARGUMENT: return "invalid argument";
    case ERR_OPEN: return "cannot open UART";
    case ERR_BUSY: return "UART is already in use";
    case ERR_CONFIGURE: return "cannot configure UART as 115200 8N1";
    case ERR_IO: return "UART I/O error";
    case ERR_PROTOCOL: return "UART protocol error";
    default: return "unknown control UART error";
    }
}

}  // namespace camera_control_uart

#ifdef CAMERA_CONTROL_UART_SELF_TEST_MAIN
#include <atomic>
#include <chrono>
#include <iostream>
#include <pty.h>
#include <thread>

int main()
{
    std::string report;
    const int result = camera_control_uart::protocol_self_test(&report);
    if (result != camera_control_uart::OK) {
        std::cerr << "CONTROL_UART_PROTOCOL_SELF_TEST_FAILED code=" << result
                  << " detail=\"" << report << "\"\n";
        return 1;
    }

    int master_fd = -1;
    int slave_fd = -1;
    char slave_name[128] = {};
    if (openpty(&master_fd, &slave_fd, slave_name, nullptr, nullptr) < 0) {
        std::cerr << "CONTROL_UART_PTY_SELF_TEST_FAILED stage=openpty\n";
        return 1;
    }
    close(slave_fd);
    std::atomic<bool> stop{false};
    int run_result = camera_control_uart::ERR_IO;
    std::thread uart_thread([&] {
        run_result = camera_control_uart::run(
            slave_name,
            [](const std::string &command, std::string *output) {
                *output = "OK command=" + command + "\n";
            },
            [&] { return stop.load(); });
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const std::string request = "$CAM,101,1,UVC_STOP\r\n";
    if (write(master_fd, request.data(), request.size()) !=
        static_cast<ssize_t>(request.size())) {
        stop = true;
        uart_thread.join();
        close(master_fd);
        std::cerr << "CONTROL_UART_PTY_SELF_TEST_FAILED stage=write\n";
        return 1;
    }
    struct pollfd poll_fd = {master_fd, POLLIN, 0};
    std::string response;
    if (poll(&poll_fd, 1, 1000) > 0) {
        char bytes[512];
        const ssize_t count = read(master_fd, bytes, sizeof(bytes));
        if (count > 0)
            response.assign(bytes, bytes + count);
    }
    stop = true;
    uart_thread.join();
    close(master_fd);
    if (run_result != camera_control_uart::OK ||
        response.find("$ACK,101,1,UVC_STOP,") == std::string::npos) {
        std::cerr << "CONTROL_UART_PTY_SELF_TEST_FAILED run_result="
                  << run_result << " response=\"" << response << "\"\n";
        return 1;
    }
    std::cout << "CONTROL_UART_PROTOCOL_SELF_TEST_OK detail=\"" << report
              << " pty=115200_8N1_ACK\"\n";
    return 0;
}
#endif
