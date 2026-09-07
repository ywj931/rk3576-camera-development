#ifndef CAMERA_TRANSFER_CLIENT_H
#define CAMERA_TRANSFER_CLIENT_H

#include "camera_transfer_protocol.h"

#include <cstdint>
#include <string>

namespace camera_transfer {

struct send_request {
    std::string path;
    photo_format format = photo_format::jpeg;
    std::uint8_t camera_id = 0;
    std::uint64_t session_id = 0;
    std::uint64_t frame_id = 0;
    std::uint64_t exposure_start_utc_ns = 0;
    std::uint64_t exposure_duration_ns = 0;
};

class client {
public:
    client(std::string host, std::uint16_t port, int timeout_ms = 10000);
    ~client();

    client(const client &) = delete;
    client &operator=(const client &) = delete;

    int send_photo(const send_request &request, ack *response,
                   std::string *error);
    void disconnect();

private:
    int connect_receiver(std::string *error);

    std::string host_;
    std::uint16_t port_ = 0;
    int timeout_ms_ = 0;
    int socket_ = -1;
};

}  // namespace camera_transfer

#endif
