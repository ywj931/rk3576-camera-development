#define MODULE_TAG "camera_uvc"

#include "camera_uvc_backend.h"

extern "C" {
#include "uvc_control.h"
#include "rk_mpi.h"
#include "mpp_buffer.h"
#include "mpp_frame.h"
#include "mpp_packet.h"
#include "rk_venc_cfg.h"
}

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

namespace {

constexpr size_t kQueueDepth = 1;
constexpr uint32_t kMjpegFourcc = 0x47504a4dU;

struct Nv12Frame {
    std::vector<uint8_t> bytes;
    uint32_t sequence = 0;
};

class MppJpegEncoder {
public:
    int initialize(uint32_t width, uint32_t height, uint32_t fps,
                   uint32_t quality) {
        width_ = width;
        height_ = height;
        hor_stride_ = (width + 15U) & ~15U;
        ver_stride_ = (height + 15U) & ~15U;
        input_size_ = static_cast<size_t>(width) * height * 3 / 2;
        frame_size_ = static_cast<size_t>(hor_stride_) * ver_stride_ * 3 / 2;
        packet_size_ = static_cast<size_t>(width) * height * 2;

        MPP_RET ret = mpp_buffer_group_get_internal(
            &buffer_group_, MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE);
        if (ret != MPP_OK)
            return ret;
        ret = mpp_buffer_get(buffer_group_, &frame_buffer_, frame_size_);
        if (ret != MPP_OK)
            return ret;
        ret = mpp_buffer_get(buffer_group_, &packet_buffer_, packet_size_);
        if (ret != MPP_OK)
            return ret;
        ret = mpp_create(&context_, &mpi_);
        if (ret != MPP_OK)
            return ret;

        MppPollType timeout = MPP_POLL_BLOCK;
        ret = mpi_->control(context_, MPP_SET_OUTPUT_TIMEOUT, &timeout);
        if (ret != MPP_OK)
            return ret;
        ret = mpp_init(context_, MPP_CTX_ENC, MPP_VIDEO_CodingMJPEG);
        if (ret != MPP_OK)
            return ret;
        ret = mpp_enc_cfg_init(&config_);
        if (ret != MPP_OK)
            return ret;
        ret = mpi_->control(context_, MPP_ENC_GET_CFG, config_);
        if (ret != MPP_OK)
            return ret;

        mpp_enc_cfg_set_s32(config_, "prep:width", width);
        mpp_enc_cfg_set_s32(config_, "prep:height", height);
        mpp_enc_cfg_set_s32(config_, "prep:hor_stride", hor_stride_);
        mpp_enc_cfg_set_s32(config_, "prep:ver_stride", ver_stride_);
        mpp_enc_cfg_set_s32(config_, "prep:format", MPP_FMT_YUV420SP);
        mpp_enc_cfg_set_s32(config_, "prep:range", MPP_FRAME_RANGE_JPEG);
        mpp_enc_cfg_set_s32(config_, "rc:mode", MPP_ENC_RC_MODE_FIXQP);
        mpp_enc_cfg_set_s32(config_, "rc:fps_in_flex", 0);
        mpp_enc_cfg_set_s32(config_, "rc:fps_in_num", fps);
        mpp_enc_cfg_set_s32(config_, "rc:fps_in_denom", 1);
        mpp_enc_cfg_set_s32(config_, "rc:fps_out_flex", 0);
        mpp_enc_cfg_set_s32(config_, "rc:fps_out_num", fps);
        mpp_enc_cfg_set_s32(config_, "rc:fps_out_denom", 1);
        mpp_enc_cfg_set_s32(config_, "codec:type", MPP_VIDEO_CodingMJPEG);
        mpp_enc_cfg_set_s32(config_, "jpeg:q_factor", quality);
        mpp_enc_cfg_set_s32(config_, "jpeg:qf_max", 99);
        mpp_enc_cfg_set_s32(config_, "jpeg:qf_min", 1);

        ret = mpi_->control(context_, MPP_ENC_SET_CFG, config_);
        return ret;
    }

    ~MppJpegEncoder() { shutdown(); }

    void shutdown() {
        if (context_ && mpi_)
            mpi_->reset(context_);
        if (context_) {
            mpp_destroy(context_);
            context_ = nullptr;
            mpi_ = nullptr;
        }
        if (config_) {
            mpp_enc_cfg_deinit(config_);
            config_ = nullptr;
        }
        if (packet_buffer_) {
            mpp_buffer_put(packet_buffer_);
            packet_buffer_ = nullptr;
        }
        if (frame_buffer_) {
            mpp_buffer_put(frame_buffer_);
            frame_buffer_ = nullptr;
        }
        if (buffer_group_) {
            mpp_buffer_group_put(buffer_group_);
            buffer_group_ = nullptr;
        }
    }

    int encode(const std::vector<uint8_t> &nv12, std::vector<uint8_t> *jpeg) {
        if (!jpeg || nv12.size() != input_size_ || !mpi_)
            return MPP_ERR_VALUE;

        void *input = mpp_buffer_get_ptr(frame_buffer_);
        if (!input)
            return MPP_ERR_NOMEM;
        mpp_buffer_sync_begin(frame_buffer_);
        std::memset(input, 0, frame_size_);
        const uint8_t *src_y = nv12.data();
        uint8_t *dst_y = static_cast<uint8_t *>(input);
        for (uint32_t row = 0; row < height_; ++row) {
            std::memcpy(dst_y + static_cast<size_t>(row) * hor_stride_,
                        src_y + static_cast<size_t>(row) * width_, width_);
        }
        const uint8_t *src_uv = src_y + static_cast<size_t>(width_) * height_;
        uint8_t *dst_uv = dst_y + static_cast<size_t>(hor_stride_) * ver_stride_;
        for (uint32_t row = 0; row < height_ / 2; ++row) {
            std::memcpy(dst_uv + static_cast<size_t>(row) * hor_stride_,
                        src_uv + static_cast<size_t>(row) * width_, width_);
        }
        mpp_buffer_sync_end(frame_buffer_);

        MppFrame frame = nullptr;
        MPP_RET ret = mpp_frame_init(&frame);
        if (ret != MPP_OK)
            return ret;
        mpp_frame_set_width(frame, width_);
        mpp_frame_set_height(frame, height_);
        mpp_frame_set_hor_stride(frame, hor_stride_);
        mpp_frame_set_ver_stride(frame, ver_stride_);
        mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
        mpp_frame_set_buffer(frame, frame_buffer_);

        MppPacket packet = nullptr;
        ret = mpp_packet_init_with_buffer(&packet, packet_buffer_);
        if (ret != MPP_OK) {
            mpp_frame_deinit(&frame);
            return ret;
        }
        mpp_packet_set_length(packet, 0);
        MppMeta meta = mpp_frame_get_meta(frame);
        mpp_meta_set_packet(meta, KEY_OUTPUT_PACKET, packet);

        ret = mpi_->encode_put_frame(context_, frame);
        mpp_frame_deinit(&frame);
        if (ret == MPP_OK)
            ret = mpi_->encode_get_packet(context_, &packet);

        if (ret == MPP_OK && packet) {
            const uint8_t *data = static_cast<const uint8_t *>(
                mpp_packet_get_pos(packet));
            const size_t length = mpp_packet_get_length(packet);
            if (!data || !length || length > packet_size_)
                ret = MPP_ERR_VALUE;
            else
                jpeg->assign(data, data + length);
        }
        if (packet)
            mpp_packet_deinit(&packet);
        return ret;
    }

private:
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t hor_stride_ = 0;
    uint32_t ver_stride_ = 0;
    size_t input_size_ = 0;
    size_t frame_size_ = 0;
    size_t packet_size_ = 0;
    MppCtx context_ = nullptr;
    MppApi *mpi_ = nullptr;
    MppEncCfg config_ = nullptr;
    MppBufferGroup buffer_group_ = nullptr;
    MppBuffer frame_buffer_ = nullptr;
    MppBuffer packet_buffer_ = nullptr;
};

}  // namespace

struct camera_uvc_channel {
    std::mutex mutex;
    std::condition_variable condition;
    std::thread worker;
    bool enabled = false;
    bool host_open = false;
    bool host_streaming = false;
    bool stop_worker = false;
    int source_camera_id = -1;
    int last_error = CAMERA_UVC_OK;
    int last_mpp_error = MPP_OK;
    uint32_t negotiated_width = 0;
    uint32_t negotiated_height = 0;
    uint32_t negotiated_fps = 0;
    uint32_t negotiated_fcc = 0;
    uint32_t source_fps = CAMERA_UVC_FPS;
    uint32_t last_sequence = 0;
    uint64_t frames_submitted = 0;
    uint64_t frames_encoded = 0;
    uint64_t frames_sent = 0;
    uint64_t frames_skipped_no_host = 0;
    uint64_t frames_rate_limited = 0;
    uint64_t queue_drops = 0;
    uint64_t encode_errors = 0;
    uint64_t jpeg_bytes = 0;
    bool pacing_initialized = false;
    std::chrono::steady_clock::time_point next_submit_time;
    std::deque<Nv12Frame> queue;
    MppJpegEncoder encoder;
};

struct camera_uvc_backend {
    camera_uvc_config_t config{};
    std::mutex control_mutex;
    bool control_running = false;
    camera_uvc_channel channels[CAMERA_UVC_CAMERA_COUNT];
};

namespace {

std::mutex g_active_mutex;
camera_uvc_backend_t *g_active_backend = nullptr;

int on_uvc_open(unsigned int index, int width, int height, int fcc, int fps) {
    std::lock_guard<std::mutex> active_lock(g_active_mutex);
    if (!g_active_backend || index >= CAMERA_UVC_CAMERA_COUNT)
        return -1;

    camera_uvc_channel &channel = g_active_backend->channels[index];
    std::lock_guard<std::mutex> lock(channel.mutex);
    channel.negotiated_width = width;
    channel.negotiated_height = height;
    channel.negotiated_fps = fps;
    channel.negotiated_fcc = static_cast<uint32_t>(fcc);
    channel.host_open = true;
    if (width != static_cast<int>(g_active_backend->config.width) ||
        height != static_cast<int>(g_active_backend->config.height) ||
        fcc != static_cast<int>(kMjpegFourcc) || (fps != 2 && fps != 4)) {
        channel.last_error = CAMERA_UVC_ERR_UNSUPPORTED;
        channel.host_streaming = false;
        return -1;
    }
    channel.host_streaming = channel.enabled;
    channel.pacing_initialized = false;
    return 0;
}

void on_uvc_close(unsigned int index) {
    std::lock_guard<std::mutex> active_lock(g_active_mutex);
    if (!g_active_backend || index >= CAMERA_UVC_CAMERA_COUNT)
        return;
    camera_uvc_channel &channel = g_active_backend->channels[index];
    std::lock_guard<std::mutex> lock(channel.mutex);
    channel.host_open = false;
    channel.host_streaming = false;
    channel.negotiated_width = 0;
    channel.negotiated_height = 0;
    channel.negotiated_fps = 0;
    channel.negotiated_fcc = 0;
    channel.pacing_initialized = false;
    channel.queue.clear();
}

void reset_statistics(camera_uvc_channel *channel) {
    channel->last_error = CAMERA_UVC_OK;
    channel->last_mpp_error = MPP_OK;
    channel->last_sequence = 0;
    channel->frames_submitted = 0;
    channel->frames_encoded = 0;
    channel->frames_sent = 0;
    channel->frames_skipped_no_host = 0;
    channel->frames_rate_limited = 0;
    channel->queue_drops = 0;
    channel->encode_errors = 0;
    channel->jpeg_bytes = 0;
    channel->pacing_initialized = false;
    channel->queue.clear();
}

void encode_worker(camera_uvc_backend_t *backend, int camera_id) {
    camera_uvc_channel &channel = backend->channels[camera_id];
    for (;;) {
        Nv12Frame frame;
        {
            std::unique_lock<std::mutex> lock(channel.mutex);
            channel.condition.wait(lock, [&channel] {
                return channel.stop_worker || !channel.queue.empty();
            });
            if (channel.stop_worker && channel.queue.empty())
                break;
            frame = std::move(channel.queue.back());
            channel.queue.clear();
        }

        std::vector<uint8_t> jpeg;
        const int ret = channel.encoder.encode(frame.bytes, &jpeg);
        if (ret != MPP_OK) {
            std::lock_guard<std::mutex> lock(channel.mutex);
            channel.last_error = CAMERA_UVC_ERR_MPP;
            channel.last_mpp_error = ret;
            ++channel.encode_errors;
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(channel.mutex);
            ++channel.frames_encoded;
            channel.jpeg_bytes += jpeg.size();
            channel.last_sequence = frame.sequence;
        }

        bool sent = false;
        {
            std::lock_guard<std::mutex> active_lock(g_active_mutex);
            if (g_active_backend == backend) {
                bool host_streaming = false;
                {
                    std::lock_guard<std::mutex> lock(channel.mutex);
                    host_streaming = channel.host_streaming;
                }
                if (host_streaming)
                    sent = uvc_read_camera_buffer_index(
                               camera_id, jpeg.data(), -1, jpeg.size(),
                               nullptr, 0) != 0;
            }
        }
        if (sent) {
            std::lock_guard<std::mutex> lock(channel.mutex);
            ++channel.frames_sent;
        }
    }
}

int start_channel(camera_uvc_backend_t *backend, int camera_id) {
    camera_uvc_channel &channel = backend->channels[camera_id];
    uint32_t source_fps = 0;
    {
        std::lock_guard<std::mutex> lock(channel.mutex);
        if (channel.enabled)
            return CAMERA_UVC_OK;
        reset_statistics(&channel);
        channel.source_camera_id = camera_id;
        source_fps = channel.source_fps;
    }

    int ret = channel.encoder.initialize(backend->config.width,
                                         backend->config.height,
                                         source_fps,
                                         backend->config.jpeg_quality);
    if (ret != MPP_OK) {
        std::lock_guard<std::mutex> lock(channel.mutex);
        channel.last_error = CAMERA_UVC_ERR_MPP;
        channel.last_mpp_error = ret;
        channel.encoder.shutdown();
        return CAMERA_UVC_ERR_MPP;
    }

    {
        std::lock_guard<std::mutex> lock(channel.mutex);
        channel.stop_worker = false;
        channel.enabled = true;
        channel.host_streaming = channel.host_open;
    }
    channel.worker = std::thread(encode_worker, backend, camera_id);
    return CAMERA_UVC_OK;
}

void stop_channel(camera_uvc_channel *channel) {
    {
        std::lock_guard<std::mutex> lock(channel->mutex);
        channel->enabled = false;
        channel->host_streaming = false;
        channel->stop_worker = true;
        channel->queue.clear();
        channel->condition.notify_all();
    }
    if (channel->worker.joinable())
        channel->worker.join();
    channel->encoder.shutdown();
}

int start_control(camera_uvc_backend_t *backend) {
    if (backend->control_running)
        return CAMERA_UVC_OK;

    {
        std::lock_guard<std::mutex> active_lock(g_active_mutex);
        if (g_active_backend && g_active_backend != backend)
            return CAMERA_UVC_ERR_STATE;
        g_active_backend = backend;
        register_uvc_open_camera_indexed(on_uvc_open);
        register_uvc_close_camera_indexed(on_uvc_close);
    }

    setenv("UVC_CNT", "2", 1);
    if (uvc_control_run(UVC_CONTROL_CHECK_STRAIGHT)) {
        std::lock_guard<std::mutex> active_lock(g_active_mutex);
        if (g_active_backend == backend)
            g_active_backend = nullptr;
        register_uvc_open_camera_indexed(nullptr);
        register_uvc_close_camera_indexed(nullptr);
        return CAMERA_UVC_ERR_NODE;
    }
    backend->control_running = true;
    return CAMERA_UVC_OK;
}

void shutdown_backend(camera_uvc_backend_t *backend) {
    std::lock_guard<std::mutex> control_lock(backend->control_mutex);
    for (int camera_id = 0; camera_id < CAMERA_UVC_CAMERA_COUNT; ++camera_id)
        stop_channel(&backend->channels[camera_id]);
    if (backend->control_running)
        uvc_control_join(UVC_CONTROL_CHECK_STRAIGHT);

    {
        std::lock_guard<std::mutex> active_lock(g_active_mutex);
        if (g_active_backend == backend)
            g_active_backend = nullptr;
        register_uvc_open_camera_indexed(nullptr);
        register_uvc_close_camera_indexed(nullptr);
    }
    backend->control_running = false;
}

}  // namespace

void camera_uvc_default_config(camera_uvc_config_t *config) {
    if (!config)
        return;
    config->width = CAMERA_UVC_WIDTH;
    config->height = CAMERA_UVC_HEIGHT;
    config->fps = CAMERA_UVC_FPS;
    config->jpeg_quality = 80;
}

int camera_uvc_create(const camera_uvc_config_t *config,
                      camera_uvc_backend_t **backend_out) {
    if (!config || !backend_out || !config->width || !config->height ||
        !config->fps || !config->jpeg_quality || config->jpeg_quality > 99)
        return CAMERA_UVC_ERR_ARGUMENT;
    if (config->width != CAMERA_UVC_WIDTH ||
        config->height != CAMERA_UVC_HEIGHT)
        return CAMERA_UVC_ERR_UNSUPPORTED;

    camera_uvc_backend_t *backend = new (std::nothrow) camera_uvc_backend_t;
    if (!backend)
        return CAMERA_UVC_ERR_NOMEM;
    backend->config = *config;
    for (int camera_id = 0; camera_id < CAMERA_UVC_CAMERA_COUNT; ++camera_id)
        backend->channels[camera_id].source_fps = config->fps;
    *backend_out = backend;
    return CAMERA_UVC_OK;
}

int camera_uvc_start(camera_uvc_backend_t *backend, int camera_id) {
    if (!backend || camera_id < 0 || camera_id >= CAMERA_UVC_CAMERA_COUNT)
        return CAMERA_UVC_ERR_ARGUMENT;
    std::lock_guard<std::mutex> control_lock(backend->control_mutex);
    int ret = start_channel(backend, camera_id);
    if (ret != CAMERA_UVC_OK)
        return ret;
    ret = start_control(backend);
    if (ret != CAMERA_UVC_OK)
        stop_channel(&backend->channels[camera_id]);
    return ret;
}

int camera_uvc_start_all(camera_uvc_backend_t *backend) {
    if (!backend)
        return CAMERA_UVC_ERR_ARGUMENT;
    std::lock_guard<std::mutex> control_lock(backend->control_mutex);

    bool newly_started[CAMERA_UVC_CAMERA_COUNT] = {};
    for (int camera_id = 0; camera_id < CAMERA_UVC_CAMERA_COUNT; ++camera_id) {
        bool was_enabled = false;
        {
            std::lock_guard<std::mutex> lock(
                backend->channels[camera_id].mutex);
            was_enabled = backend->channels[camera_id].enabled;
        }
        const int ret = start_channel(backend, camera_id);
        if (ret != CAMERA_UVC_OK) {
            for (int rollback = 0; rollback < CAMERA_UVC_CAMERA_COUNT;
                 ++rollback) {
                if (newly_started[rollback])
                    stop_channel(&backend->channels[rollback]);
            }
            return ret;
        }
        newly_started[camera_id] = !was_enabled;
    }

    const int ret = start_control(backend);
    if (ret != CAMERA_UVC_OK) {
        for (int camera_id = 0; camera_id < CAMERA_UVC_CAMERA_COUNT;
             ++camera_id) {
            if (newly_started[camera_id])
                stop_channel(&backend->channels[camera_id]);
        }
    }
    return ret;
}

int camera_uvc_stop_camera(camera_uvc_backend_t *backend, int camera_id) {
    if (!backend || camera_id < 0 || camera_id >= CAMERA_UVC_CAMERA_COUNT)
        return CAMERA_UVC_ERR_ARGUMENT;

    std::lock_guard<std::mutex> control_lock(backend->control_mutex);
    stop_channel(&backend->channels[camera_id]);
    return CAMERA_UVC_OK;
}

int camera_uvc_stop(camera_uvc_backend_t *backend) {
    if (!backend)
        return CAMERA_UVC_ERR_ARGUMENT;

    std::lock_guard<std::mutex> control_lock(backend->control_mutex);
    for (int camera_id = 0; camera_id < CAMERA_UVC_CAMERA_COUNT; ++camera_id)
        stop_channel(&backend->channels[camera_id]);
    return CAMERA_UVC_OK;
}

int camera_uvc_set_source_fps(camera_uvc_backend_t *backend, int camera_id,
                              uint32_t fps) {
    if (!backend || camera_id < 0 || camera_id >= CAMERA_UVC_CAMERA_COUNT)
        return CAMERA_UVC_ERR_ARGUMENT;
    if (fps != 2 && fps != 4)
        return CAMERA_UVC_ERR_UNSUPPORTED;
    camera_uvc_channel &channel = backend->channels[camera_id];
    std::lock_guard<std::mutex> lock(channel.mutex);
    channel.source_fps = fps;
    channel.pacing_initialized = false;
    return CAMERA_UVC_OK;
}

int camera_uvc_submit_nv12(camera_uvc_backend_t *backend, int camera_id,
                           const void *plane0, size_t plane0_size,
                           const void *plane1, size_t plane1_size,
                           uint32_t sequence) {
    if (!backend || camera_id < 0 || camera_id >= CAMERA_UVC_CAMERA_COUNT ||
        !plane0 || !plane1)
        return CAMERA_UVC_ERR_ARGUMENT;
    camera_uvc_channel &channel = backend->channels[camera_id];
    const size_t y_size = static_cast<size_t>(backend->config.width) *
                          backend->config.height;
    const size_t uv_size = y_size / 2;
    if (plane0_size < y_size || plane1_size < uv_size)
        return CAMERA_UVC_ERR_ARGUMENT;

    {
        std::lock_guard<std::mutex> lock(channel.mutex);
        if (!channel.enabled || channel.source_camera_id != camera_id)
            return CAMERA_UVC_ERR_STATE;
        if (!channel.host_streaming) {
            ++channel.frames_skipped_no_host;
            return CAMERA_UVC_OK;
        }

        const uint32_t fps = channel.negotiated_fps
                                 ? channel.negotiated_fps
                                 : channel.source_fps;
        if (fps) {
            const auto now = std::chrono::steady_clock::now();
            const auto interval = std::chrono::nanoseconds(1000000000ULL / fps);
            if (channel.pacing_initialized &&
                now < channel.next_submit_time) {
                ++channel.frames_rate_limited;
                return CAMERA_UVC_OK;
            }
            if (!channel.pacing_initialized ||
                now >= channel.next_submit_time + interval)
                channel.next_submit_time = now + interval;
            else
                channel.next_submit_time += interval;
            channel.pacing_initialized = true;
        }
    }

    Nv12Frame frame;
    try {
        frame.bytes.resize(y_size + uv_size);
    } catch (const std::bad_alloc &) {
        return CAMERA_UVC_ERR_NOMEM;
    }
    std::memcpy(frame.bytes.data(), plane0, y_size);
    std::memcpy(frame.bytes.data() + y_size, plane1, uv_size);
    frame.sequence = sequence;

    {
        std::lock_guard<std::mutex> lock(channel.mutex);
        if (!channel.enabled || !channel.host_streaming)
            return CAMERA_UVC_OK;
        if (channel.queue.size() >= kQueueDepth) {
            channel.queue.pop_front();
            ++channel.queue_drops;
        }
        channel.queue.emplace_back(std::move(frame));
        ++channel.frames_submitted;
        channel.last_sequence = sequence;
        channel.condition.notify_one();
    }
    return CAMERA_UVC_OK;
}

int camera_uvc_get_status(camera_uvc_backend_t *backend, int camera_id,
                          camera_uvc_status_t *status) {
    if (!backend || camera_id < 0 || camera_id >= CAMERA_UVC_CAMERA_COUNT ||
        !status)
        return CAMERA_UVC_ERR_ARGUMENT;
    camera_uvc_channel &channel = backend->channels[camera_id];
    std::lock_guard<std::mutex> lock(channel.mutex);
    std::memset(status, 0, sizeof(*status));
    status->enabled = channel.enabled;
    status->host_streaming = channel.host_streaming;
    status->source_camera_id = channel.source_camera_id;
    status->last_error = channel.last_error;
    status->last_mpp_error = channel.last_mpp_error;
    status->width = backend->config.width;
    status->height = backend->config.height;
    status->fps = channel.source_fps;
    status->negotiated_width = channel.negotiated_width;
    status->negotiated_height = channel.negotiated_height;
    status->negotiated_fps = channel.negotiated_fps;
    status->negotiated_fcc = channel.negotiated_fcc;
    status->last_sequence = channel.last_sequence;
    status->queue_pending = static_cast<uint32_t>(channel.queue.size());
    status->frames_submitted = channel.frames_submitted;
    status->frames_encoded = channel.frames_encoded;
    status->frames_sent = channel.frames_sent;
    status->frames_skipped_no_host = channel.frames_skipped_no_host;
    status->frames_rate_limited = channel.frames_rate_limited;
    status->queue_drops = channel.queue_drops;
    status->encode_errors = channel.encode_errors;
    status->jpeg_bytes = channel.jpeg_bytes;
    return CAMERA_UVC_OK;
}

void camera_uvc_destroy(camera_uvc_backend_t *backend) {
    if (!backend)
        return;
    shutdown_backend(backend);
    delete backend;
}

const char *camera_uvc_strerror(int result) {
    switch (result) {
    case CAMERA_UVC_OK: return "ok";
    case CAMERA_UVC_ERR_ARGUMENT: return "invalid argument";
    case CAMERA_UVC_ERR_STATE: return "invalid state";
    case CAMERA_UVC_ERR_NODE: return "UVC video node unavailable";
    case CAMERA_UVC_ERR_MPP: return "MPP JPEG encoder error";
    case CAMERA_UVC_ERR_NOMEM: return "out of memory";
    case CAMERA_UVC_ERR_UNSUPPORTED: return "unsupported UVC mode";
    default: return "unknown UVC error";
    }
}
