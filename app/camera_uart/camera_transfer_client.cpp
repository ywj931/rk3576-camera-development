#include "camera_transfer_client.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <openssl/evp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace camera_transfer {
namespace {

int send_exact(int socket, const void *data, std::size_t size)
{
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    std::size_t sent = 0;
    while (sent < size) {
        const ssize_t count =
            send(socket, bytes + sent, size - sent, MSG_NOSIGNAL);
        if (count > 0) {
            sent += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return errno == EAGAIN || errno == EWOULDBLOCK ? err_timeout : err_io;
    }
    return ok;
}

int receive_exact(int socket, void *data, std::size_t size)
{
    auto *bytes = static_cast<std::uint8_t *>(data);
    std::size_t received = 0;
    while (received < size) {
        const ssize_t count = recv(socket, bytes + received, size - received, 0);
        if (count > 0) {
            received += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return err_timeout;
        return err_io;
    }
    return ok;
}

void set_error(std::string *error, const char *prefix)
{
    if (!error)
        return;
    *error = prefix;
    if (errno) {
        error->append(": ");
        error->append(std::strerror(errno));
    }
}

}  // namespace

client::client(std::string host, std::uint16_t port, int timeout_ms)
    : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms)
{
}

client::~client()
{
    disconnect();
}

void client::disconnect()
{
    if (socket_ >= 0) {
        shutdown(socket_, SHUT_RDWR);
        close(socket_);
        socket_ = -1;
    }
}

int client::connect_receiver(std::string *error)
{
    if (socket_ >= 0)
        return ok;
    if (host_.empty() || !port_ || timeout_ms_ <= 0)
        return err_argument;

    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    char service[16];
    std::snprintf(service, sizeof(service), "%u", port_);
    struct addrinfo *addresses = nullptr;
    const int lookup = getaddrinfo(host_.c_str(), service, &hints, &addresses);
    if (lookup != 0) {
        if (error)
            *error = gai_strerror(lookup);
        return err_connect;
    }

    int connected = -1;
    for (struct addrinfo *address = addresses; address;
         address = address->ai_next) {
        const int candidate = socket(address->ai_family,
                                     address->ai_socktype | SOCK_CLOEXEC,
                                     address->ai_protocol);
        if (candidate < 0)
            continue;
        const int original_flags = fcntl(candidate, F_GETFL, 0);
        if (original_flags < 0 ||
            fcntl(candidate, F_SETFL, original_flags | O_NONBLOCK) < 0) {
            close(candidate);
            continue;
        }
        int connect_result =
            connect(candidate, address->ai_addr, address->ai_addrlen);
        if (connect_result < 0 && errno == EINPROGRESS) {
            struct pollfd descriptor = {candidate, POLLOUT, 0};
            do {
                connect_result = poll(&descriptor, 1, timeout_ms_);
            } while (connect_result < 0 && errno == EINTR);
            if (connect_result > 0) {
                int socket_error = 0;
                socklen_t socket_error_size = sizeof(socket_error);
                if (getsockopt(candidate, SOL_SOCKET, SO_ERROR, &socket_error,
                               &socket_error_size) == 0 &&
                    socket_error == 0) {
                    connect_result = 0;
                } else {
                    errno = socket_error ? socket_error : ECONNREFUSED;
                    connect_result = -1;
                }
            } else if (connect_result == 0) {
                errno = ETIMEDOUT;
                connect_result = -1;
            }
        }
        if (connect_result == 0 &&
            fcntl(candidate, F_SETFL, original_flags) == 0) {
            connected = candidate;
            break;
        }
        close(candidate);
    }
    freeaddrinfo(addresses);
    if (connected < 0) {
        set_error(error, "connect failed");
        return errno == ETIMEDOUT ? err_timeout : err_connect;
    }

    struct timeval timeout = {};
    timeout.tv_sec = timeout_ms_ / 1000;
    timeout.tv_usec = (timeout_ms_ % 1000) * 1000;
    if (setsockopt(connected, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout)) != 0 ||
        setsockopt(connected, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) != 0) {
        const int saved_errno = errno ? errno : EIO;
        close(connected);
        errno = saved_errno;
        set_error(error, "socket timeout setup failed");
        return err_io;
    }
    socket_ = connected;
    if (error)
        error->clear();
    return ok;
}

int client::send_photo(const send_request &request, ack *response,
                       std::string *error)
{
    if (!response || request.path.empty() || request.camera_id > 1U ||
        !request.session_id || !request.exposure_start_utc_ns ||
        !request.exposure_duration_ns || !supported_format(request.format))
        return err_argument;

    photo_header header;
    header.format = request.format;
    header.camera_id = request.camera_id;
    header.session_id = request.session_id;
    header.frame_id = request.frame_id;
    header.exposure_start_utc_ns = request.exposure_start_utc_ns;
    header.exposure_duration_ns = request.exposure_duration_ns;
    const int file =
        open(request.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file < 0) {
        set_error(error, "open photo failed");
        return err_io;
    }
    int result =
        sha256_fd(file, &header.sha256, &header.payload_size, error);
    if (result != ok) {
        close(file);
        return result;
    }

    std::array<std::uint8_t, kPhotoHeaderSize> wire_header{};
    result = encode_photo_header(header, &wire_header);
    if (result != ok) {
        close(file);
        return result;
    }
    result = connect_receiver(error);
    if (result != ok) {
        close(file);
        return result;
    }
    result = send_exact(socket_, wire_header.data(), wire_header.size());
    std::array<std::uint8_t, 64 * 1024> buffer{};
    std::array<std::uint8_t, kSha256Size> sent_digest{};
    EVP_MD_CTX *sent_hash = EVP_MD_CTX_new();
    if (!sent_hash || EVP_DigestInit_ex(sent_hash, EVP_sha256(), nullptr) != 1)
        result = err_hash;
    std::uint64_t sent = 0;
    while (result == ok && sent < header.payload_size) {
        const std::size_t wanted = static_cast<std::size_t>(std::min<
            std::uint64_t>(buffer.size(), header.payload_size - sent));
        ssize_t count;
        do {
            count = pread(file, buffer.data(), wanted,
                          static_cast<off_t>(sent));
        } while (count < 0 && errno == EINTR);
        if (count <= 0 || static_cast<std::size_t>(count) != wanted ||
            EVP_DigestUpdate(sent_hash, buffer.data(), wanted) != 1) {
            result = err_io;
            break;
        }
        result = send_exact(socket_, buffer.data(), wanted);
        sent += wanted;
    }
    unsigned int sent_digest_size = 0;
    if (result == ok &&
        (EVP_DigestFinal_ex(sent_hash, sent_digest.data(),
                            &sent_digest_size) != 1 ||
         sent_digest_size != kSha256Size || sent_digest != header.sha256)) {
        result = err_hash;
    }
    if (sent_hash)
        EVP_MD_CTX_free(sent_hash);
    if (close(file) != 0 && result == ok)
        result = err_io;
    if (result != ok) {
        if (error && result == err_hash)
            *error = "photo changed while it was being transmitted";
        else
            set_error(error, "send photo failed");
        disconnect();
        return result;
    }

    std::array<std::uint8_t, kAckSize> wire_ack{};
    result = receive_exact(socket_, wire_ack.data(), wire_ack.size());
    if (result == ok)
        result = decode_ack(wire_ack.data(), wire_ack.size(), response);
    if (result != ok) {
        set_error(error, "receive ACK failed");
        disconnect();
        return result;
    }
    if (response->session_id != request.session_id ||
        response->camera_id != request.camera_id ||
        response->frame_id != request.frame_id) {
        if (error)
            *error = "ACK identity does not match the photo";
        disconnect();
        return err_protocol;
    }
    if (response->status != ack_status::ok) {
        if (error)
            *error = ack_status_string(response->status);
        return err_remote;
    }
    if (error)
        error->clear();
    return ok;
}

}  // namespace camera_transfer
