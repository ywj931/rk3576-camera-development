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

    const bool has_global_save =
        static_cast<bool>(service_operations.global_save_enabled) ||
        static_cast<bool>(service_operations.global_save_session_error) ||
        static_cast<bool>(service_operations.start_global_save) ||
        static_cast<bool>(service_operations.stop_global_save);
    if (has_global_save &&
        (!service_operations.global_save_enabled ||
         !service_operations.global_save_session_error ||
         !service_operations.start_global_save ||
         !service_operations.stop_global_save)) {
        reply->error = ERROR_BAD_COMMAND;
        return;
    }
    if (request.payload.empty() && has_global_save) {
        for (const std::uint8_t camera_id : cameras) {
            if (service_operations.save_enabled(camera_id)) {
                reply->error = ERROR_BAD_COMMAND;
                return;
            }
        }
        if (!service_operations.global_save_enabled() &&
            service_operations.start_global_save() != 0) {
            reply->error = ERROR_BAD_COMMAND;
        }
        return;
    }
    if (has_global_save && service_operations.global_save_enabled()) {
        reply->error = ERROR_BAD_COMMAND;
        return;
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

    const bool has_global_save =
        static_cast<bool>(service_operations.global_save_enabled) ||
        static_cast<bool>(service_operations.global_save_session_error) ||
        static_cast<bool>(service_operations.start_global_save) ||
        static_cast<bool>(service_operations.stop_global_save);
    if (has_global_save &&
        (!service_operations.global_save_enabled ||
         !service_operations.global_save_session_error ||
         !service_operations.start_global_save ||
         !service_operations.stop_global_save)) {
        reply->error = ERROR_BAD_COMMAND;
        return;
    }
    if (request.payload.empty() && has_global_save) {
        bool failed = false;
        if (service_operations.global_save_enabled()) {
            failed = service_operations.stop_global_save() != 0;
        }
        if (service_operations.global_save_session_error() != 0)
            failed = true;
        if (failed)
            reply->error = ERROR_BAD_COMMAND;
        return;
    }
    if (has_global_save && service_operations.global_save_enabled()) {
        reply->error = ERROR_BAD_COMMAND;
        return;
    }
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

bool get_single_value(const xdas_camera_protocol::request &request,
                      std::uint8_t *value,
                      xdas_camera_protocol::reply *reply)
{
    if (!value || request.payload.size() != 1U) {
        reply->error = ERROR_BAD_COMMAND;
        return false;
    }
    *value = request.payload[0];
    return true;
}

bool apply_to_all_cameras(const config &service_config,
                          const std::function<int(std::uint8_t)> &operation)
{
    if (service_config.camera_count == 0 || !operation)
        return false;
    bool succeeded = true;
    for (unsigned int id = 0; id < service_config.camera_count; ++id) {
        if (operation(static_cast<std::uint8_t>(id)) != 0)
            succeeded = false;
    }
    return succeeded;
}

void reboot_device(const xdas_camera_protocol::request &request,
                   xdas_camera_protocol::reply *reply,
                   const operations &service_operations)
{
    if (!payload_is_empty(request, reply))
        return;
    if (!service_operations.schedule_reboot) {
        reply->error = ERROR_UNSUPPORTED;
        return;
    }
    if (service_operations.schedule_reboot() != 0)
        reply->error = ERROR_BAD_COMMAND;
}

void set_max_exposure(const xdas_camera_protocol::request &request,
                      xdas_camera_protocol::reply *reply,
                      const config &service_config,
                      const operations &service_operations)
{
    static constexpr std::uint32_t kMaxExposureUs[] = {
        50000U, 20000U, 10000U, 5000U, 3333U,
        2500U,  2000U,  1667U,  1250U,
    };
    std::uint8_t slot = 0;
    if (!get_single_value(request, &slot, reply))
        return;
    if (slot >= sizeof(kMaxExposureUs) / sizeof(kMaxExposureUs[0])) {
        reply->error = ERROR_BAD_COMMAND;
        return;
    }
    if (!service_operations.get_max_exposure_us ||
        !service_operations.set_max_exposure_us ||
        service_config.camera_count == 0) {
        reply->error = ERROR_UNSUPPORTED;
        return;
    }

    std::vector<std::uint32_t> previous(service_config.camera_count, 0U);
    for (unsigned int id = 0; id < service_config.camera_count; ++id) {
        if (service_operations.get_max_exposure_us(
                static_cast<std::uint8_t>(id), &previous[id]) != 0) {
            reply->error = ERROR_BAD_COMMAND;
            return;
        }
    }

    unsigned int applied = 0;
    for (; applied < service_config.camera_count; ++applied) {
        if (service_operations.set_max_exposure_us(
                static_cast<std::uint8_t>(applied),
                kMaxExposureUs[slot]) == 0) {
            continue;
        }
        for (unsigned int rollback = 0; rollback < applied; ++rollback) {
            const int rollback_result = service_operations.set_max_exposure_us(
                static_cast<std::uint8_t>(rollback), previous[rollback]);
            if (rollback_result != 0) {
                std::fprintf(stderr,
                             "XDAS_MAX_EXPOSURE_ROLLBACK_ERROR camera_id=%u "
                             "requested_us=%u result=%d\n",
                             rollback, previous[rollback], rollback_result);
            }
        }
        reply->error = ERROR_BAD_COMMAND;
        return;
    }
    reply->payload = {0x20, 0x00};
}

void set_shutter(const xdas_camera_protocol::request &request,
                 xdas_camera_protocol::reply *reply,
                 const config &service_config,
                 const operations &service_operations)
{
    static constexpr std::uint32_t kShutterUs[] = {
        0U,       16000000U, 8000000U, 4000000U, 2000000U, 1000000U,
        500000U,  250000U,   125000U,  66667U,   50000U,   41667U,
        33333U,   16667U,    8000U,    4000U,    2000U,    1000U,
        500U,     250U,      125U,     63U,
    };
    std::uint8_t slot = 0;
    if (!get_single_value(request, &slot, reply))
        return;
    if (slot >= sizeof(kShutterUs) / sizeof(kShutterUs[0])) {
        reply->error = ERROR_BAD_COMMAND;
        return;
    }
    if (slot == 0U) {
        if (!service_operations.set_auto) {
            reply->error = ERROR_UNSUPPORTED;
            return;
        }
        if (!apply_to_all_cameras(service_config, service_operations.set_auto))
            reply->error = ERROR_BAD_COMMAND;
        return;
    }
    if (!service_operations.set_exposure_us) {
        reply->error = ERROR_UNSUPPORTED;
        return;
    }
    if (!apply_to_all_cameras(service_config, [&](std::uint8_t id) {
            return service_operations.set_exposure_us(id, kShutterUs[slot]);
        })) {
        reply->error = ERROR_BAD_COMMAND;
    }
}

void set_iso(const xdas_camera_protocol::request &request,
             xdas_camera_protocol::reply *reply,
             const config &service_config,
             const operations &service_operations)
{
    static constexpr std::uint32_t kIso[] = {
        0U, 100U, 200U, 400U, 800U, 1600U, 3200U,
    };
    std::uint8_t slot = 0;
    if (!get_single_value(request, &slot, reply))
        return;
    if (slot >= sizeof(kIso) / sizeof(kIso[0])) {
        reply->error = ERROR_BAD_COMMAND;
        return;
    }
    if (slot == 0U) {
        if (!service_operations.set_auto) {
            reply->error = ERROR_UNSUPPORTED;
            return;
        }
        if (!apply_to_all_cameras(service_config, service_operations.set_auto))
            reply->error = ERROR_BAD_COMMAND;
        return;
    }
    if (!service_operations.set_iso) {
        reply->error = ERROR_UNSUPPORTED;
        return;
    }
    if (!apply_to_all_cameras(service_config, [&](std::uint8_t id) {
            return service_operations.set_iso(id, kIso[slot]);
        })) {
        reply->error = ERROR_BAD_COMMAND;
    }
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
    } else if (request.command0 == 0x01 && request.command1 == 0x04) {
        reboot_device(request, reply, service_operations);
    } else if (request.command0 == 0x01 && request.command1 == 0x17) {
        set_max_exposure(request, reply, service_config, service_operations);
    } else if (request.command0 == 0x01 && request.command1 == 0x19) {
        set_shutter(request, reply, service_config, service_operations);
    } else if (request.command0 == 0x01 && request.command1 == 0x1a) {
        set_iso(request, reply, service_config, service_operations);
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
    int auto_calls[2] = {0, 0};
    std::uint32_t exposure_us[2] = {0, 0};
    std::uint32_t iso[2] = {0, 0};
    std::uint32_t max_exposure_us[2] = {100000U, 100000U};
    int max_exposure_sets[2] = {0, 0};
    int reboot_requests = 0;
    bool fail_camera1 = false;
    bool fail_max_exposure_camera1 = false;
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
    service_operations.schedule_reboot = [&] {
        ++reboot_requests;
        return 0;
    };
    service_operations.set_auto = [&](std::uint8_t id) {
        ++auto_calls[id];
        return 0;
    };
    service_operations.set_exposure_us = [&](std::uint8_t id,
                                              std::uint32_t value) {
        exposure_us[id] = value;
        return 0;
    };
    service_operations.set_iso = [&](std::uint8_t id, std::uint32_t value) {
        iso[id] = value;
        return 0;
    };
    service_operations.get_max_exposure_us =
        [&](std::uint8_t id, std::uint32_t *value) {
            *value = max_exposure_us[id];
            return 0;
        };
    service_operations.set_max_exposure_us =
        [&](std::uint8_t id, std::uint32_t value) {
            ++max_exposure_sets[id];
            if (id == 1 && fail_max_exposure_camera1)
                return -1;
            max_exposure_us[id] = value;
            return 0;
        };
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

    bool global_saving = false;
    int global_starts = 0;
    int global_stops = 0;
    int global_session_error = 0;
    service_operations.global_save_enabled = [&] { return global_saving; };
    service_operations.global_save_session_error = [&] {
        return global_session_error;
    };
    service_operations.start_global_save = [&] {
        global_starts++;
        global_saving = true;
        global_session_error = 0;
        return 0;
    };
    service_operations.stop_global_save = [&] {
        global_stops++;
        global_saving = false;
        return 0;
    };
    fail_camera1 = false;
    request.payload.clear();
    handle_command(request, &reply, service_config, service_operations);
    handle_command(request, &reply, service_config, service_operations);
    if (reply.error != ERROR_NONE || !global_saving || global_starts != 1 ||
        saving[0] || saving[1]) {
        if (report)
            *report = "composite global save-start mapping failed";
        return xdas_camera_protocol::ERR_PROTOCOL;
    }
    request.payload = {0x00};
    handle_command(request, &reply, service_config, service_operations);
    if (reply.error != ERROR_BAD_COMMAND || !global_saving) {
        if (report)
            *report = "targeted save was accepted during composite save";
        return xdas_camera_protocol::ERR_PROTOCOL;
    }
    request.command1 = 0x12;
    request.payload.clear();
    handle_command(request, &reply, service_config, service_operations);
    if (reply.error != ERROR_NONE || global_saving || global_stops != 1) {
        if (report)
            *report = "composite global save-stop mapping failed";
        return xdas_camera_protocol::ERR_PROTOCOL;
    }
    global_session_error = -1;
    handle_command(request, &reply, service_config, service_operations);
    if (reply.error != ERROR_BAD_COMMAND || global_stops != 1) {
        if (report)
            *report = "composite transfer failure was not sticky";
        return xdas_camera_protocol::ERR_PROTOCOL;
    }
    global_session_error = 0;

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

    request.command1 = 0x04;
    request.payload.clear();
    handle_command(request, &reply, service_config, service_operations);
    if (reply.error != ERROR_NONE || reboot_requests != 1) {
        if (report)
            *report = "reboot scheduling mapping failed";
        return xdas_camera_protocol::ERR_PROTOCOL;
    }

    request.command1 = 0x17;
    request.payload = {0x08};
    handle_command(request, &reply, service_config, service_operations);
    if (reply.error != ERROR_NONE ||
        reply.payload != std::vector<std::uint8_t>({0x20, 0x00}) ||
        max_exposure_us[0] != 1250U || max_exposure_us[1] != 1250U) {
        if (report)
            *report = "maximum exposure slot mapping failed";
        return xdas_camera_protocol::ERR_PROTOCOL;
    }
    fail_max_exposure_camera1 = true;
    request.payload = {0x00};
    handle_command(request, &reply, service_config, service_operations);
    fail_max_exposure_camera1 = false;
    if (reply.error != ERROR_BAD_COMMAND || max_exposure_us[0] != 1250U ||
        max_exposure_us[1] != 1250U || max_exposure_sets[0] != 3 ||
        max_exposure_sets[1] != 2) {
        if (report)
            *report = "maximum exposure failure rollback failed";
        return xdas_camera_protocol::ERR_PROTOCOL;
    }
    request.payload.clear();
    handle_command(request, &reply, service_config, service_operations);
    if (reply.error != ERROR_BAD_COMMAND) {
        if (report)
            *report = "empty maximum exposure payload was accepted";
        return xdas_camera_protocol::ERR_PROTOCOL;
    }
    request.payload = {0x09};
    handle_command(request, &reply, service_config, service_operations);
    if (reply.error != ERROR_BAD_COMMAND) {
        if (report)
            *report = "invalid maximum exposure slot was accepted";
        return xdas_camera_protocol::ERR_PROTOCOL;
    }

    request.command1 = 0x19;
    request.payload = {0x01};
    handle_command(request, &reply, service_config, service_operations);
    if (reply.error != ERROR_NONE || exposure_us[0] != 16000000U ||
        exposure_us[1] != 16000000U) {
        if (report)
            *report = "shutter slot mapping failed";
        return xdas_camera_protocol::ERR_PROTOCOL;
    }
    request.payload = {0x00};
    handle_command(request, &reply, service_config, service_operations);
    if (reply.error != ERROR_NONE || auto_calls[0] != 1 ||
        auto_calls[1] != 1) {
        if (report)
            *report = "shutter auto mapping failed";
        return xdas_camera_protocol::ERR_PROTOCOL;
    }

    request.command1 = 0x1a;
    request.payload = {0x06};
    handle_command(request, &reply, service_config, service_operations);
    if (reply.error != ERROR_NONE || iso[0] != 3200U || iso[1] != 3200U) {
        if (report)
            *report = "ISO slot mapping failed";
        return xdas_camera_protocol::ERR_PROTOCOL;
    }

    if (report)
        *report =
            "save_global=1 save_target=1 composite_global=1 idempotent=1 "
            "sticky_error=1 rollback=1 queries=2 set_time=1 reboot=1 "
            "max_exposure=transactional shutter=1 ISO=1";
    return xdas_camera_protocol::OK;
}

}  // namespace xdas_camera_service
