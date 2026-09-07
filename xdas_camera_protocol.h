#ifndef XDAS_CAMERA_PROTOCOL_H
#define XDAS_CAMERA_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace xdas_camera_protocol {

enum result {
    OK = 0,
    ERR_ARGUMENT = -600,
    ERR_OPEN = -601,
    ERR_BUSY = -602,
    ERR_CONFIGURE = -603,
    ERR_IO = -604,
    ERR_PROTOCOL = -605,
};

enum error_code : std::uint8_t {
    ERROR_NONE = 0x00,
    ERROR_MISSING_HEADER = 0x01,
    ERROR_BAD_COMMAND = 0x02,
    ERROR_BAD_LENGTH = 0x03,
    ERROR_BAD_CRC = 0x04,
    ERROR_UNSUPPORTED = 0xff,
};

struct request {
    std::uint8_t command0 = 0xff;
    std::uint8_t command1 = 0xff;
    std::vector<std::uint8_t> payload;
};

struct reply {
    std::uint8_t error = ERROR_NONE;
    std::vector<std::uint8_t> payload;
};

using command_handler = std::function<void(const request &, reply *)>;
using stop_requested = std::function<bool()>;
/* Receiver for non-AA bytes seen on the wire (e.g. GNSS NMEA lines).
   Invoked with the bytes that were skipped while hunting for the 0xAA
   header, and with the trailing partial bytes on a poll timeout. */
using stray_bytes_sink =
    std::function<void(const std::vector<std::uint8_t> &)>;

std::uint8_t crc8(const std::uint8_t *data, std::size_t size);
int process_frame(const std::uint8_t *frame, std::size_t frame_size,
                  const command_handler &handler,
                  std::vector<std::uint8_t> *response);
int run(const std::string &device, const command_handler &handler,
        const stop_requested &should_stop,
        const stray_bytes_sink &stray_sink = stray_bytes_sink());
int protocol_self_test(std::string *report);
const char *strerror(int value);

}  // namespace xdas_camera_protocol

#endif
