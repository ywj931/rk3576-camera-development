#ifndef CAMERA_TRANSFER_PROTOCOL_H
#define CAMERA_TRANSFER_PROTOCOL_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace camera_transfer {

constexpr std::uint8_t kProtocolVersion = 1;
constexpr std::size_t kPhotoHeaderSize = 80;
constexpr std::size_t kAckSize = 24;
constexpr std::size_t kSha256Size = 32;
constexpr std::uint64_t kMaximumPayloadSize = 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint16_t kDefaultPort = 46000;

enum class photo_format : std::uint8_t {
    jpeg = 1,
    png = 2,
    nv12 = 3,
    dng = 4,
};

enum class ack_status : std::uint8_t {
    ok = 0,
    bad_header = 1,
    bad_length = 2,
    sha256_mismatch = 3,
    write_error = 4,
    unsupported_format = 5,
    /* A photo with this identity already exists with different data.
       Retrying with the same identity can never succeed. */
    conflict = 6,
};

enum result {
    ok = 0,
    err_argument = -700,
    err_protocol = -701,
    err_io = -702,
    err_hash = -703,
    err_connect = -704,
    err_timeout = -705,
    err_queue_full = -706,
    err_remote = -707,
    err_conflict = -708,
};

struct photo_header {
    photo_format format = photo_format::jpeg;
    std::uint8_t camera_id = 0;
    std::uint64_t session_id = 0;
    std::uint64_t frame_id = 0;
    std::uint64_t exposure_start_utc_ns = 0;
    std::uint64_t exposure_duration_ns = 0;
    std::uint64_t payload_size = 0;
    std::array<std::uint8_t, kSha256Size> sha256{};
};

struct ack {
    ack_status status = ack_status::bad_header;
    std::uint8_t camera_id = 0xff;
    std::uint64_t session_id = 0;
    std::uint64_t frame_id = 0;
};

int encode_photo_header(
    const photo_header &header,
    std::array<std::uint8_t, kPhotoHeaderSize> *wire);
int decode_photo_header(const std::uint8_t *wire, std::size_t wire_size,
                        photo_header *header);
int encode_ack(const ack &value, std::array<std::uint8_t, kAckSize> *wire);
int decode_ack(const std::uint8_t *wire, std::size_t wire_size, ack *value);

int sha256_file(const std::string &path,
                std::array<std::uint8_t, kSha256Size> *digest,
                std::uint64_t *file_size, std::string *error);
int sha256_fd(int fd, std::array<std::uint8_t, kSha256Size> *digest,
              std::uint64_t *file_size, std::string *error);
std::string sha256_hex(
    const std::array<std::uint8_t, kSha256Size> &digest);
bool supported_format(photo_format format);
const char *format_extension(photo_format format);
const char *ack_status_string(ack_status status);
const char *strerror(int value);

}  // namespace camera_transfer

#endif
