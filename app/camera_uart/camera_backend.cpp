#include "camera_backend.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <mutex>
#include <new>
#include <string>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

#include "uAPI2/rk_aiq_user_api2_ae.h"
#include "uAPI2/rk_aiq_user_api2_imgproc.h"
#include "uAPI2/rk_aiq_user_api2_sysctl.h"

namespace {

constexpr uint32_t kMinExposureUs = 1;
constexpr uint32_t kMaxExposureUs = 1000000;
constexpr uint32_t kMinGainX1000 = 1000;
constexpr uint32_t kMaxGainX1000 = 64000;
constexpr uint32_t kMinFps = 1;
constexpr uint32_t kMaxFps = 120;
constexpr uint32_t kStreamStartEvent = V4L2_EVENT_PRIVATE_START + 1;
constexpr uint32_t kStreamStopEvent = V4L2_EVENT_PRIVATE_START + 2;

struct CameraSlot {
    std::mutex mutex;
    rk_aiq_sys_ctx_t *ctx = nullptr;
    bool started = false;
    bool manual_mode = false;
    uint32_t requested_fps = 0;
    int last_aiq_error = 0;
    std::string sensor_name;
    std::string iq_dir;
    std::string params_device;
    int event_fd = -1;
    std::atomic<bool> stop_event_thread{false};
    std::thread event_thread;
};

bool directory_exists(const char *path)
{
    struct stat st = {};
    return path != nullptr && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int validate_camera_id(const camera_backend_t *backend, int camera_id)
{
    if (backend == nullptr)
        return CAMERA_BACKEND_ERR_ARGUMENT;
    if (camera_id < 0 || camera_id >= CAMERA_BACKEND_CAMERA_COUNT)
        return CAMERA_BACKEND_ERR_RANGE;
    return CAMERA_BACKEND_OK;
}

uint32_t to_u32_rounded(float value)
{
    if (!std::isfinite(value) || value <= 0.0f)
        return 0;
    if (value >= static_cast<float>(UINT32_MAX))
        return UINT32_MAX;
    return static_cast<uint32_t>(std::lround(value));
}

int record_aiq_result(CameraSlot &camera, const char *operation,
                      XCamReturn aiq_result)
{
    camera.last_aiq_error = static_cast<int>(aiq_result);
    if (aiq_result == XCAM_RETURN_NO_ERROR)
        return CAMERA_BACKEND_OK;

    std::fprintf(stderr, "CAMERA_AIQ_ERROR sensor=%s operation=%s result=%d\n",
                 camera.sensor_name.c_str(), operation,
                 static_cast<int>(aiq_result));
    return CAMERA_BACKEND_ERR_AIQ;
}

int set_stream_event_subscription(int fd, uint32_t event_type, bool subscribe)
{
    struct v4l2_event_subscription subscription = {};
    subscription.type = event_type;
    const unsigned long request = subscribe ? VIDIOC_SUBSCRIBE_EVENT
                                            : VIDIOC_UNSUBSCRIBE_EVENT;
    int result;
    do {
        result = ioctl(fd, request, &subscription);
    } while (result < 0 && errno == EINTR);
    return result;
}

void stream_event_worker(CameraSlot *camera)
{
    while (!camera->stop_event_thread.load()) {
        struct v4l2_event event = {};
        int result;
        do {
            result = ioctl(camera->event_fd, VIDIOC_DQEVENT, &event);
        } while (result < 0 && errno == EINTR);

        if (result < 0) {
            if (errno == EAGAIN || errno == ENOENT) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (!camera->stop_event_thread.load()) {
                std::fprintf(stderr,
                             "CAMERA_EVENT_ERROR sensor=%s params=%s operation=dqevent errno=%d\n",
                             camera->sensor_name.c_str(),
                             camera->params_device.c_str(), errno);
            }
            break;
        }

        std::lock_guard<std::mutex> lock(camera->mutex);
        if (event.type == kStreamStartEvent && !camera->started) {
            XCamReturn aiq_result = rk_aiq_uapi2_sysctl_start(camera->ctx);
            if (record_aiq_result(*camera, "start", aiq_result) ==
                CAMERA_BACKEND_OK) {
                camera->started = true;
                std::fprintf(stdout,
                             "CAMERA_STREAM sensor=%s state=STARTED params=%s\n",
                             camera->sensor_name.c_str(),
                             camera->params_device.c_str());
                std::fflush(stdout);
            }
        } else if (event.type == kStreamStopEvent && camera->started) {
            XCamReturn aiq_result =
                rk_aiq_uapi2_sysctl_stop(camera->ctx, false);
            if (record_aiq_result(*camera, "stop", aiq_result) ==
                CAMERA_BACKEND_OK) {
                camera->started = false;
                camera->manual_mode = false;
                camera->requested_fps = 0;
                std::fprintf(stdout,
                             "CAMERA_STREAM sensor=%s state=STOPPED params=%s\n",
                             camera->sensor_name.c_str(),
                             camera->params_device.c_str());
                std::fflush(stdout);
            }
        }
    }
}

void stop_event_worker(CameraSlot &camera)
{
    camera.stop_event_thread.store(true);
    if (camera.event_thread.joinable())
        camera.event_thread.join();

    if (camera.event_fd >= 0) {
        set_stream_event_subscription(camera.event_fd, kStreamStartEvent, false);
        set_stream_event_subscription(camera.event_fd, kStreamStopEvent, false);
        close(camera.event_fd);
        camera.event_fd = -1;
    }
}

void shutdown_camera(CameraSlot &camera)
{
    if (camera.ctx == nullptr)
        return;

    if (camera.started) {
        XCamReturn result = rk_aiq_uapi2_sysctl_stop(camera.ctx, false);
        if (result != XCAM_RETURN_NO_ERROR) {
            std::fprintf(stderr,
                         "CAMERA_AIQ_ERROR sensor=%s operation=stop result=%d\n",
                         camera.sensor_name.c_str(), static_cast<int>(result));
        }
    }
    rk_aiq_uapi2_sysctl_deinit(camera.ctx);
    camera.ctx = nullptr;
    camera.started = false;
}

}  // namespace

struct camera_backend {
    CameraSlot cameras[CAMERA_BACKEND_CAMERA_COUNT];
    uint32_t width = 0;
    uint32_t height = 0;
};

extern "C" void camera_backend_default_config(camera_backend_config_t *config)
{
    if (config == nullptr)
        return;

    std::memset(config, 0, sizeof(*config));
    config->width = 4000;
    config->height = 3000;
    config->iq_dir[0] = "/etc/iqfiles/cam0";
    config->iq_dir[1] = "/etc/iqfiles/cam1";
    config->expected_sensor[0] = "imx586";
    config->expected_sensor[1] = "imx586";
    config->params_device[0] = "/dev/video29";
    config->params_device[1] = "/dev/video38";
}

extern "C" int camera_backend_create(const camera_backend_config_t *config,
                                     camera_backend_t **backend_out)
{
    if (config == nullptr || backend_out == nullptr || config->width == 0 ||
        config->height == 0) {
        return CAMERA_BACKEND_ERR_ARGUMENT;
    }
    *backend_out = nullptr;

    for (int camera_id = 0; camera_id < CAMERA_BACKEND_CAMERA_COUNT;
         ++camera_id) {
        if (!directory_exists(config->iq_dir[camera_id]) ||
            config->params_device[camera_id] == nullptr ||
            config->params_device[camera_id][0] == '\0') {
            std::fprintf(stderr, "CAMERA_CONFIG_ERROR camera_id=%d iq_dir=%s\n",
                         camera_id,
                         config->iq_dir[camera_id] != nullptr
                             ? config->iq_dir[camera_id]
                             : "(null)");
            return CAMERA_BACKEND_ERR_IO;
        }
    }

    camera_backend_t *backend = new (std::nothrow) camera_backend_t;
    if (backend == nullptr)
        return CAMERA_BACKEND_ERR_IO;
    backend->width = config->width;
    backend->height = config->height;

    int result = CAMERA_BACKEND_OK;

    // Build both contexts before preparing either one. Rockchip's multi-camera
    // samples use the same two-phase ordering so each ISP is known up front.
    for (int camera_id = 0; camera_id < CAMERA_BACKEND_CAMERA_COUNT;
         ++camera_id) {
        CameraSlot &camera = backend->cameras[camera_id];
        rk_aiq_static_info_t static_info = {};

        XCamReturn aiq_result = rk_aiq_uapi2_sysctl_enumStaticMetasByPhyId(
            camera_id, &static_info);
        if (aiq_result != XCAM_RETURN_NO_ERROR) {
            std::fprintf(stderr,
                         "CAMERA_AIQ_ERROR camera_id=%d operation=enum result=%d\n",
                         camera_id, static_cast<int>(aiq_result));
            result = CAMERA_BACKEND_ERR_AIQ;
            break;
        }

        camera.sensor_name = static_info.sensor_info.sensor_name;
        camera.iq_dir = config->iq_dir[camera_id];
        camera.params_device = config->params_device[camera_id];
        const char *expected = config->expected_sensor[camera_id];
        if (expected != nullptr && expected[0] != '\0' &&
            camera.sensor_name.find(expected) == std::string::npos) {
            std::fprintf(stderr,
                         "CAMERA_CONFIG_ERROR camera_id=%d expected=%s actual=%s\n",
                         camera_id, expected, camera.sensor_name.c_str());
            result = CAMERA_BACKEND_ERR_SENSOR_MISMATCH;
            break;
        }

        camera.ctx = rk_aiq_uapi2_sysctl_init(camera.sensor_name.c_str(),
                                              camera.iq_dir.c_str(), nullptr,
                                              nullptr);
        if (camera.ctx == nullptr) {
            std::fprintf(stderr,
                         "CAMERA_AIQ_ERROR camera_id=%d sensor=%s operation=init iq=%s\n",
                         camera_id, camera.sensor_name.c_str(),
                         camera.iq_dir.c_str());
            result = CAMERA_BACKEND_ERR_AIQ;
            break;
        }

        rk_aiq_uapi2_sysctl_setMulCamConc(camera.ctx, true);
        rk_aiq_uapi2_sysctl_setListenStrmStatus(camera.ctx, false);
    }

    for (int camera_id = 0;
         result == CAMERA_BACKEND_OK &&
         camera_id < CAMERA_BACKEND_CAMERA_COUNT;
         ++camera_id) {
        CameraSlot &camera = backend->cameras[camera_id];
        XCamReturn aiq_result = rk_aiq_uapi2_sysctl_prepare(
            camera.ctx, backend->width, backend->height,
            RK_AIQ_WORKING_MODE_NORMAL);
        if (record_aiq_result(camera, "prepare", aiq_result) !=
            CAMERA_BACKEND_OK) {
            result = CAMERA_BACKEND_ERR_AIQ;
            break;
        }

        camera.event_fd = open(camera.params_device.c_str(),
                               O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (camera.event_fd < 0) {
            std::fprintf(stderr,
                         "CAMERA_EVENT_ERROR camera_id=%d params=%s operation=open errno=%d\n",
                         camera_id, camera.params_device.c_str(), errno);
            result = CAMERA_BACKEND_ERR_IO;
            break;
        }
        if (set_stream_event_subscription(camera.event_fd, kStreamStartEvent,
                                          true) < 0 ||
            set_stream_event_subscription(camera.event_fd, kStreamStopEvent,
                                          true) < 0) {
            std::fprintf(stderr,
                         "CAMERA_EVENT_ERROR camera_id=%d params=%s operation=subscribe errno=%d\n",
                         camera_id, camera.params_device.c_str(), errno);
            result = CAMERA_BACKEND_ERR_IO;
            break;
        }

        try {
            camera.event_thread = std::thread(stream_event_worker, &camera);
        } catch (...) {
            std::fprintf(stderr,
                         "CAMERA_EVENT_ERROR camera_id=%d params=%s operation=thread\n",
                         camera_id, camera.params_device.c_str());
            result = CAMERA_BACKEND_ERR_IO;
            break;
        }

        std::fprintf(stdout,
                     "CAMERA_INIT camera_id=%d sensor=%s iq=%s params=%s size=%ux%u state=PREPARED\n",
                     camera_id, camera.sensor_name.c_str(),
                     camera.iq_dir.c_str(), camera.params_device.c_str(),
                     backend->width, backend->height);
        std::fflush(stdout);
    }

    if (result != CAMERA_BACKEND_OK) {
        for (int camera_id = 0; camera_id < CAMERA_BACKEND_CAMERA_COUNT;
             ++camera_id) {
            backend->cameras[camera_id].stop_event_thread.store(true);
        }
        for (int camera_id = 0; camera_id < CAMERA_BACKEND_CAMERA_COUNT;
             ++camera_id) {
            stop_event_worker(backend->cameras[camera_id]);
        }
        for (int camera_id = CAMERA_BACKEND_CAMERA_COUNT - 1; camera_id >= 0;
             --camera_id) {
            shutdown_camera(backend->cameras[camera_id]);
        }
        delete backend;
        return result;
    }

    *backend_out = backend;
    return CAMERA_BACKEND_OK;
}

extern "C" void camera_backend_destroy(camera_backend_t *backend)
{
    if (backend == nullptr)
        return;

    for (int camera_id = 0; camera_id < CAMERA_BACKEND_CAMERA_COUNT;
         ++camera_id) {
        backend->cameras[camera_id].stop_event_thread.store(true);
    }
    for (int camera_id = 0; camera_id < CAMERA_BACKEND_CAMERA_COUNT;
         ++camera_id) {
        stop_event_worker(backend->cameras[camera_id]);
    }

    for (int camera_id = CAMERA_BACKEND_CAMERA_COUNT - 1; camera_id >= 0;
         --camera_id) {
        std::lock_guard<std::mutex> lock(backend->cameras[camera_id].mutex);
        shutdown_camera(backend->cameras[camera_id]);
    }
    delete backend;
}

extern "C" int camera_backend_set_auto(camera_backend_t *backend,
                                       int camera_id)
{
    int result = validate_camera_id(backend, camera_id);
    if (result != CAMERA_BACKEND_OK)
        return result;

    CameraSlot &camera = backend->cameras[camera_id];
    std::lock_guard<std::mutex> lock(camera.mutex);
    if (camera.ctx == nullptr || !camera.started)
        return CAMERA_BACKEND_ERR_NOT_READY;

    result = record_aiq_result(camera, "setExpMode(auto)",
                               rk_aiq_uapi2_setExpMode(camera.ctx, OP_AUTO));
    if (result == CAMERA_BACKEND_OK)
        camera.manual_mode = false;
    return result;
}
extern "C" int camera_backend_set_exposure(camera_backend_t *backend,
                                           int camera_id,
                                           uint32_t exposure_us)
{
    int result = validate_camera_id(backend, camera_id);
    if (result != CAMERA_BACKEND_OK)
        return result;
    if (exposure_us < kMinExposureUs || exposure_us > kMaxExposureUs)
        return CAMERA_BACKEND_ERR_RANGE;

    CameraSlot &camera = backend->cameras[camera_id];
    std::lock_guard<std::mutex> lock(camera.mutex);
    if (camera.ctx == nullptr || !camera.started)
        return CAMERA_BACKEND_ERR_NOT_READY;

    ae_api_queryInfo_t current = {};
    XCamReturn query_result =
        rk_aiq_user_api2_ae_queryExpResInfo(camera.ctx, &current);
    result = record_aiq_result(camera, "queryBeforeSetExposure", query_result);
    if (result != CAMERA_BACKEND_OK)
        return result;

    const float current_time =
        current.linExpInfo.expParam.integration_time;
    const float current_gain = current.linExpInfo.expParam.analog_gain;
    if (!std::isfinite(current_time) || current_time <= 0.0f ||
        !std::isfinite(current_gain) || current_gain < 1.0f) {
        std::fprintf(stderr,
                     "CAMERA_AIQ_ERROR sensor=%s operation=queryBeforeSetExposure invalid_time=%f invalid_gain=%f\n",
                     camera.sensor_name.c_str(), current_time, current_gain);
        return CAMERA_BACKEND_ERR_NOT_READY;
    }

    result = record_aiq_result(
        camera, "setExpMode(exposure)",
        rk_aiq_uapi2_setExpMode(camera.ctx, OP_MANUAL));
    if (result != CAMERA_BACKEND_OK)
        return result;
    result = record_aiq_result(
        camera, "setExpTimeMode(exposure)",
        rk_aiq_uapi2_setExpTimeMode(camera.ctx, OP_MANUAL));
    if (result != CAMERA_BACKEND_OK)
        return result;
    result = record_aiq_result(
        camera, "setExpGainMode(exposure)",
        rk_aiq_uapi2_setExpGainMode(camera.ctx, OP_MANUAL));
    if (result != CAMERA_BACKEND_OK)
        return result;

    result = record_aiq_result(
        camera, "seedExposureTime",
        rk_aiq_uapi2_setExpManualTime(camera.ctx, current_time));
    if (result != CAMERA_BACKEND_OK)
        return result;
    result = record_aiq_result(
        camera, "holdExposureGain",
        rk_aiq_uapi2_setExpManualGain(camera.ctx, current_gain));
    if (result != CAMERA_BACKEND_OK)
        return result;

    const float exposure_seconds = static_cast<float>(exposure_us) / 1000000.0f;
    result = record_aiq_result(
        camera, "setExposureOnly",
        rk_aiq_uapi2_setExpManualTime(camera.ctx, exposure_seconds));
    if (result == CAMERA_BACKEND_OK)
        camera.manual_mode = true;
    return result;
}

extern "C" int camera_backend_set_gain(camera_backend_t *backend,
                                       int camera_id,
                                       uint32_t gain_x1000)
{
    int result = validate_camera_id(backend, camera_id);
    if (result != CAMERA_BACKEND_OK)
        return result;
    if (gain_x1000 < kMinGainX1000 || gain_x1000 > kMaxGainX1000)
        return CAMERA_BACKEND_ERR_RANGE;

    CameraSlot &camera = backend->cameras[camera_id];
    std::lock_guard<std::mutex> lock(camera.mutex);
    if (camera.ctx == nullptr || !camera.started)
        return CAMERA_BACKEND_ERR_NOT_READY;

    ae_api_queryInfo_t current = {};
    XCamReturn query_result =
        rk_aiq_user_api2_ae_queryExpResInfo(camera.ctx, &current);
    result = record_aiq_result(camera, "queryBeforeSetGain", query_result);
    if (result != CAMERA_BACKEND_OK)
        return result;

    const float current_time =
        current.linExpInfo.expParam.integration_time;
    const float current_gain = current.linExpInfo.expParam.analog_gain;
    if (!std::isfinite(current_time) || current_time <= 0.0f ||
        !std::isfinite(current_gain) || current_gain < 1.0f) {
        std::fprintf(stderr,
                     "CAMERA_AIQ_ERROR sensor=%s operation=queryBeforeSetGain invalid_time=%f invalid_gain=%f\n",
                     camera.sensor_name.c_str(), current_time, current_gain);
        return CAMERA_BACKEND_ERR_NOT_READY;
    }

    result = record_aiq_result(
        camera, "setExpMode(gain)",
        rk_aiq_uapi2_setExpMode(camera.ctx, OP_MANUAL));
    if (result != CAMERA_BACKEND_OK)
        return result;
    result = record_aiq_result(
        camera, "setExpTimeMode(gain)",
        rk_aiq_uapi2_setExpTimeMode(camera.ctx, OP_MANUAL));
    if (result != CAMERA_BACKEND_OK)
        return result;
    result = record_aiq_result(
        camera, "setExpGainMode(gain)",
        rk_aiq_uapi2_setExpGainMode(camera.ctx, OP_MANUAL));
    if (result != CAMERA_BACKEND_OK)
        return result;

    result = record_aiq_result(
        camera, "holdGainTime",
        rk_aiq_uapi2_setExpManualTime(camera.ctx, current_time));
    if (result != CAMERA_BACKEND_OK)
        return result;
    result = record_aiq_result(
        camera, "seedGain",
        rk_aiq_uapi2_setExpManualGain(camera.ctx, current_gain));
    if (result != CAMERA_BACKEND_OK)
        return result;

    const float gain = static_cast<float>(gain_x1000) / 1000.0f;
    result = record_aiq_result(
        camera, "setAnalogGainOnly",
        rk_aiq_uapi2_setExpManualGain(camera.ctx, gain));
    if (result == CAMERA_BACKEND_OK)
        camera.manual_mode = true;
    return result;
}

extern "C" int camera_backend_set_fps(camera_backend_t *backend, int camera_id,
                                      uint32_t fps)
{
    int result = validate_camera_id(backend, camera_id);
    if (result != CAMERA_BACKEND_OK)
        return result;
    if (fps < kMinFps || fps > kMaxFps)
        return CAMERA_BACKEND_ERR_RANGE;

    CameraSlot &camera = backend->cameras[camera_id];
    std::lock_guard<std::mutex> lock(camera.mutex);
    if (camera.ctx == nullptr || !camera.started)
        return CAMERA_BACKEND_ERR_NOT_READY;

    frameRateInfo_t frame_rate = {};
    frame_rate.mode = OP_MANUAL;
    frame_rate.fps = fps;
    result = record_aiq_result(
        camera, "setFrameRate",
        rk_aiq_uapi2_setFrameRate(camera.ctx, frame_rate));
    if (result == CAMERA_BACKEND_OK)
        camera.requested_fps = fps;
    return result;
}

extern "C" int camera_backend_get_status(camera_backend_t *backend,
                                         int camera_id,
                                         camera_backend_status_t *status)
{
    if (status == nullptr)
        return CAMERA_BACKEND_ERR_ARGUMENT;

    int result = validate_camera_id(backend, camera_id);
    if (result != CAMERA_BACKEND_OK)
        return result;

    CameraSlot &camera = backend->cameras[camera_id];
    std::lock_guard<std::mutex> lock(camera.mutex);
    std::memset(status, 0, sizeof(*status));
    status->camera_id = camera_id;
    status->online = camera.ctx != nullptr;
    status->started = camera.started;
    status->manual_mode = camera.manual_mode;
    status->last_aiq_error = camera.last_aiq_error;
    std::snprintf(status->sensor_name, sizeof(status->sensor_name), "%s",
                  camera.sensor_name.c_str());
    std::snprintf(status->iq_dir, sizeof(status->iq_dir), "%s",
                  camera.iq_dir.c_str());

    if (camera.ctx == nullptr || !camera.started)
        return CAMERA_BACKEND_ERR_NOT_READY;

    opMode_t mode = OP_AUTO;
    XCamReturn mode_result = rk_aiq_uapi2_getExpMode(camera.ctx, &mode);
    if (mode_result == XCAM_RETURN_NO_ERROR)
        status->manual_mode = mode == OP_MANUAL;

    ae_api_queryInfo_t query = {};
    XCamReturn query_result =
        rk_aiq_user_api2_ae_queryExpResInfo(camera.ctx, &query);
    if (query_result == XCAM_RETURN_NO_ERROR) {
        status->query_valid = 1;
        status->converged = query.isConverged;
        status->exposure_us = to_u32_rounded(
            query.linExpInfo.expParam.integration_time * 1000000.0f);
        status->gain_x1000 = to_u32_rounded(
            query.linExpInfo.expParam.analog_gain * 1000.0f);
        status->iso = query.linExpInfo.expParam.iso;
        status->fps_x1000 = to_u32_rounded(query.fps * 1000.0f);
        status->mean_luma = query.linExpInfo.meanLuma;
    } else if (camera.requested_fps != 0) {
        status->fps_x1000 = camera.requested_fps * 1000;
    }

    if (query_result != XCAM_RETURN_NO_ERROR)
        status->last_aiq_error = static_cast<int>(query_result);
    else if (mode_result != XCAM_RETURN_NO_ERROR)
        status->last_aiq_error = static_cast<int>(mode_result);
    else
        status->last_aiq_error = 0;

    return CAMERA_BACKEND_OK;
}

extern "C" const char *camera_backend_strerror(int result)
{
    switch (result) {
    case CAMERA_BACKEND_OK:
        return "success";
    case CAMERA_BACKEND_ERR_ARGUMENT:
        return "invalid argument";
    case CAMERA_BACKEND_ERR_RANGE:
        return "value out of range";
    case CAMERA_BACKEND_ERR_IO:
        return "IQ directory or memory error";
    case CAMERA_BACKEND_ERR_AIQ:
        return "RKAIQ operation failed";
    case CAMERA_BACKEND_ERR_NOT_READY:
        return "camera is not ready";
    case CAMERA_BACKEND_ERR_SENSOR_MISMATCH:
        return "camera sensor does not match configuration";
    default:
        return "unknown camera backend error";
    }
}
