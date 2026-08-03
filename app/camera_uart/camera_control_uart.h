#ifndef CAMERA_CONTROL_UART_H
#define CAMERA_CONTROL_UART_H

#include <functional>
#include <string>

namespace camera_control_uart {

enum result {
    OK = 0,
    ERR_ARGUMENT = -300,
    ERR_OPEN = -301,
    ERR_BUSY = -302,
    ERR_CONFIGURE = -303,
    ERR_IO = -304,
    ERR_PROTOCOL = -305,
};

using command_handler =
    std::function<void(const std::string &command, std::string *output)>;
using stop_requested = std::function<bool()>;

int run(const std::string &device, const command_handler &handler,
        const stop_requested &should_stop);
int protocol_self_test(std::string *report);
const char *strerror(int value);

}  // namespace camera_control_uart

#endif
