#include "xdas_camera_protocol.h"
#include "xdas_camera_service.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <pty.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

bool write_full(int fd, const std::uint8_t *data, std::size_t size)
{
    while (size) {
        const ssize_t written = write(fd, data, size);
        if (written > 0) {
            data += static_cast<std::size_t>(written);
            size -= static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

bool read_frame(int fd, std::vector<std::uint8_t> *frame)
{
    std::vector<std::uint8_t> input;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        struct pollfd descriptor = {fd, POLLIN, 0};
        const int ready = poll(&descriptor, 1, 50);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
            return false;
        if (ready == 0)
            continue;
        std::uint8_t bytes[64];
        const ssize_t count = read(fd, bytes, sizeof(bytes));
        if (count > 0)
            input.insert(input.end(), bytes, bytes + count);
        if (input.size() < 2U)
            continue;
        if (input[0] != 0x55U)
            return false;
        const std::size_t size = input[1];
        if (size >= 6U && input.size() >= size) {
            frame->assign(input.begin(), input.begin() + size);
            return true;
        }
    }
    return false;
}

bool pty_test(std::string *error)
{
    int master_fd = -1;
    int slave_fd = -1;
    char slave_name[128] = {};
    if (openpty(&master_fd, &slave_fd, slave_name, nullptr, nullptr) < 0) {
        *error = std::string("openpty failed: ") + std::strerror(errno);
        return false;
    }

    std::atomic<bool> stop{false};
    std::atomic<int> command_count{0};
    int run_result = xdas_camera_protocol::ERR_IO;
    const auto handler = [&](const xdas_camera_protocol::request &request,
                             xdas_camera_protocol::reply *reply) {
        if (request.command0 != 0x01 || request.command1 != 0x11 ||
            !request.payload.empty()) {
            reply->error = xdas_camera_protocol::ERROR_BAD_COMMAND;
            return;
        }
        command_count.fetch_add(1);
    };
    std::thread service([&] {
        run_result = xdas_camera_protocol::run(
            slave_name, handler, [&] { return stop.load(); });
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const std::uint8_t noise[] = {0x7e, 0x00};
    const std::uint8_t request[] = {0xaa, 0x05, 0x01, 0x11, 0x73};
    bool passed = write_full(master_fd, noise, sizeof(noise)) &&
                  write_full(master_fd, request, 2U);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    passed = passed && write_full(master_fd, request + 2U, sizeof(request) - 2U);

    std::vector<std::uint8_t> response;
    const std::vector<std::uint8_t> success =
        {0x55, 0x06, 0x01, 0x11, 0x00, 0xa7};
    passed = passed && read_frame(master_fd, &response) && response == success &&
             command_count.load() == 1;

    for (int iteration = 1; passed && iteration < 10; ++iteration) {
        passed = write_full(master_fd, request, sizeof(request)) &&
                 read_frame(master_fd, &response) && response == success;
    }

    std::uint8_t bad_crc[sizeof(request)] = {};
    std::memcpy(bad_crc, request, sizeof(request));
    bad_crc[sizeof(bad_crc) - 1U] ^= 0x01U;
    passed = passed && write_full(master_fd, bad_crc, sizeof(bad_crc)) &&
             read_frame(master_fd, &response) && response.size() == 6U &&
             response[2] == 0xff && response[3] == 0xff &&
             response[4] == xdas_camera_protocol::ERROR_BAD_CRC &&
             xdas_camera_protocol::crc8(response.data(), response.size() - 1U) ==
                 response.back();
    passed = passed && write_full(master_fd, request, sizeof(request)) &&
             read_frame(master_fd, &response) && response == success &&
             command_count.load() == 11;

    stop.store(true);
    service.join();
    close(slave_fd);
    close(master_fd);
    if (run_result != xdas_camera_protocol::OK)
        passed = false;
    if (!passed)
        *error = "fragmented/repeated/CRC-recovery PTY test failed";
    return passed;
}

bool reference_frame_test(std::string *error)
{
    xdas_camera_service::config config;
    config.version = "USBCAM-00.00.04";
    xdas_camera_service::operations operations;
    std::uint64_t set_timestamp_us = 0;
    operations.capture_running = [](std::uint8_t) { return true; };
    operations.save_enabled = [](std::uint8_t) { return false; };
    operations.start_save = [](std::uint8_t, const std::string &) { return 0; };
    operations.stop_save = [](std::uint8_t) { return 0; };
    operations.start_uvc = [](int) { return 0; };
    operations.schedule_reboot = [] { return 0; };
    operations.set_auto = [](std::uint8_t) { return 0; };
    operations.set_exposure_us = [](std::uint8_t, std::uint32_t) { return 0; };
    operations.set_iso = [](std::uint8_t, std::uint32_t) { return 0; };
    operations.get_max_exposure_us =
        [](std::uint8_t, std::uint32_t *value) {
            *value = 100000U;
            return 0;
        };
    operations.set_max_exposure_us =
        [](std::uint8_t, std::uint32_t) { return 0; };
    operations.capacity_mb = [](std::uint32_t *free_mb,
                                std::uint32_t *total_mb) {
        *free_mb = 14533;
        *total_mb = 14736;
        return true;
    };
    operations.realtime_us = [] { return 1559319666101665ULL; };
    operations.set_realtime_us = [&](std::uint64_t timestamp_us) {
        set_timestamp_us = timestamp_us;
        return 0;
    };
    const auto handler = [&](const xdas_camera_protocol::request &request,
                             xdas_camera_protocol::reply *reply) {
        xdas_camera_service::handle_command(request, reply, config, operations);
    };
    const auto expect = [&](const std::vector<std::uint8_t> &request,
                            const std::vector<std::uint8_t> &expected) {
        std::vector<std::uint8_t> response;
        return xdas_camera_protocol::process_frame(
                   request.data(), request.size(), handler, &response) ==
                   xdas_camera_protocol::OK &&
               response == expected;
    };

    if (!expect({0xaa, 0x05, 0x00, 0x00, 0xff},
                {0x55, 0x16, 0x00, 0x00, 0x00, 0x02, 0x55, 0x53,
                 0x42, 0x43, 0x41, 0x4d, 0x2d, 0x30, 0x30, 0x2e,
                 0x30, 0x30, 0x2e, 0x30, 0x34, 0x87}) ||
        !expect({0xaa, 0x05, 0x00, 0x02, 0x80},
                {0x55, 0x0b, 0x00, 0x02, 0x00, 0x01, 0x02, 0x05,
                 0x00, 0x00, 0x2b}) ||
        !expect({0xaa, 0x05, 0x00, 0x03, 0x55},
                {0x55, 0x0f, 0x00, 0x03, 0x00, 0x01, 0xc5, 0x38,
                 0x00, 0x00, 0x90, 0x39, 0x00, 0x00, 0xb1}) ||
        !expect({0xaa, 0x05, 0x00, 0x04, 0x01},
                {0x55, 0x18, 0x00, 0x04, 0x00, 0x02, 0x31, 0x35,
                 0x35, 0x39, 0x33, 0x31, 0x39, 0x36, 0x36, 0x36,
                 0x2e, 0x31, 0x30, 0x31, 0x36, 0x36, 0x35, 0x2b}) ||
        !expect({0xaa, 0x05, 0x01, 0x15, 0x8d},
                {0x55, 0x08, 0x01, 0x15, 0x00, 0x20, 0x00, 0x9f}) ||
        !expect({0xaa, 0x06, 0x01, 0x17, 0x08, 0x1d},
                {0x55, 0x08, 0x01, 0x17, 0x00, 0x20, 0x00, 0x15}) ||
        !expect({0xaa, 0x06, 0x01, 0x19, 0x0f, 0x2b},
                {0x55, 0x06, 0x01, 0x19, 0x00, 0xff}) ||
        !expect({0xaa, 0x06, 0x01, 0x1a, 0x06, 0xca},
                {0x55, 0x06, 0x01, 0x1a, 0x00, 0xe2}) ||
        !expect({0xaa, 0x16, 0x01, 0x16, 0x31, 0x37, 0x36, 0x33,
                 0x35, 0x34, 0x33, 0x31, 0x36, 0x31, 0x2e, 0x38,
                 0x31, 0x39, 0x37, 0x32, 0x30, 0x44},
                {0x55, 0x08, 0x01, 0x16, 0x00, 0x40, 0x00, 0x5a}) ||
        set_timestamp_us != 1763543161819720ULL) {
        *error = "xdas reference request/response vector mismatch";
        return false;
    }
    return true;
}

}  // namespace

int main()
{
    std::string report;
    if (xdas_camera_protocol::protocol_self_test(&report) !=
        xdas_camera_protocol::OK) {
        std::fprintf(stderr, "XDAS_PROTOCOL_TEST_FAILED stage=framing detail=\"%s\"\n",
                     report.c_str());
        return 1;
    }
    if (xdas_camera_service::self_test(&report) != xdas_camera_protocol::OK) {
        std::fprintf(stderr, "XDAS_PROTOCOL_TEST_FAILED stage=service detail=\"%s\"\n",
                     report.c_str());
        return 1;
    }
    if (!reference_frame_test(&report)) {
        std::fprintf(stderr, "XDAS_PROTOCOL_TEST_FAILED stage=reference detail=\"%s\"\n",
                     report.c_str());
        return 1;
    }
    if (!pty_test(&report)) {
        std::fprintf(stderr, "XDAS_PROTOCOL_TEST_FAILED stage=pty detail=\"%s\"\n",
                     report.c_str());
        return 1;
    }
    std::printf("XDAS_PROTOCOL_TEST_OK crc=CRC8_D5 reference=version,mode,capacity,get_time,set_time,uvc,max_exposure,shutter,iso save=global,targeted,idempotent,sticky_error,rollback pty=fragmented,repeated,crc_recovery,resync\n");
    return 0;
}
