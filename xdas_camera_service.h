#ifndef XDAS_CAMERA_SERVICE_H
#define XDAS_CAMERA_SERVICE_H

#include "xdas_camera_protocol.h"

#include <cstdint>
#include <functional>
#include <string>

namespace xdas_camera_service {

constexpr std::uint8_t kUdiskMode = 0x01;
constexpr std::uint8_t kUvcMode = 0x02;
constexpr int kAllCameras = -1;

struct config {
    std::uint8_t camera_count = 2;
    std::uint8_t work_mode = kUvcMode;
    std::string version = "DASCAM-00.01";
    std::string save_root = "/data/camera";
};

struct operations {
    std::function<bool(std::uint8_t)> capture_running;
    std::function<bool()> global_save_enabled;
    std::function<int()> global_save_session_error;
    std::function<int()> start_global_save;
    std::function<int()> stop_global_save;
    std::function<bool(std::uint8_t)> save_enabled;
    std::function<int(std::uint8_t)> save_session_error;
    std::function<int(std::uint8_t, const std::string &)> start_save;
    std::function<int(std::uint8_t)> stop_save;
    std::function<int(int)> start_uvc;
    std::function<int()> schedule_reboot;
    std::function<int(std::uint8_t)> set_auto;
    std::function<int(std::uint8_t, std::uint32_t)> set_exposure_us;
    std::function<int(std::uint8_t, std::uint32_t)> set_iso;
    std::function<int(std::uint8_t, std::uint32_t *)> get_max_exposure_us;
    std::function<int(std::uint8_t, std::uint32_t)> set_max_exposure_us;
    std::function<bool(std::uint32_t *, std::uint32_t *)> capacity_mb;
    std::function<std::uint64_t()> realtime_us;
    std::function<int(std::uint64_t)> set_realtime_us;
};

void handle_command(const xdas_camera_protocol::request &request,
                    xdas_camera_protocol::reply *reply,
                    const config &service_config,
                    const operations &service_operations);
int self_test(std::string *report);

}  // namespace xdas_camera_service

#endif
