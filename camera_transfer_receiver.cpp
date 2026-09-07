#include "camera_transfer_protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace {

volatile sig_atomic_t g_stop = 0;
constexpr int kClientIdleTimeoutSeconds = 2;

void handle_signal(int)
{
    g_stop = 1;
}

bool parse_u16(const char *text, std::uint16_t *value)
{
    if (!text || !*text || !value || text[0] == '-')
        return false;
    errno = 0;
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (errno || end == text || *end || !parsed || parsed > UINT16_MAX)
        return false;
    *value = static_cast<std::uint16_t>(parsed);
    return true;
}

int make_directories(const std::string &path)
{
    if (path.empty() || path[0] != '/')
        return EINVAL;
    std::string current;
    for (std::size_t index = 0; index < path.size(); ++index) {
        current.push_back(path[index]);
        if (path[index] != '/' || current.size() == 1U)
            continue;
        while (current.size() > 1U && current.back() == '/')
            current.pop_back();
        if (mkdir(current.c_str(), 0755) < 0 && errno != EEXIST)
            return errno;
        current.push_back('/');
    }
    while (current.size() > 1U && current.back() == '/')
        current.pop_back();
    if (mkdir(current.c_str(), 0755) < 0 && errno != EEXIST)
        return errno;
    struct stat information = {};
    if (stat(current.c_str(), &information) < 0 ||
        !S_ISDIR(information.st_mode))
        return ENOTDIR;
    return 0;
}

int receive_exact(int socket, void *data, std::size_t size, bool *clean_eof)
{
    auto *bytes = static_cast<std::uint8_t *>(data);
    std::size_t received = 0;
    if (clean_eof)
        *clean_eof = false;
    while (received < size) {
        const ssize_t count = recv(socket, bytes + received, size - received, 0);
        if (count > 0) {
            received += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            if (g_stop)
                return camera_transfer::err_io;
            continue;
        }
        if (count == 0 && received == 0 && clean_eof)
            *clean_eof = true;
        return camera_transfer::err_io;
    }
    return camera_transfer::ok;
}

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
        if (count < 0 && errno == EINTR) {
            if (g_stop)
                return camera_transfer::err_io;
            continue;
        }
        return camera_transfer::err_io;
    }
    return camera_transfer::ok;
}

std::string identity_base(const std::string &directory,
                          const camera_transfer::photo_header &header)
{
    /* Name by frame_id alone: senders put the exposure-start unix
       nanosecond timestamp in frame_id, so the received photo lands as
       "<unix-ns>.jpg" and re-sends of the same exposure collide on the
       same path (letting the conflict check catch changed payloads). */
    char name[64];
    std::snprintf(name, sizeof(name), "/%020llu",
                  static_cast<unsigned long long>(header.frame_id));
    return directory + name;
}

bool write_all(int fd, const void *data, std::size_t size)
{
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    std::size_t written = 0;
    while (written < size) {
        const ssize_t count = write(fd, bytes + written, size - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

bool existing_photo_matches(
    const std::string &path, const camera_transfer::photo_header &header)
{
    std::array<std::uint8_t, camera_transfer::kSha256Size> hash{};
    std::uint64_t size = 0;
    std::string error;
    return camera_transfer::sha256_file(path, &hash, &size, &error) ==
               camera_transfer::ok &&
           size == header.payload_size && hash == header.sha256;
}

bool metadata_matches(
    const std::string &path,
    const std::array<std::uint8_t, camera_transfer::kPhotoHeaderSize> &wire)
{
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return false;
    struct stat information = {};
    std::array<std::uint8_t, camera_transfer::kPhotoHeaderSize> existing{};
    const bool valid = fstat(fd, &information) == 0 &&
                       S_ISREG(information.st_mode) &&
                       information.st_size ==
                           static_cast<off_t>(existing.size()) &&
                       pread(fd, existing.data(), existing.size(), 0) ==
                           static_cast<ssize_t>(existing.size()) &&
                       existing == wire;
    close(fd);
    return valid;
}

int sync_regular_file(const std::string &path)
{
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return errno ? errno : EIO;
    struct stat information = {};
    int result = 0;
    if (fstat(fd, &information) != 0 || !S_ISREG(information.st_mode))
        result = errno ? errno : EINVAL;
    else if (fsync(fd) != 0)
        result = errno ? errno : EIO;
    if (close(fd) != 0 && !result)
        result = errno ? errno : EIO;
    return result;
}

bool publish_metadata(const std::string &path,
                      const camera_transfer::photo_header &header,
                      bool durable, std::string *detail)
{
    std::array<std::uint8_t, camera_transfer::kPhotoHeaderSize> wire{};
    if (camera_transfer::encode_photo_header(header, &wire) !=
        camera_transfer::ok) {
        if (detail)
            *detail = "unable to encode metadata sidecar";
        return false;
    }
    if (metadata_matches(path, wire)) {
        const int sync_error = durable ? sync_regular_file(path) : 0;
        if (sync_error && detail)
            *detail = std::strerror(sync_error);
        return sync_error == 0;
    }

    const std::string temporary_template = path + ".part.XXXXXX";
    std::vector<char> mutable_template(temporary_template.begin(),
                                       temporary_template.end());
    mutable_template.push_back('\0');
    const int fd = mkstemp(mutable_template.data());
    if (fd < 0) {
        if (detail)
            *detail = std::strerror(errno);
        return false;
    }
    const std::string temporary_path = mutable_template.data();
    int metadata_error = 0;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 || fchmod(fd, 0644) != 0 ||
        !write_all(fd, wire.data(), wire.size()) ||
        (durable && fsync(fd) != 0)) {
        metadata_error = errno ? errno : EIO;
    }
    if (close(fd) != 0 && !metadata_error)
        metadata_error = errno ? errno : EIO;
    if (!metadata_error && link(temporary_path.c_str(), path.c_str()) != 0) {
        const int link_error = errno;
        if (link_error != EEXIST || !metadata_matches(path, wire))
            metadata_error = link_error ? link_error : EEXIST;
    }
    if (unlink(temporary_path.c_str()) != 0 && !metadata_error)
        metadata_error = errno ? errno : EIO;
    if (metadata_error) {
        if (detail)
            *detail = metadata_error == EEXIST
                          ? "photo identity metadata conflicts with existing file"
                          : std::strerror(metadata_error);
        return false;
    }
    return true;
}

int sync_directory(const std::string &directory)
{
    const int fd =
        open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return errno;
    const int result = fsync(fd);
    const int saved_errno = result == 0 ? 0 : (errno ? errno : EIO);
    close(fd);
    return saved_errno;
}

int create_listener(const std::string &bind_address, std::uint16_t port,
                    std::string *error)
{
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;
    char service[16];
    std::snprintf(service, sizeof(service), "%u", port);
    struct addrinfo *addresses = nullptr;
    const char *node = bind_address == "*" ? nullptr : bind_address.c_str();
    const int lookup = getaddrinfo(node, service, &hints, &addresses);
    if (lookup != 0) {
        if (error)
            *error = gai_strerror(lookup);
        return -1;
    }
    int listener = -1;
    for (struct addrinfo *address = addresses; address;
         address = address->ai_next) {
        const int candidate = socket(address->ai_family,
                                     address->ai_socktype | SOCK_CLOEXEC,
                                     address->ai_protocol);
        if (candidate < 0)
            continue;
        int reuse = 1;
        setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (bind(candidate, address->ai_addr, address->ai_addrlen) == 0 &&
            listen(candidate, 4) == 0) {
            listener = candidate;
            break;
        }
        close(candidate);
    }
    freeaddrinfo(addresses);
    if (listener < 0 && error)
        *error = std::strerror(errno);
    return listener;
}

camera_transfer::ack_status receive_photo(
    int socket, const camera_transfer::photo_header &header,
    const std::string &directory, bool durable, std::string *saved_path,
    std::string *detail)
{
    if (header.format != camera_transfer::photo_format::jpeg) {
        std::array<std::uint8_t, 64 * 1024> discard{};
        std::uint64_t remaining = header.payload_size;
        while (remaining) {
            const std::size_t wanted = static_cast<std::size_t>(
                std::min<std::uint64_t>(discard.size(), remaining));
            bool clean_eof = false;
            if (receive_exact(socket, discard.data(), wanted, &clean_eof) !=
                camera_transfer::ok) {
                if (detail)
                    *detail = "connection closed while discarding unsupported format";
                return camera_transfer::ack_status::bad_length;
            }
            remaining -= wanted;
        }
        if (detail)
            *detail = "V1 receiver currently accepts JPEG only";
        return camera_transfer::ack_status::unsupported_format;
    }

    const std::string base = identity_base(directory, header);
    const std::string final_path = base + ".jpg";
    const std::string metadata_path = base + ".meta";
    const std::string temporary_template = final_path + ".part.XXXXXX";
    std::vector<char> mutable_template(temporary_template.begin(),
                                       temporary_template.end());
    mutable_template.push_back('\0');
    int file = mkstemp(mutable_template.data());
    const std::string temporary_path =
        file >= 0 ? mutable_template.data() : temporary_template;
    bool write_failed = file < 0;
    int write_error = write_failed ? errno : 0;
    if (file >= 0 &&
        (fcntl(file, F_SETFD, FD_CLOEXEC) != 0 || fchmod(file, 0644) != 0)) {
        write_error = errno ? errno : EIO;
        close(file);
        file = -1;
        unlink(temporary_path.c_str());
        write_failed = true;
    }
    std::array<std::uint8_t, 64 * 1024> buffer{};
    std::uint64_t remaining = header.payload_size;
    while (remaining) {
        const std::size_t wanted = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), remaining));
        bool clean_eof = false;
        if (receive_exact(socket, buffer.data(), wanted, &clean_eof) !=
            camera_transfer::ok) {
            if (file >= 0)
                close(file);
            unlink(temporary_path.c_str());
            if (detail)
                *detail = "connection closed before data_length bytes";
            return camera_transfer::ack_status::bad_length;
        }
        if (!write_failed) {
            std::size_t written = 0;
            while (written < wanted) {
                const ssize_t count =
                    write(file, buffer.data() + written, wanted - written);
                if (count > 0) {
                    written += static_cast<std::size_t>(count);
                    continue;
                }
                if (count < 0 && errno == EINTR)
                    continue;
                write_failed = true;
                write_error = errno ? errno : EIO;
                break;
            }
        }
        remaining -= wanted;
    }

    if (file >= 0) {
        if (!write_failed && durable && fsync(file) != 0) {
            write_failed = true;
            write_error = errno ? errno : EIO;
        }
        if (close(file) != 0 && !write_failed) {
            write_failed = true;
            write_error = errno ? errno : EIO;
        }
    }
    if (write_failed) {
        unlink(temporary_path.c_str());
        if (detail)
            *detail = std::strerror(write_error);
        return camera_transfer::ack_status::write_error;
    }

    std::array<std::uint8_t, camera_transfer::kSha256Size> actual_hash{};
    std::uint64_t actual_size = 0;
    std::string hash_error;
    if (camera_transfer::sha256_file(temporary_path, &actual_hash, &actual_size,
                                     &hash_error) != camera_transfer::ok) {
        unlink(temporary_path.c_str());
        if (detail)
            *detail = hash_error;
        return camera_transfer::ack_status::write_error;
    }
    if (actual_size != header.payload_size) {
        unlink(temporary_path.c_str());
        if (detail)
            *detail = "saved length differs from data_length";
        return camera_transfer::ack_status::bad_length;
    }
    if (actual_hash != header.sha256) {
        unlink(temporary_path.c_str());
        if (detail)
            *detail = "saved SHA-256 differs from sender";
        return camera_transfer::ack_status::sha256_mismatch;
    }
    bool photo_created = false;
    if (link(temporary_path.c_str(), final_path.c_str()) != 0) {
        const int link_error = errno;
        const bool duplicate = link_error == EEXIST &&
                               existing_photo_matches(final_path, header);
        unlink(temporary_path.c_str());
        if (!duplicate) {
            if (link_error == EEXIST) {
                /* Same photo identity but different data: retrying with
                   this identity can never succeed, so report a permanent
                   conflict instead of a transient write error. */
                if (detail)
                    *detail =
                        "photo identity already exists with different data";
                return camera_transfer::ack_status::conflict;
            }
            if (detail)
                *detail = std::strerror(link_error);
            return camera_transfer::ack_status::write_error;
        }
        if (durable) {
            const int sync_error = sync_regular_file(final_path);
            if (sync_error) {
                if (detail)
                    *detail = std::strerror(sync_error);
                return camera_transfer::ack_status::write_error;
            }
        }
    } else {
        photo_created = true;
        if (unlink(temporary_path.c_str()) != 0) {
            const int unlink_error = errno ? errno : EIO;
            unlink(final_path.c_str());
            if (detail)
                *detail = std::strerror(unlink_error);
            return camera_transfer::ack_status::write_error;
        }
    }
    if (!publish_metadata(metadata_path, header, durable, detail)) {
        if (photo_created)
            unlink(final_path.c_str());
        return camera_transfer::ack_status::write_error;
    }
    if (durable) {
        const int directory_error = sync_directory(directory);
        if (directory_error) {
            if (detail)
                *detail = std::strerror(directory_error);
            return camera_transfer::ack_status::write_error;
        }
    }
    if (!existing_photo_matches(final_path, header)) {
        if (detail)
            *detail = "published photo changed before ACK";
        return camera_transfer::ack_status::write_error;
    }
    if (saved_path)
        *saved_path = final_path;
    if (detail)
        detail->clear();
    return camera_transfer::ack_status::ok;
}

int handle_client(int socket, const std::string &directory, bool durable,
                  bool once)
{
    for (;;) {
        std::array<std::uint8_t, camera_transfer::kPhotoHeaderSize> wire{};
        bool clean_eof = false;
        if (receive_exact(socket, wire.data(), wire.size(), &clean_eof) !=
            camera_transfer::ok)
            return clean_eof || g_stop ? 0 : 1;
        camera_transfer::photo_header header;
        if (camera_transfer::decode_photo_header(wire.data(), wire.size(),
                                                 &header) !=
            camera_transfer::ok) {
            std::fprintf(stderr, "TRANSFER_BAD_HEADER\n");
            return 1;
        }

        std::string path;
        std::string detail;
        const camera_transfer::ack_status status =
            receive_photo(socket, header, directory, durable, &path, &detail);
        camera_transfer::ack response;
        response.status = status;
        response.camera_id = header.camera_id;
        response.session_id = header.session_id;
        response.frame_id = header.frame_id;
        std::array<std::uint8_t, camera_transfer::kAckSize> ack_wire{};
        if (camera_transfer::encode_ack(response, &ack_wire) !=
                camera_transfer::ok ||
            send_exact(socket, ack_wire.data(), ack_wire.size()) !=
                camera_transfer::ok) {
            return 1;
        }
        if (status == camera_transfer::ack_status::ok) {
            std::printf(
                "PHOTO_RECEIVED session=%016llx camera_id=%u frame_id=%llu "
                "bytes=%llu sha256=%s path=\"%s\"\n",
                static_cast<unsigned long long>(header.session_id),
                header.camera_id,
                static_cast<unsigned long long>(header.frame_id),
                static_cast<unsigned long long>(header.payload_size),
                camera_transfer::sha256_hex(header.sha256).c_str(), path.c_str());
            std::fflush(stdout);
            if (once)
                return 0;
        } else {
            std::fprintf(
                stderr,
                "PHOTO_REJECTED session=%016llx camera_id=%u frame_id=%llu "
                "status=%s detail=\"%s\"\n",
                static_cast<unsigned long long>(header.session_id),
                header.camera_id,
                static_cast<unsigned long long>(header.frame_id),
                camera_transfer::ack_status_string(status), detail.c_str());
        }
    }
}

void usage(const char *program)
{
    std::fprintf(stderr,
                 "usage: %s OUTPUT_DIR [PORT [BIND_ADDRESS]] [--once] "
                 "[--durable]\n",
                 program);
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 6) {
        usage(argv[0]);
        return 2;
    }
    const std::string output_directory = argv[1];
    std::uint16_t port = camera_transfer::kDefaultPort;
    std::string bind_address = "*";
    int positional = 0;
    bool once = false;
    bool durable = false;
    for (int index = 2; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--once") {
            once = true;
        } else if (option == "--durable") {
            durable = true;
        } else if (positional == 0) {
            if (!parse_u16(argv[index], &port)) {
                usage(argv[0]);
                return 2;
            }
            ++positional;
        } else if (positional == 1) {
            bind_address = option;
            ++positional;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    const int directory_error = make_directories(output_directory);
    if (directory_error) {
        std::fprintf(stderr, "OUTPUT_DIRECTORY_FAILED reason=\"%s\"\n",
                     std::strerror(directory_error));
        return 1;
    }
    std::string listen_error;
    const int listener = create_listener(bind_address, port, &listen_error);
    if (listener < 0) {
        std::fprintf(stderr, "TRANSFER_LISTEN_FAILED reason=\"%s\"\n",
                     listen_error.c_str());
        return 1;
    }
    struct sigaction action = {};
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    std::signal(SIGPIPE, SIG_IGN);
    std::printf(
        "TRANSFER_RECEIVER_READY bind=%s port=%u output=\"%s\" durable=%d\n",
        bind_address.c_str(), port, output_directory.c_str(), durable ? 1 : 0);
    std::fflush(stdout);

    int result = 0;
    while (!g_stop) {
        const int connection = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (connection >= 0) {
            struct timeval timeout = {};
            timeout.tv_sec = kClientIdleTimeoutSeconds;
            if (setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                           sizeof(timeout)) != 0 ||
                setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                           sizeof(timeout)) != 0) {
                std::fprintf(stderr,
                             "TRANSFER_CLIENT_SETUP_FAILED reason=\"%s\"\n",
                             std::strerror(errno));
                close(connection);
                result = 1;
                if (once)
                    break;
                continue;
            }
            result = handle_client(connection, output_directory, durable, once);
            close(connection);
            if (once)
                break;
            continue;
        }
        if (errno == EINTR && !g_stop)
            continue;
        result = g_stop ? 0 : 1;
        break;
    }
    close(listener);
    return result;
}
