#include "camera_transfer_protocol.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

namespace camera_transfer {
namespace {

constexpr std::array<std::uint8_t, 4> kPhotoMagic = {'D', 'I', 'M', 'G'};
constexpr std::array<std::uint8_t, 4> kAckMagic = {'D', 'A', 'C', 'K'};

void put_u64(std::uint8_t *output, std::uint64_t value)
{
    for (int index = 7; index >= 0; --index) {
        output[index] = static_cast<std::uint8_t>(value);
        value >>= 8U;
    }
}

std::uint64_t get_u64(const std::uint8_t *input)
{
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index)
        value = (value << 8U) | input[index];
    return value;
}

bool valid_ack_status(ack_status status)
{
    return status >= ack_status::ok && status <= ack_status::conflict;
}

}  // namespace

bool supported_format(photo_format format)
{
    return format >= photo_format::jpeg && format <= photo_format::dng;
}

int encode_photo_header(
    const photo_header &header,
    std::array<std::uint8_t, kPhotoHeaderSize> *wire)
{
    if (!wire || !supported_format(header.format) || header.camera_id > 1U ||
        !header.session_id || !header.exposure_start_utc_ns ||
        !header.exposure_duration_ns || !header.payload_size ||
        header.payload_size > kMaximumPayloadSize) {
        return err_argument;
    }

    wire->fill(0);
    std::copy(kPhotoMagic.begin(), kPhotoMagic.end(), wire->begin());
    (*wire)[4] = kProtocolVersion;
    (*wire)[5] = static_cast<std::uint8_t>(kPhotoHeaderSize);
    (*wire)[6] = static_cast<std::uint8_t>(header.format);
    (*wire)[7] = header.camera_id;
    put_u64(wire->data() + 8, header.session_id);
    put_u64(wire->data() + 16, header.frame_id);
    put_u64(wire->data() + 24, header.exposure_start_utc_ns);
    put_u64(wire->data() + 32, header.exposure_duration_ns);
    put_u64(wire->data() + 40, header.payload_size);
    std::copy(header.sha256.begin(), header.sha256.end(), wire->begin() + 48);
    return ok;
}

int decode_photo_header(const std::uint8_t *wire, std::size_t wire_size,
                        photo_header *header)
{
    if (!wire || !header || wire_size != kPhotoHeaderSize)
        return err_argument;
    if (!std::equal(kPhotoMagic.begin(), kPhotoMagic.end(), wire) ||
        wire[4] != kProtocolVersion || wire[5] != kPhotoHeaderSize)
        return err_protocol;

    photo_header decoded;
    decoded.format = static_cast<photo_format>(wire[6]);
    decoded.camera_id = wire[7];
    decoded.session_id = get_u64(wire + 8);
    decoded.frame_id = get_u64(wire + 16);
    decoded.exposure_start_utc_ns = get_u64(wire + 24);
    decoded.exposure_duration_ns = get_u64(wire + 32);
    decoded.payload_size = get_u64(wire + 40);
    std::copy(wire + 48, wire + 80, decoded.sha256.begin());
    if (decoded.camera_id > 1U || !decoded.session_id ||
        !decoded.exposure_start_utc_ns ||
        !decoded.exposure_duration_ns || !decoded.payload_size ||
        decoded.payload_size > kMaximumPayloadSize) {
        return err_protocol;
    }
    *header = decoded;
    return ok;
}

int encode_ack(const ack &value, std::array<std::uint8_t, kAckSize> *wire)
{
    if (!wire || !valid_ack_status(value.status) || value.camera_id > 1U ||
        !value.session_id)
        return err_argument;
    wire->fill(0);
    std::copy(kAckMagic.begin(), kAckMagic.end(), wire->begin());
    (*wire)[4] = kProtocolVersion;
    (*wire)[5] = static_cast<std::uint8_t>(kAckSize);
    (*wire)[6] = static_cast<std::uint8_t>(value.status);
    (*wire)[7] = value.camera_id;
    put_u64(wire->data() + 8, value.session_id);
    put_u64(wire->data() + 16, value.frame_id);
    return ok;
}

int decode_ack(const std::uint8_t *wire, std::size_t wire_size, ack *value)
{
    if (!wire || !value || wire_size != kAckSize)
        return err_argument;
    if (!std::equal(kAckMagic.begin(), kAckMagic.end(), wire) ||
        wire[4] != kProtocolVersion || wire[5] != kAckSize)
        return err_protocol;
    ack decoded;
    decoded.status = static_cast<ack_status>(wire[6]);
    decoded.camera_id = wire[7];
    decoded.session_id = get_u64(wire + 8);
    decoded.frame_id = get_u64(wire + 16);
    if (!valid_ack_status(decoded.status) || decoded.camera_id > 1U ||
        !decoded.session_id)
        return err_protocol;
    *value = decoded;
    return ok;
}

int sha256_fd(int fd, std::array<std::uint8_t, kSha256Size> *digest,
              std::uint64_t *file_size, std::string *error)
{
    if (fd < 0 || !digest || !file_size)
        return err_argument;
    struct stat before = {};
    if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size <= 0 ||
        static_cast<std::uint64_t>(before.st_size) > kMaximumPayloadSize) {
        if (error)
            *error = "file is not a non-empty regular photo within limits";
        return err_argument;
    }

    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (!context || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
        if (context)
            EVP_MD_CTX_free(context);
        if (error)
            *error = "unable to initialize SHA-256";
        return err_hash;
    }

    const std::uint64_t expected_size =
        static_cast<std::uint64_t>(before.st_size);
    std::uint64_t offset = 0;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    int result = ok;
    while (offset < expected_size) {
        const std::size_t wanted = static_cast<std::size_t>(std::min<
            std::uint64_t>(buffer.size(), expected_size - offset));
        ssize_t count;
        do {
            count = pread(fd, buffer.data(), wanted,
                          static_cast<off_t>(offset));
        } while (count < 0 && errno == EINTR);
        if (count <= 0 || static_cast<std::size_t>(count) != wanted) {
            result = err_io;
            break;
        }
        if (EVP_DigestUpdate(context, buffer.data(), wanted) != 1) {
            result = err_hash;
            break;
        }
        offset += wanted;
    }

    unsigned int digest_size = 0;
    if (result == ok &&
        (EVP_DigestFinal_ex(context, digest->data(), &digest_size) != 1 ||
         digest_size != kSha256Size)) {
        result = err_hash;
    }
    EVP_MD_CTX_free(context);
    struct stat after = {};
    if (result == ok &&
        (fstat(fd, &after) != 0 || before.st_dev != after.st_dev ||
         before.st_ino != after.st_ino || before.st_size != after.st_size ||
         before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
         before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
         before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
         before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)) {
        result = err_io;
    }
    if (result != ok) {
        if (error)
            *error = result == err_hash ? "unable to calculate SHA-256"
                                       : std::strerror(errno ? errno : EIO);
        return result;
    }
    *file_size = expected_size;
    if (error)
        error->clear();
    return ok;
}

int sha256_file(const std::string &path,
                std::array<std::uint8_t, kSha256Size> *digest,
                std::uint64_t *file_size, std::string *error)
{
    if (path.empty() || !digest || !file_size)
        return err_argument;
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (error)
            *error = std::strerror(errno);
        return err_io;
    }
    const int result = sha256_fd(fd, digest, file_size, error);
    const int close_result = close(fd);
    if (result == ok && close_result != 0) {
        if (error)
            *error = std::strerror(errno);
        return err_io;
    }
    return result;
}

std::string sha256_hex(
    const std::array<std::uint8_t, kSha256Size> &digest)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string output;
    output.resize(kSha256Size * 2U);
    for (std::size_t index = 0; index < digest.size(); ++index) {
        output[index * 2U] = kHex[digest[index] >> 4U];
        output[index * 2U + 1U] = kHex[digest[index] & 0x0fU];
    }
    return output;
}

const char *format_extension(photo_format format)
{
    switch (format) {
    case photo_format::jpeg:
        return "jpg";
    case photo_format::png:
        return "png";
    case photo_format::nv12:
        return "nv12";
    case photo_format::dng:
        return "dng";
    }
    return "bin";
}

const char *ack_status_string(ack_status status)
{
    switch (status) {
    case ack_status::ok:
        return "OK";
    case ack_status::bad_header:
        return "BAD_HEADER";
    case ack_status::bad_length:
        return "BAD_LENGTH";
    case ack_status::sha256_mismatch:
        return "SHA256_MISMATCH";
    case ack_status::write_error:
        return "WRITE_ERROR";
    case ack_status::unsupported_format:
        return "UNSUPPORTED_FORMAT";
    case ack_status::conflict:
        return "CONFLICT";
    }
    return "UNKNOWN";
}

const char *strerror(int value)
{
    switch (value) {
    case ok:
        return "success";
    case err_argument:
        return "invalid transfer argument";
    case err_protocol:
        return "invalid transfer frame";
    case err_io:
        return "file or socket I/O failed";
    case err_hash:
        return "SHA-256 calculation failed";
    case err_connect:
        return "unable to connect to receiver";
    case err_timeout:
        return "transfer timed out";
    case err_queue_full:
        return "transfer queue is full";
    case err_remote:
        return "receiver rejected the photo";
    case err_conflict:
        return "receiver rejected the photo as a conflicting duplicate";
    default:
        return "unknown transfer error";
    }
}

}  // namespace camera_transfer
