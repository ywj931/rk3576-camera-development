#include "xdas_camera_service.h"

#include <cctype>
#include <cstdio>
#include <limits>
#include <vector>

namespace xdas_camera_service {
namespace {

using xdas_camera_protocol::ERROR_BAD_COMMAND;
using xdas_camera_protocol::ERROR_NONE;
using xdas_camera_protocol::ERROR_UNSUPPORTED;

void append_u32_le(std::vector<std::uint8_t> *output, std::uint32_t value)
{
    output->push_back(static_cast<std::uint8_t>(value & 0xffU));
    output->push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    output->push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    output->push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

bool payload_is_empty(const xdas_camera_protocol::request &request,
                      xdas_camera_protocol::reply *reply)
{
    if (request.payload.empty())
        return true;
    reply->error = ERROR_BAD_COMMAND;
    return false;
}

bool select_cameras(const xdas_camera_protocol::request &request,
                    const config &service_config,
                    std::vector<std::uint8_t> *cameras,
                    xdas_camera_protocol::reply *reply)
{
    cameras->clear();
    if (service_config.camera_count == 0) {
        reply->error = ERROR_BAD_COMMAND;
        return false;
    }
    if (request.payload.empty()) {
        for (unsigned int id = 0; id < service_config.camera_count; ++id)
            cameras->push_back(static_cast<std::uint8_t>(id));
        return true;
    }
    if (request.payload.size() != 1U ||
        request.payload[0] >= service_config.camera_count) {
        reply->error = ERROR_BAD_COMMAND;
        return false;
    }
    cameras->push_back(request.payload[0]);
    return true;
}

std::string save_path(const config &service_config, std::uint8_t camera_id)
{
    std::string root = service_config.save_root;
    while (root.size() > 1U && root.back() == '/')
        root.pop_back();
    return root + "/cam" + std::to_string(camera_id);
}

void get_version(const xdas_camera_protocol::request &request,
                 xdas_camera_protocol::reply *reply,
                 const config &service_config)
{
    if (!payload_is_empty(request, reply))
        return;
    if (service_config.version.empty() || service_config.version.size() > 200U) {
        reply->error = ERROR_BAD_COMMAND;
        return;
    }
    reply->payload.push_back(0x02);
    reply->payload.insert(reply->payload.end(), service_config.version.begin(),
                          service_config.version.end());
}

void get_mode(const xdas_camera_protocol::request &request,
              xdas_camera_protocol::reply *reply,
              const config &service_config)
{
    if (!payload_is_empty(request, reply))
        return;
    if (service_config.work_mode != kUdiskMode &&
        service_config.work_mode != kUvcMode) {
        reply->error = ERROR_BAD_COMMAND;
        return;
    }
    // This five-byte payload matches the existing xdas camera response.
    reply->payload = {0x01, service_config.work_mode, 0x05, 0x00, 0x00};
}

void get_capacity(const xdas_camera_protocol::request &request,
                  xdas_camera_protocol::reply *reply,
                  const operations &service_operations)
{
    if (!payload_is_empty(request, reply))
        return;
    if (!service_operations.capacity_mb) {
        reply->error = ERROR_UNSUPPORTED;
        return;
    }
    std::uint32_t free_mb = 0;
    std::uint32_t total_mb = 0;
    if (!service_operations.capacity_mb(&free_mb, &total_mb) ||
        free_mb > total_mb) {
        reply->error = ERROR_BAD_COMMAND;
        return;
    }
    reply->payload.push_back(0x01);
    append_u32_le(&reply->payload, free_mb);
    append_u32_le(&reply->payload, total_mb);
}

void get_time(const xdas_camera_protocol::request &request,
              xdas_camera_protocol::reply *reply,
              const operations &service_operations)
{
    if (!payload_is_empty(request, reply))
        return;
    if (!service_operations.realtime_us) {
        reply->error = ERROR_UNSUPPORTED;
        return;
    }
    const std::uint64_t timestamp_us = service_operations.realtime_us();
    const std::uint64_t seconds = timestamp_us / 1000000ULL;
    const std::uint64_t microseconds = timestamp_us % 1000000ULL;
    if (seconds > 9999999999ULL) {
        reply->error = ERROR_BAD_COMMAND;
        return;
    }
    char timestamp[18] = {};
    const int size = std::snprintf(
        timestamp, sizeof(timestamp), "%010llu.%06llu",
        static_cast<unsigned long long>(seconds),
        static_cast<unsigned long long>(microseconds));
    if (size != 17) {
        reply->error = ERROR_BAD_COMMAND;
        return;
    }
    reply->payload.push_back(0x02);
    reply->payload.insert(reply->payload.end(), timestamp, timestamp + size);
}

bool parse_time_payload(const std::vector<std::uint8_t> &payload,
                        std::uint64_t *timestamp_us)
{
    if (!timestamp_us || payload.size() != 17U || payload[10] != '.')
        return false;
    std::uint64_t seconds = 0;
    std::uint64_t microseconds = 0;
    for (std::size_t index = 0; index < payload.size(); ++index) {
        if (index == 10U)
            continue;
        if (!std::isdigit(static_cast<unsigned char>(payload[index])))
            return false;
        const std::uint64_t digit = payload[index] - '0';
        if (index < 10U)
            seconds = seconds * 10U + digit;
        else
            microseconds = microseconds * 10U + digit;
    }
    if (seconds > (std::numeric_limits<std::uint64_t>::max() - microseconds) /
                      1000000ULL)
        return false;
    *timestamp_us = seconds * 1000000ULL + microseconds;
    return true;
}

void set_time(const xdas_camera_protocol::request &request,
              xdas_camera_protocol::reply *reply,
              const operations &service_operations)
{
    if (!service_operations.set_realtime_us) {
        reply->error = ERROR_UNSUPPORTED;
        return;
    }
    std::uint64_t timestamp_us = 0;
    if (!parse_time_payload(request.payload, &timestamp_us) ||
        service_operations.set_realtime_us(timestamp_us) != 0) {
        reply->error = ERROR_BAD_COMMAND;
        return;
    }
    // This is the response payload documented by xdas V2.1.
    reply->payload = {0x40, 0x00};
}

void start_save(const xdas_camera_protocol::request &request,
                xdas_camera_protocol::reply *reply,
                const config &service_config,
                const operations &service_operations)
{
    if (!service_operations.capture_running ||
        !service_operations.save_enabled || !service_operations.start_save ||
        !service_operations.stop_save || service_config.save_root.empty() ||
        service_config.save_root[0] != '/') {
        reply->error = ERROR_BAD_COMMAND;
        return;
    }
    std::vector<std::uint8_t> cameras;
    if (!select_cameras(request, service_config, &cameras, reply))
        return;
    for (const std::uint8_t camera_id : cameras) {
        if (!service_operations.capture_running(camera_id)) {
            reply->error = ERROR_BAD_COMMAND;
            return;
        }
    }

    std::vector<std::uint8_t> started;
    for (const std::uint8_t camera_id : cameras) {
        if (service_operations.save_enabled(camera_id))
            continue;
        if (service_operations.start_save(camera_id,
                                          save_path(service_config, camera_id)) !=
            0) {
            for (auto iterator = started.rbegin(); iterator != started.rend();
                 ++iterator)
                service_operations.stop_save(*iterator);
            reply->error = ERROR_BAD_COMMAND;
            return;
        }
        started.push_back(camera_id);
    }
}

void stop_save(const xdas_camera_protocol::request &request,
               xdas_camera_protocol::reply *reply,
               const config &service_config,
               const operations &service_operations)
{
    if (!service_operations.save_enabled || !service_operations.stop_save) {
        reply->error = ERROR_BAD_COMMAND;
        return;
    }
    std::vector<std::uint8_t> cameras;
    if (!select_cameras(request, service_config, &cameras, reply))
        return;
    bool failed = false;
    for (const std::uint8_t camera_id : cameras) {
        if (service_operations.save_enabled(camera_id)) {
            if (service_operations.stop_save(camera_id) != 0)
                failed = true;
        } else if (service_operations.save_session_error &&
                   service_operations.save_session_error(camera_id) != 0) {
            failed = true;
        }
    }
    if (failed)
        reply->error = ERROR_BAD_COMMAND;
}

void set_uvc(const xdas_camera_protocol::request &request,
             xdas_camera_protocol::reply *reply,
             const config &service_config,
             const operations &service_operations)
{
    if (!service_operations.start_uvc) {
        reply->error = ERROR_UNSUPPORTED;
        return;
    }
    std::vector<std::uint8_t> cameras;
    if (!select_cameras(request, service_config, &cameras, reply))
        return;
    const int target = request.payload.empty() ? kAllCameras : cameras.front();
    if (service_operations.start_uvc(target) != 0) {
        reply->error = ERROR_BAD_COMMAND;
        return;
    }
    // Preserve the two response bytes used by the reference camera firmware.
    reply->payload = {0x20, 0x00};
}

}  // namespace

void handle_command(const xdas_camera_protocol::request &request,
                    xdas_camera_protocol::reply *reply,
                    const config &service_config,
                    const operations &service_operations)
{
    if (!reply)
        return;
    *reply = {};
    if (request.command0 == 0x00 && request.command1 == 0x00) {
        get_version(request, reply, service_config);
    } else if (request.command0 == 0x00 && request.command1 == 0x02) {
        get_mode(request, reply, service_config);
    } else if (request.command0 == 0x00 && request.command1 == 0x03) {
        get_capacity(request, reply, service_operations);
    } else if (request.command0 == 0x00 && request.command1 == 0x04) {
        get_time(request, reply, service_operations);
    } else if (request.command0 == 0x01 && request.command1 == 0x11) {
        start_save(request, reply, service_config, service_operations);
    } else if (request.command0 == 0x01 && request.command1 == 0x12) {
        stop_save(request, reply, service_config, service_operations);
    } else if (request.command0 == 0x01 && request.command1 == 0x15) {
        set_uvc(request, reply, service_config, service_operations);
    } else if (request.command0 == 0x01 && request.command1 == 0x16) {
        set_time(request, reply, service_operations);
    } else {
        reply->error = ERROR_UNSUPPORTED;
    }
}

int self_test(std::string *report)
{
    config service_config;
    service_config.version = "DASCAM-TEST";
    service_config.save_root = "/data/test";
    bool capture_running[2] = {true, true};
    bool saving[2] = {false, false};
    int starts[2] = {0, 0};
    int stops[2] = {0, 0};
    bool fail_camera1 = false;
    std::string paths[2];

    operations service_operations;
    std::uint64_t set_timestamp_us = 0;
    int session_error[2] = {0, 0};
    service_operations.capture_running = [&](std::uint8_t id) {
        return capture_running[id];
    };
    service_operations.save_enabled =
        [&](std::uint8_t id) { return saving[id]; };
    service_operations.save_session_error =
        [&](std::uint8_t id) { return session_error[id]; };
    service_operations.start_save =
        [&](std::uint8_t id, const std::string &path) {
            starts[id]++;
            paths[id] = path;
            if (id == 1 && fail_camera1)
                return -1;
            saving[id] = true;
            session_error[id] = 0;
            return 0;
        };
    service_operations.stop_save = [&](std::uint8_t id) {
        stops[id]++;
        saving[id] = false;
        return 0;
    };
    service_operations.start_uvc = [](int) { return 0; };
    service_operations.capacity_mb = [](std::uint32_t *free_mb,
                                        std::uint32_t *total_mb) {
        *free_mb = 14533;
        *total_mb = 14736;
        return true;
    };
    service_operations.realtime_us = [] { return 1559319666101665ULL; };
    service_operations.set_realtime_us = [&](std::uint64_t timestamp_us) {
        set_timestamp_us = timestamp_us;
        return 0;
    };

    xdas_camera_protocol::request request;
    request.command0 = 0x01;
    request.command1 = 0x11;
    xdas_camera_protocol::reply reply;
    handle_command(request, &reply, service_config, service_operations);
    if (reply.error != ERROR_NONE || !saving[0] || !saving[1] ||
        starts[0] != 1 || starts[1] != 1 || paths[0] != "/data/test/cam0" ||
        paths[1] != "/data/test/cam1") {
        if (report)
            *report = "global save-start mapping failed";
        return xdas_camera_protocol::ERR_PROTOCOL;
    }

    handle_command(request, &reply, service_config, service_operations);
    if (reply.error != ERROR_NONE || starts[0] != 1 || starts[1] != 1) {
        if (report)
            *report = "save-start is not idempotent";
        return xdas_camera_protocol::ERR_PROTOCOL;
    }

    request.command1 = 0x12;
    request.payload = {0x00};
    handle_command(request, &reply, service_config, service_operations);
    if (reply.error != ERROR_NONE || saving[0] || !saving[1]) {
        if (report)
            *report = "camera-targeted save-stop mapping failed";
        return xdas_camera_protocol::ERR_PROTOCOL;
    }
    request.payload.clear();
    handle_command(request, &reply, service_config, service_operations);
    if (reply.error != ERROR_NONE || saving[0] || saving[1]) {
        if (report)
            *report = "global save-stop mapping failed";
        return xdas_camera_protocol::ERR_PROTOCOL;
    }

    session_error[1] = -1;
    handle_command(request, &reply, service_config, service_operations);
    if (reply.error != ERROR_BAD_COMMAND) {
        if (report)
            *report = "failed save session was not sticky across stop retry";
        return xdas_camera_protocol::ERR_PROTOCOL;
    }
    session_error[1] = 0;

    starts[0] = starts[1] = stops[0] = stops[1] = 0;
    fail_camera1 = true;
    request.command1 = 0x11;
    handle_command(request, &reply, service_config, service_operations);
    if (reply.error != ERROR_BAD_COMMAND || saving[0] || saving[1] ||
        starts[0] != 1 || starts[1] != 1 || stops[0] != 1) {
        if (report)
            *report = "dual-camera save rollback failed";
        return xdas_camera_protocol::ERR_PROTOCOL;
    }

    fail_camera1 = false;
    request.command0 = 0x00;
    request.command1 = 0x03;
    handle_command(request, &reply, service_config, service_operations);
    const std::vector<std::uint8_t> expected_capacity =
        {0x01, 0xc5, 0x38, 0x00, 0x00, 0x90, 0x39, 0x00, 0x00};
    if (reply.error != ERROR_NONE || reply.payload != expected_capacity) {
        if (report)
            *report = "capacity response mapping failed";
        return xdas_camera_protocol::ERR_PROTOCOL;
    }

    request.command1 = 0x04;
    handle_command(request, &reply, service_config, service_operations);
    const std::string expected_time = "1559319666.101665";
    if (reply.error != ERROR_NONE || reply.payload.size() != 18U ||
        reply.payload[0] != 0x02 ||
        std::string(reply.payload.begin() + 1, reply.payload.end()) !=
            expected_time) {
        if (report)
            *report = "time response mapping failed";
        return xdas_camera_protocol::ERR_PROTOCOL;
    }

    request.command0 = 0x01;
    request.command1 = 0x16;
    const std::string set_time_text = "1763543161.819720";
    request.payload.assign(set_time_text.begin(), set_time_text.end());
    handle_command(request, &reply, service_config, service_operations);
    if (reply.error != ERROR_NONE ||
        reply.payload != std::vector<std::uint8_t>({0x40, 0x00}) ||
        set_timestamp_us != 1763543161819720ULL) {
        if (report)
            *report = "set-time response mapping failed";
        return xdas_camera_protocol::ERR_PROTOCOL;
    }

    request.payload.back() = 'x';
    handle_command(request, &reply, service_config, service_operations);
    if (reply.error != ERROR_BAD_COMMAND ||
        set_timestamp_us != 1763543161819720ULL) {
        if (report)
            *report = "invalid set-time payload was accepted";
        return xdas_camera_protocol::ERR_PROTOCOL;
    }

    if (report)
        *report =
            "save_global=1 save_target=1 idempotent=1 sticky_error=1 rollback=1 queries=2 set_time=1";
    return xdas_camera_protocol::OK;
}

}  // namespace xdas_camera_service
