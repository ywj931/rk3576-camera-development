#include "stitch_transfer_worker.h"

#include "camera_transfer_protocol.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace std::chrono_literals;

bool receive_exact(int socket, void *data, std::size_t size)
{
    auto *bytes = static_cast<std::uint8_t *>(data);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t count = recv(socket, bytes + offset, size - offset, 0);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool send_exact(int socket, const void *data, std::size_t size)
{
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t count =
            send(socket, bytes + offset, size - offset, MSG_NOSIGNAL);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

int create_listener(std::uint16_t *port, std::uint16_t requested_port = 0)
{
    const int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0)
        return -1;
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(requested_port);
    if (bind(listener, reinterpret_cast<const struct sockaddr *>(&address),
             sizeof(address)) != 0 ||
        listen(listener, 4) != 0) {
        close(listener);
        return -1;
    }
    socklen_t address_size = sizeof(address);
    if (getsockname(listener, reinterpret_cast<struct sockaddr *>(&address),
                    &address_size) != 0) {
        close(listener);
        return -1;
    }
    *port = ntohs(address.sin_port);
    return listener;
}

int accept_with_timeout(int listener, int timeout_ms = 5000)
{
    struct pollfd descriptor = {listener, POLLIN, 0};
    int result;
    do {
        result = poll(&descriptor, 1, timeout_ms);
    } while (result < 0 && errno == EINTR);
    if (result <= 0)
        return -1;
    const int connection = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    if (connection >= 0) {
        struct timeval timeout = {3, 0};
        setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout));
        setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout));
    }
    return connection;
}

bool receive_request(int socket, camera_transfer::photo_header *header,
                     std::vector<std::uint8_t> *payload)
{
    std::array<std::uint8_t, camera_transfer::kPhotoHeaderSize> wire{};
    if (!receive_exact(socket, wire.data(), wire.size()) ||
        camera_transfer::decode_photo_header(wire.data(), wire.size(), header) !=
            camera_transfer::ok) {
        return false;
    }
    payload->resize(static_cast<std::size_t>(header->payload_size));
    return receive_exact(socket, payload->data(), payload->size());
}

bool send_ok_ack(int socket, const camera_transfer::photo_header &header)
{
    camera_transfer::ack response;
    response.status = camera_transfer::ack_status::ok;
    response.camera_id = header.camera_id;
    response.session_id = header.session_id;
    response.frame_id = header.frame_id;
    std::array<std::uint8_t, camera_transfer::kAckSize> wire{};
    return camera_transfer::encode_ack(response, &wire) == camera_transfer::ok &&
           send_exact(socket, wire.data(), wire.size());
}

bool serve_acked_requests(int listener, std::size_t expected,
                          std::vector<std::uint64_t> *frame_ids = nullptr)
{
    int connection = -1;
    std::size_t received = 0;
    while (received < expected) {
        if (connection < 0)
            connection = accept_with_timeout(listener);
        if (connection < 0)
            return false;
        camera_transfer::photo_header header;
        std::vector<std::uint8_t> payload;
        if (!receive_request(connection, &header, &payload)) {
            close(connection);
            connection = -1;
            continue;
        }
        if (!send_ok_ack(connection, header)) {
            close(connection);
            return false;
        }
        if (frame_ids)
            frame_ids->push_back(header.frame_id);
        ++received;
    }
    if (connection >= 0)
        close(connection);
    return true;
}

bool wait_until(const std::function<bool()> &predicate, int timeout_ms = 5000)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate())
            return true;
        std::this_thread::sleep_for(10ms);
    }
    return predicate();
}

std::string create_directory()
{
    char path[] = "/tmp/stitch_transfer_worker.XXXXXX";
    return mkdtemp(path) ? path : std::string{};
}

bool write_file(const std::string &path,
                const std::vector<std::uint8_t> &payload)
{
    const int file = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                          0600);
    if (file < 0)
        return false;
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const ssize_t count =
            write(file, payload.data() + offset, payload.size() - offset);
        if (count > 0)
            offset += static_cast<std::size_t>(count);
        else if (count < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    const bool success = offset == payload.size() && fsync(file) == 0 &&
                         close(file) == 0;
    if (!success) {
        close(file);
        unlink(path.c_str());
    }
    return success;
}

std::vector<std::uint8_t> read_file(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

std::uint64_t file_size(const std::string &path)
{
    struct stat info = {};
    return stat(path.c_str(), &info) == 0
               ? static_cast<std::uint64_t>(info.st_size)
               : 0;
}

camera_stitch_transfer::item make_item(const std::string &path,
                                       std::uint64_t pair_id)
{
    camera_stitch_transfer::item work;
    work.path = path;
    work.pair_id = pair_id;
    work.exposure_start_utc_ns = 1710000000000000000ULL + pair_id;
    work.exposure_duration_ns = 7000000ULL;
    work.payload_size = file_size(path);
    return work;
}

std::string stitch_path(const std::string &directory, std::uint64_t pair_id,
                        std::uint64_t timestamp_ns)
{
    char name[256];
    std::snprintf(name, sizeof(name),
                  "%s/stitch_pair_%020llu_top_%010u_bottom_%010u_%llu.jpg",
                  directory.c_str(),
                  static_cast<unsigned long long>(pair_id), 10U, 20U,
                  static_cast<unsigned long long>(timestamp_ns));
    return name;
}

std::vector<std::uint8_t> jpeg_with_composite_comment(
    std::uint64_t exposure_start_ns, std::uint64_t exposure_us)
{
    std::string comment =
        "layout=vertical;pair_id=88;composite_capture_realtime_ns=" +
        std::to_string(exposure_start_ns) +
        ";top={camera_id=0;exposure_start_realtime_ns=" +
        std::to_string(exposure_start_ns) + ";exposure_us=" +
        std::to_string(exposure_us) +
        ";iso=200};bottom={camera_id=1;exposure_us=8000}";
    std::vector<std::uint8_t> output = {0xff, 0xd8, 0xff, 0xe1};
    output.insert(output.end(), comment.begin(), comment.end());
    output.push_back(0xff);
    output.push_back(0xd9);
    return output;
}

bool write_metadata(const std::string &path, const std::string &jpeg_path,
                    std::uint64_t pair_id, std::uint64_t exposure_start_ns,
                    std::uint64_t exposure_us)
{
    std::ofstream csv(path);
    csv << "jpeg_path,top_exposure_us,pair_id,unused,"
           "top_exposure_start_realtime_ns\n";
    csv << jpeg_path << ',' << exposure_us << ',' << pair_id << ",x,"
        << exposure_start_ns << '\n';
    csv.flush();
    return csv.good();
}

bool test_retry_then_ack()
{
    const std::vector<std::uint8_t> payload = {0xff, 0xd8, 'a', 0xff, 0xd9};
    const std::string directory = create_directory();
    const std::string path = directory + "/single.jpg";
    std::uint16_t port = 0;
    const int listener = create_listener(&port);
    if (directory.empty() || !write_file(path, payload) || listener < 0)
        return false;

    std::atomic<bool> server_ok{true};
    std::thread server([&] {
        camera_transfer::photo_header first;
        std::vector<std::uint8_t> first_payload;
        int connection = accept_with_timeout(listener);
        if (connection < 0 ||
            !receive_request(connection, &first, &first_payload)) {
            server_ok = false;
        }
        if (connection >= 0)
            close(connection);

        camera_transfer::photo_header second;
        std::vector<std::uint8_t> second_payload;
        connection = accept_with_timeout(listener);
        if (connection < 0 ||
            !receive_request(connection, &second, &second_payload) ||
            first.session_id != second.session_id ||
            first.frame_id != second.frame_id || first_payload != payload ||
            second_payload != payload || !send_ok_ack(connection, second)) {
            server_ok = false;
        }
        if (connection >= 0)
            close(connection);
        close(listener);
    });

    camera_stitch_transfer::worker transfer(4, 500, 3, 1500);
    const bool success =
        transfer.start("127.0.0.1", port) == camera_stitch_transfer::ok &&
        transfer.enqueue(make_item(path, 42)) == camera_stitch_transfer::ok &&
        wait_until([&] { return transfer.get_status().delivered == 1; }) &&
        transfer.stop() == camera_stitch_transfer::ok;
    server.join();
    const camera_stitch_transfer::status status = transfer.get_status();
    const bool result = success && server_ok && status.retries >= 1 &&
                        status.failed == 0 && status.backlog == 0 &&
                        access(path.c_str(), F_OK) != 0;
    unlink(path.c_str());
    rmdir(directory.c_str());
    return result;
}

bool test_outage_backlog_recovery()
{
    const std::vector<std::uint8_t> payload = {0xff, 0xd8, 'q', 0xff, 0xd9};
    const std::string directory = create_directory();
    std::uint16_t port = 0;
    const int reservation = create_listener(&port);
    if (directory.empty() || reservation < 0)
        return false;
    close(reservation);

    std::vector<std::string> paths;
    for (std::uint64_t index = 0; index < 6; ++index) {
        const std::string path =
            directory + "/queued_" + std::to_string(index) + ".jpg";
        if (!write_file(path, payload))
            return false;
        paths.push_back(path);
    }

    camera_stitch_transfer::worker transfer(2, 200, 0, 500);
    if (transfer.start("127.0.0.1", port) != camera_stitch_transfer::ok)
        return false;
    for (std::size_t index = 0; index < paths.size(); ++index) {
        if (transfer.enqueue(make_item(paths[index], 100 + index)) !=
            camera_stitch_transfer::ok) {
            return false;
        }
    }
    if (!wait_until([&] { return transfer.get_status().deferred > 0; }, 3000))
        return false;

    std::uint16_t rebound_port = port;
    const int listener = create_listener(&rebound_port, port);
    if (listener < 0)
        return false;
    std::atomic<bool> server_ok{true};
    std::thread server([&] {
        server_ok = serve_acked_requests(listener, paths.size());
        close(listener);
    });
    const bool drained = wait_until(
        [&] { return transfer.get_status().delivered == paths.size(); }, 10000);
    const int stop_result = transfer.stop();
    server.join();
    const camera_stitch_transfer::status status = transfer.get_status();
    bool files_deleted = true;
    for (const std::string &path : paths) {
        files_deleted = files_deleted && access(path.c_str(), F_OK) != 0;
        unlink(path.c_str());
    }
    rmdir(directory.c_str());
    return drained && stop_result == camera_stitch_transfer::ok && server_ok &&
           files_deleted && status.enqueued == paths.size() &&
           status.delivered == paths.size() && status.queue_rejected == 0 &&
           status.failed == 0 && status.backlog == 0 &&
           status.backlog_peak >= paths.size() && status.deferred > 0 &&
           status.recovered > 0;
}

bool test_spool_and_orphan_recovery()
{
    const std::string directory = create_directory();
    const std::uint64_t csv_timestamp = 1710000000123000000ULL;
    const std::uint64_t orphan_timestamp = 1710000000456000000ULL;
    const std::string csv_jpeg = stitch_path(directory, 77, csv_timestamp);
    const std::string orphan_jpeg =
        stitch_path(directory, 88, orphan_timestamp);
    const std::string bad_orphan = stitch_path(directory, 99, orphan_timestamp);
    const std::vector<std::uint8_t> plain = {0xff, 0xd8, 'c', 0xff, 0xd9};
    if (directory.empty() || !write_file(csv_jpeg, plain) ||
        !write_file(orphan_jpeg,
                    jpeg_with_composite_comment(orphan_timestamp, 7000)) ||
        !write_file(bad_orphan, plain) ||
        !write_metadata(directory + "/stitch_metadata.csv", csv_jpeg, 77,
                        csv_timestamp, 6000) ||
        !write_metadata(directory + "/stitch_metadata_v2.csv", csv_jpeg, 77,
                        csv_timestamp, 6000)) {
        return false;
    }

    std::uint16_t port = 0;
    const int listener = create_listener(&port);
    if (listener < 0)
        return false;
    std::atomic<bool> server_ok{true};
    std::vector<std::uint64_t> frame_ids;
    std::thread server([&] {
        server_ok = serve_acked_requests(listener, 2, &frame_ids);
        close(listener);
    });

    camera_stitch_transfer::worker transfer(1, 500, 1, 1500);
    const bool started = transfer.start("127.0.0.1", port, directory) ==
                         camera_stitch_transfer::ok;
    const bool delivered =
        started && wait_until([&] { return transfer.get_status().delivered == 2; });
    const int stop_result = transfer.stop();
    server.join();
    const camera_stitch_transfer::status status = transfer.get_status();
    std::sort(frame_ids.begin(), frame_ids.end());
    const bool result =
        delivered && stop_result == camera_stitch_transfer::ok && server_ok &&
        frame_ids ==
            std::vector<std::uint64_t>({77, orphan_timestamp}) &&
        status.spool_recovered == 2 && status.orphan_recovered == 1 &&
        status.orphan_unrecoverable == 1 && status.recovered == 2 &&
        status.backlog == 0 && access(csv_jpeg.c_str(), F_OK) != 0 &&
        access(orphan_jpeg.c_str(), F_OK) != 0 &&
        access(bad_orphan.c_str(), F_OK) == 0;

    unlink(csv_jpeg.c_str());
    unlink(orphan_jpeg.c_str());
    unlink(bad_orphan.c_str());
    unlink((directory + "/stitch_metadata.csv").c_str());
    unlink((directory + "/stitch_metadata_v2.csv").c_str());
    rmdir(directory.c_str());
    return result;
}

bool test_ack_does_not_delete_replacement()
{
    const std::string directory = create_directory();
    const std::string path = directory + "/replace.jpg";
    const std::string temporary = directory + "/new.tmp";
    const std::vector<std::uint8_t> original = {0xff, 0xd8, 'o', 0xff, 0xd9};
    const std::vector<std::uint8_t> replacement = {0xff, 0xd8, 'n', 'e', 'w',
                                                   0xff, 0xd9};
    std::uint16_t port = 0;
    const int listener = create_listener(&port);
    if (directory.empty() || !write_file(path, original) || listener < 0)
        return false;

    std::mutex gate_mutex;
    std::condition_variable gate;
    bool request_received = false;
    bool replacement_ready = false;
    std::atomic<bool> server_ok{true};
    std::thread server([&] {
        const int connection = accept_with_timeout(listener);
        camera_transfer::photo_header header;
        std::vector<std::uint8_t> payload;
        if (connection < 0 || !receive_request(connection, &header, &payload) ||
            payload != original) {
            server_ok = false;
        } else {
            std::unique_lock<std::mutex> lock(gate_mutex);
            request_received = true;
            gate.notify_all();
            gate.wait_for(lock, 3s, [&] { return replacement_ready; });
            if (!replacement_ready || !send_ok_ack(connection, header))
                server_ok = false;
        }
        if (connection >= 0)
            close(connection);
        close(listener);
    });

    camera_stitch_transfer::worker transfer(4, 500, 0, 1000);
    bool success =
        transfer.start("127.0.0.1", port) == camera_stitch_transfer::ok &&
        transfer.enqueue(make_item(path, 123)) == camera_stitch_transfer::ok;
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        success = success && gate.wait_for(lock, 3s, [&] {
            return request_received;
        });
    }
    if (success && write_file(temporary, replacement) &&
        rename(temporary.c_str(), path.c_str()) == 0) {
        std::lock_guard<std::mutex> lock(gate_mutex);
        replacement_ready = true;
        gate.notify_all();
    } else {
        success = false;
        std::lock_guard<std::mutex> lock(gate_mutex);
        replacement_ready = true;
        gate.notify_all();
    }
    success = success &&
              wait_until([&] { return transfer.get_status().delivered == 1; });
    const int stop_result = transfer.stop();
    server.join();
    const camera_stitch_transfer::status status = transfer.get_status();
    const bool result = success && server_ok &&
                        stop_result == camera_stitch_transfer::ok &&
                        status.delete_errors == 1 && status.backlog == 0 &&
                        read_file(path) == replacement;
    unlink(temporary.c_str());
    unlink(path.c_str());
    rmdir(directory.c_str());
    return result;
}

bool test_bounded_stop_and_restart()
{
    const std::string directory = create_directory();
    const std::string path = directory + "/blocked.jpg";
    const std::vector<std::uint8_t> payload = {0xff, 0xd8, 'b', 0xff, 0xd9};
    std::uint16_t blocked_port = 0;
    const int blocked_listener = create_listener(&blocked_port);
    if (directory.empty() || !write_file(path, payload) || blocked_listener < 0)
        return false;

    std::mutex accepted_mutex;
    std::condition_variable accepted_condition;
    bool accepted = false;
    std::thread blocked_server([&] {
        const int connection = accept_with_timeout(blocked_listener);
        camera_transfer::photo_header header;
        std::vector<std::uint8_t> received;
        if (connection >= 0 && receive_request(connection, &header, &received)) {
            {
                std::lock_guard<std::mutex> lock(accepted_mutex);
                accepted = true;
                accepted_condition.notify_all();
            }
            std::this_thread::sleep_for(1200ms);
        }
        if (connection >= 0)
            close(connection);
        close(blocked_listener);
    });

    camera_stitch_transfer::worker transfer(4, 800, 0, 100);
    bool success = transfer.start("127.0.0.1", blocked_port) ==
                       camera_stitch_transfer::ok &&
                   transfer.enqueue(make_item(path, 501)) ==
                       camera_stitch_transfer::ok;
    {
        std::unique_lock<std::mutex> lock(accepted_mutex);
        success = success && accepted_condition.wait_for(lock, 3s, [&] {
            return accepted;
        });
    }
    const auto stop_started = std::chrono::steady_clock::now();
    const int first_stop = transfer.stop();
    const auto stop_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - stop_started);
    const int raced_start = transfer.start("127.0.0.1", blocked_port);
    blocked_server.join();
    success = success && first_stop == camera_stitch_transfer::err_stop_timeout &&
              stop_elapsed < 400ms &&
              raced_start == camera_stitch_transfer::err_already_running &&
              wait_until([&] { return !transfer.get_status().processing; }, 2000) &&
              access(path.c_str(), F_OK) == 0 &&
              transfer.get_status().backlog == 1;

    std::uint16_t recovery_port = 0;
    const int recovery_listener = create_listener(&recovery_port);
    if (recovery_listener < 0)
        return false;
    std::atomic<bool> server_ok{true};
    std::thread recovery_server([&] {
        server_ok = serve_acked_requests(recovery_listener, 1);
        close(recovery_listener);
    });
    success = success &&
              transfer.start("127.0.0.1", recovery_port) ==
                  camera_stitch_transfer::ok &&
              wait_until([&] { return transfer.get_status().delivered == 1; }) &&
              transfer.stop() == camera_stitch_transfer::ok;
    recovery_server.join();
    const camera_stitch_transfer::status status = transfer.get_status();
    const bool result = success && server_ok && status.backlog == 0 &&
                        status.recovered == 1 &&
                        access(path.c_str(), F_OK) != 0;
    unlink(path.c_str());
    rmdir(directory.c_str());
    return result;
}

bool test_conflict_drop_logged()
{
    const std::vector<std::uint8_t> payload = {0xff, 0xd8, 'x', 0xff, 0xd9};
    const std::string directory = create_directory();
    const std::string path = directory + "/conflict.jpg";
    std::uint16_t port = 0;
    const int listener = create_listener(&port);
    if (directory.empty() || !write_file(path, payload) || listener < 0)
        return false;

    std::atomic<int> requests{0};
    std::atomic<bool> server_ok{true};
    std::thread server([&] {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
        int connection = -1;
        while (std::chrono::steady_clock::now() < deadline) {
            if (connection < 0)
                connection = accept_with_timeout(listener, 600);
            if (connection < 0)
                continue;
            camera_transfer::photo_header header;
            std::vector<std::uint8_t> body;
            if (!receive_request(connection, &header, &body)) {
                close(connection);
                connection = -1;
                continue;
            }
            ++requests;
            camera_transfer::ack response;
            response.status = camera_transfer::ack_status::conflict;
            response.camera_id = header.camera_id;
            response.session_id = header.session_id;
            response.frame_id = header.frame_id;
            std::array<std::uint8_t, camera_transfer::kAckSize> wire{};
            if (camera_transfer::encode_ack(response, &wire) !=
                    camera_transfer::ok ||
                !send_exact(connection, wire.data(), wire.size())) {
                server_ok = false;
                close(connection);
                break;
            }
        }
        if (connection >= 0)
            close(connection);
        close(listener);
    });

    camera_stitch_transfer::worker transfer(4, 500, 3, 1500);
    const bool success =
        transfer.start("127.0.0.1", port) == camera_stitch_transfer::ok &&
        transfer.enqueue(make_item(path, 77)) == camera_stitch_transfer::ok &&
        wait_until([&] { return transfer.get_status().failed == 1; }) &&
        transfer.stop() == camera_stitch_transfer::ok;
    server.join();
    const camera_stitch_transfer::status status = transfer.get_status();

    const std::string log_path = directory + "/transfer_dropped.csv";
    const std::vector<std::uint8_t> log_bytes = read_file(log_path);
    const std::string log_text(log_bytes.begin(), log_bytes.end());
    const bool log_ok = log_text.find("drop_realtime_ns") == 0 &&
                        log_text.find(path) != std::string::npos &&
                        log_text.find("CONFLICT") != std::string::npos;
    const bool result =
        success && server_ok && requests == 1 && status.delivered == 0 &&
        status.failed == 1 && status.backlog == 0 && log_ok &&
        access(path.c_str(), F_OK) == 0;
    unlink(path.c_str());
    unlink(log_path.c_str());
    rmdir(directory.c_str());
    return result;
}

}  // namespace

int main()
{
    const struct {
        const char *name;
        bool (*test)();
    } tests[] = {
        {"retry_then_ack", test_retry_then_ack},
        {"outage_backlog_recovery", test_outage_backlog_recovery},
        {"spool_and_orphan_recovery", test_spool_and_orphan_recovery},
        {"ack_does_not_delete_replacement",
         test_ack_does_not_delete_replacement},
        {"bounded_stop_and_restart", test_bounded_stop_and_restart},
        {"conflict_drop_logged", test_conflict_drop_logged},
    };
    for (const auto &test : tests) {
        if (!test.test()) {
            std::fprintf(stderr, "STITCH_TRANSFER_WORKER_TEST_FAIL test=%s\n",
                         test.name);
            return 1;
        }
    }
    std::puts("STITCH_TRANSFER_WORKER_TEST_OK outage_recovery=1 "
              "spool_recovery=1 orphan_accounting=1 bounded_stop=1 "
              "inode_safe_delete=1");
    return 0;
}
