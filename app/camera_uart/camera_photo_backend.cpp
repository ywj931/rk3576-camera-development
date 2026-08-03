#define MODULE_TAG "camera_photo"

#include "camera_photo_backend.h"
#include "photo_exif.h"

extern "C" {
#include "rk_mpi.h"
#include "mpp_buffer.h"
#include "mpp_frame.h"
#include "mpp_packet.h"
#include "rk_venc_cfg.h"
}

#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <new>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

struct PhotoWork {
    std::vector<uint8_t> nv12;
    camera_photo_metadata_t metadata{};
};

class MppJpegEncoder {
public:
    int initialize(uint32_t width, uint32_t height, uint32_t quality);
    int encode(const std::vector<uint8_t> &nv12, std::vector<uint8_t> *jpeg);
    void shutdown();
    ~MppJpegEncoder() { shutdown(); }

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

int MppJpegEncoder::initialize(uint32_t width, uint32_t height,
                               uint32_t quality)
{
    shutdown();
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
    mpp_enc_cfg_set_s32(config_, "rc:fps_in_num", 2);
    mpp_enc_cfg_set_s32(config_, "rc:fps_in_denom", 1);
    mpp_enc_cfg_set_s32(config_, "rc:fps_out_num", 2);
    mpp_enc_cfg_set_s32(config_, "rc:fps_out_denom", 1);
    mpp_enc_cfg_set_s32(config_, "codec:type", MPP_VIDEO_CodingMJPEG);
    mpp_enc_cfg_set_s32(config_, "jpeg:q_factor", quality);
    mpp_enc_cfg_set_s32(config_, "jpeg:qf_max", 99);
    mpp_enc_cfg_set_s32(config_, "jpeg:qf_min", 1);
    return mpi_->control(context_, MPP_ENC_SET_CFG, config_);
}

int MppJpegEncoder::encode(const std::vector<uint8_t> &nv12,
                           std::vector<uint8_t> *jpeg)
{
    if (!jpeg || nv12.size() != input_size_ || !mpi_)
        return MPP_ERR_VALUE;
    void *input = mpp_buffer_get_ptr(frame_buffer_);
    if (!input)
        return MPP_ERR_NOMEM;
    mpp_buffer_sync_begin(frame_buffer_);
    std::memset(input, 0, frame_size_);
    const uint8_t *src_y = nv12.data();
    uint8_t *dst_y = static_cast<uint8_t *>(input);
    for (uint32_t row = 0; row < height_; ++row)
        std::memcpy(dst_y + static_cast<size_t>(row) * hor_stride_,
                    src_y + static_cast<size_t>(row) * width_, width_);
    const uint8_t *src_uv = src_y + static_cast<size_t>(width_) * height_;
    uint8_t *dst_uv = dst_y + static_cast<size_t>(hor_stride_) * ver_stride_;
    for (uint32_t row = 0; row < height_ / 2; ++row)
        std::memcpy(dst_uv + static_cast<size_t>(row) * hor_stride_,
                    src_uv + static_cast<size_t>(row) * width_, width_);
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
    if (ret == MPP_OK) {
        mpp_packet_set_length(packet, 0);
        MppMeta meta = mpp_frame_get_meta(frame);
        mpp_meta_set_packet(meta, KEY_OUTPUT_PACKET, packet);
        ret = mpi_->encode_put_frame(context_, frame);
        if (ret == MPP_OK)
            ret = mpi_->encode_get_packet(context_, &packet);
    }
    mpp_frame_deinit(&frame);
    if (ret == MPP_OK && packet) {
        const uint8_t *data =
            static_cast<const uint8_t *>(mpp_packet_get_pos(packet));
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

void MppJpegEncoder::shutdown()
{
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

bool valid_camera_id(int camera_id)
{
    return camera_id >= 0 && camera_id < CAMERA_PHOTO_CAMERA_COUNT;
}

int make_directories(const std::string &path)
{
    if (path.empty() || path.size() >= CAMERA_PHOTO_PATH_MAX)
        return EINVAL;
    std::string current;
    size_t start = 0;
    if (path[0] == '/') {
        current = "/";
        start = 1;
    }
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const std::string part =
            path.substr(start, slash == std::string::npos
                                   ? std::string::npos
                                   : slash - start);
        if (!part.empty()) {
            if (current.size() > 1 && current.back() != '/')
                current.push_back('/');
            current += part;
            if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST)
                return errno;
        }
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return 0;
}

camera_photo::Metadata convert_metadata(const camera_photo_metadata_t &input)
{
    camera_photo::Metadata output;
    output.camera_id = input.camera_id;
    output.frame_id = input.frame_id;
    output.trigger_id = input.trigger_id;
    output.trigger_monotonic_ns = input.trigger_monotonic_ns;
    output.trigger_realtime_ns = input.trigger_realtime_ns;
    output.pps_id = input.pps_id;
    output.trigger_timer_tick = input.trigger_timer_tick;
    output.frame_monotonic_ns = input.frame_monotonic_ns;
    output.frame_realtime_ns = input.frame_realtime_ns;
    output.exposure_start_realtime_ns = input.exposure_start_realtime_ns;
    output.exposure_center_realtime_ns = input.exposure_center_realtime_ns;
    output.sensor_response_offset_ns = input.sensor_response_offset_ns;
    output.trigger_to_frame_ns = input.trigger_to_frame_ns;
    output.exposure_us = input.exposure_us;
    output.gain_x1000 = input.gain_x1000;
    output.iso = input.iso;
    output.utc_valid = input.utc_valid != 0;
    output.trigger_monotonic_is_uart_arrival =
        input.trigger_monotonic_is_uart_arrival != 0;
    output.iso_estimated = input.iso_estimated != 0;
    output.trigger_source = input.trigger_source;
    output.exposure_source = input.exposure_source;
    return output;
}

}  // namespace

struct camera_photo_backend;

struct CameraPhotoStream {
    camera_photo_backend *backend = nullptr;
    int camera_id = -1;
    std::mutex mutex;
    std::condition_variable condition;
    std::condition_variable idle;
    std::thread worker;
    bool stop_worker = false;
    bool enabled = false;
    bool processing = false;
    int last_error = CAMERA_PHOTO_OK;
    int last_mpp_error = MPP_OK;
    int last_errno = 0;
    int64_t response_offset_ns = 0;
    std::string output_dir;
    std::string metadata_csv;
    std::string last_photo;
    FILE *csv = nullptr;
    std::deque<PhotoWork> queue;
    MppJpegEncoder encoder;
    uint64_t frames_submitted = 0;
    uint64_t photos_saved = 0;
    uint64_t queue_drops = 0;
    uint64_t frames_without_trigger = 0;
    uint64_t invalid_metadata = 0;
    uint64_t encode_errors = 0;
    uint64_t exif_errors = 0;
    uint64_t write_errors = 0;
    uint64_t jpeg_bytes = 0;
    uint32_t last_frame_id = 0;
    uint64_t last_trigger_id = 0;
};

struct camera_photo_backend {
    camera_photo_config_t config{};
    CameraPhotoStream stream[CAMERA_PHOTO_CAMERA_COUNT];
};

namespace {

void write_csv_header(FILE *file)
{
    std::fputs(
        "camera_id,frame_id,trigger_id,trigger_source,trigger_monotonic_ns,"
        "trigger_realtime_ns,pps_id,trigger_timer_tick,utc_valid,"
        "trigger_monotonic_is_uart_arrival,frame_monotonic_ns,frame_realtime_ns,"
        "exposure_start_realtime_ns,exposure_center_realtime_ns,"
        "exposure_us,gain_x1000,iso,iso_estimated,response_offset_ns,"
        "trigger_to_frame_ns,exposure_source,jpeg_path\n",
        file);
    std::fflush(file);
}

void write_csv_record(FILE *file, const camera_photo_metadata_t &m,
                      const std::string &path)
{
    if (!file)
        return;
    std::fprintf(
        file,
        "%d,%u,%llu,%s,%llu,%llu,%llu,%llu,%d,%d,%llu,%llu,%llu,%llu,"
        "%u,%u,%u,%d,%lld,%lld,%s,%s\n",
        m.camera_id, m.frame_id,
        static_cast<unsigned long long>(m.trigger_id), m.trigger_source,
        static_cast<unsigned long long>(m.trigger_monotonic_ns),
        static_cast<unsigned long long>(m.trigger_realtime_ns),
        static_cast<unsigned long long>(m.pps_id),
        static_cast<unsigned long long>(m.trigger_timer_tick), m.utc_valid,
        m.trigger_monotonic_is_uart_arrival,
        static_cast<unsigned long long>(m.frame_monotonic_ns),
        static_cast<unsigned long long>(m.frame_realtime_ns),
        static_cast<unsigned long long>(m.exposure_start_realtime_ns),
        static_cast<unsigned long long>(m.exposure_center_realtime_ns),
        m.exposure_us, m.gain_x1000, m.iso, m.iso_estimated,
        static_cast<long long>(m.sensor_response_offset_ns),
        static_cast<long long>(m.trigger_to_frame_ns), m.exposure_source,
        path.c_str());
    std::fflush(file);
}

int write_atomic(const std::string &path, const std::vector<uint8_t> &bytes)
{
    const std::string temporary = path + ".tmp";
    FILE *file = std::fopen(temporary.c_str(), "wb");
    if (!file)
        return errno;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) ==
                        bytes.size() &&
                    std::fflush(file) == 0 && fsync(fileno(file)) == 0;
    int saved_errno = ok ? 0 : (errno ? errno : EIO);
    if (std::fclose(file) != 0 && !saved_errno)
        saved_errno = errno;
    if (!saved_errno && rename(temporary.c_str(), path.c_str()) != 0)
        saved_errno = errno;
    if (saved_errno)
        unlink(temporary.c_str());
    return saved_errno;
}

void photo_worker(CameraPhotoStream *stream)
{
    for (;;) {
        PhotoWork work;
        {
            std::unique_lock<std::mutex> lock(stream->mutex);
            stream->condition.wait(lock, [stream] {
                return stream->stop_worker || !stream->queue.empty();
            });
            if (stream->stop_worker && stream->queue.empty())
                break;
            work = std::move(stream->queue.front());
            stream->queue.pop_front();
            stream->processing = true;
        }

        std::vector<uint8_t> jpeg;
        int result = stream->encoder.encode(work.nv12, &jpeg);
        if (result != MPP_OK) {
            std::lock_guard<std::mutex> lock(stream->mutex);
            stream->encode_errors++;
            stream->last_mpp_error = result;
            stream->last_error = CAMERA_PHOTO_ERR_MPP;
        } else {
            std::vector<uint8_t> with_exif;
            std::string exif_error;
            const int exif_result = camera_photo::insert_exif(
                jpeg, convert_metadata(work.metadata), &with_exif,
                &exif_error);
            if (exif_result != camera_photo::EXIF_OK) {
                std::lock_guard<std::mutex> lock(stream->mutex);
                stream->exif_errors++;
                stream->last_error = CAMERA_PHOTO_ERR_EXIF;
            } else {
                char name[256];
                std::snprintf(
                    name, sizeof(name),
                    "/cam%d_trigger_%010llu_frame_%010u_%llu.jpg",
                    stream->camera_id,
                    static_cast<unsigned long long>(work.metadata.trigger_id),
                    work.metadata.frame_id,
                    static_cast<unsigned long long>(
                        work.metadata.exposure_start_realtime_ns));
                const std::string path = stream->output_dir + name;
                const int write_error = write_atomic(path, with_exif);
                std::lock_guard<std::mutex> lock(stream->mutex);
                if (write_error) {
                    stream->write_errors++;
                    stream->last_errno = write_error;
                    stream->last_error = CAMERA_PHOTO_ERR_IO;
                } else {
                    stream->photos_saved++;
                    stream->jpeg_bytes += with_exif.size();
                    stream->last_frame_id = work.metadata.frame_id;
                    stream->last_trigger_id = work.metadata.trigger_id;
                    stream->last_photo = path;
                    stream->last_error = CAMERA_PHOTO_OK;
                    write_csv_record(stream->csv, work.metadata, path);
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(stream->mutex);
            stream->processing = false;
            if (stream->queue.empty())
                stream->idle.notify_all();
        }
    }
}

}  // namespace

extern "C" void camera_photo_default_config(camera_photo_config_t *config)
{
    if (!config)
        return;
    std::memset(config, 0, sizeof(*config));
    config->width = 4000;
    config->height = 3000;
    config->jpeg_quality = 90;
    config->queue_depth = 2;
}

extern "C" int camera_photo_create(const camera_photo_config_t *config,
                                    camera_photo_backend_t **backend_out)
{
    if (!backend_out)
        return CAMERA_PHOTO_ERR_ARGUMENT;
    *backend_out = nullptr;
    camera_photo_config_t selected{};
    camera_photo_default_config(&selected);
    if (config)
        selected = *config;
    if (!selected.width || !selected.height || !selected.queue_depth ||
        selected.jpeg_quality < 1 || selected.jpeg_quality > 99)
        return CAMERA_PHOTO_ERR_ARGUMENT;
    camera_photo_backend_t *backend =
        new (std::nothrow) camera_photo_backend;
    if (!backend)
        return CAMERA_PHOTO_ERR_IO;
    backend->config = selected;
    for (int camera_id = 0; camera_id < CAMERA_PHOTO_CAMERA_COUNT;
         ++camera_id) {
        CameraPhotoStream &stream = backend->stream[camera_id];
        stream.backend = backend;
        stream.camera_id = camera_id;
        stream.worker = std::thread(photo_worker, &stream);
    }
    *backend_out = backend;
    return CAMERA_PHOTO_OK;
}

extern "C" void camera_photo_destroy(camera_photo_backend_t *backend)
{
    if (!backend)
        return;
    for (int camera_id = 0; camera_id < CAMERA_PHOTO_CAMERA_COUNT; ++camera_id)
        camera_photo_stop(backend, camera_id);
    for (CameraPhotoStream &stream : backend->stream) {
        {
            std::lock_guard<std::mutex> lock(stream.mutex);
            stream.stop_worker = true;
            stream.condition.notify_all();
        }
        if (stream.worker.joinable())
            stream.worker.join();
    }
    delete backend;
}

extern "C" int camera_photo_start(camera_photo_backend_t *backend,
                                   int camera_id, const char *output_dir)
{
    if (!backend || !valid_camera_id(camera_id) || !output_dir ||
        !*output_dir)
        return CAMERA_PHOTO_ERR_ARGUMENT;
    CameraPhotoStream &stream = backend->stream[camera_id];
    std::lock_guard<std::mutex> lock(stream.mutex);
    if (stream.enabled)
        return CAMERA_PHOTO_ERR_ALREADY_RUNNING;
    const int directory_error = make_directories(output_dir);
    if (directory_error) {
        stream.last_errno = directory_error;
        return CAMERA_PHOTO_ERR_IO;
    }
    const int mpp_result = stream.encoder.initialize(
        backend->config.width, backend->config.height,
        backend->config.jpeg_quality);
    if (mpp_result != MPP_OK) {
        stream.last_mpp_error = mpp_result;
        stream.last_error = CAMERA_PHOTO_ERR_MPP;
        stream.encoder.shutdown();
        return CAMERA_PHOTO_ERR_MPP;
    }
    stream.output_dir = output_dir;
    while (stream.output_dir.size() > 1 && stream.output_dir.back() == '/')
        stream.output_dir.pop_back();
    stream.metadata_csv = stream.output_dir + "/stage6_metadata.csv";
    const bool new_file = access(stream.metadata_csv.c_str(), F_OK) != 0;
    stream.csv = std::fopen(stream.metadata_csv.c_str(), "a");
    if (!stream.csv) {
        stream.last_errno = errno;
        stream.encoder.shutdown();
        return CAMERA_PHOTO_ERR_IO;
    }
    if (new_file)
        write_csv_header(stream.csv);
    stream.enabled = true;
    stream.last_error = CAMERA_PHOTO_OK;
    return CAMERA_PHOTO_OK;
}

extern "C" int camera_photo_stop(camera_photo_backend_t *backend,
                                  int camera_id)
{
    if (!backend || !valid_camera_id(camera_id))
        return CAMERA_PHOTO_ERR_ARGUMENT;
    CameraPhotoStream &stream = backend->stream[camera_id];
    std::unique_lock<std::mutex> lock(stream.mutex);
    if (!stream.enabled) {
        if (stream.csv) {
            std::fclose(stream.csv);
            stream.csv = nullptr;
        }
        return CAMERA_PHOTO_ERR_NOT_RUNNING;
    }
    stream.enabled = false;
    stream.idle.wait(lock, [&stream] {
        return stream.queue.empty() && !stream.processing;
    });
    if (stream.csv) {
        std::fclose(stream.csv);
        stream.csv = nullptr;
    }
    stream.encoder.shutdown();
    return CAMERA_PHOTO_OK;
}

extern "C" int camera_photo_is_enabled(camera_photo_backend_t *backend,
                                        int camera_id)
{
    if (!backend || !valid_camera_id(camera_id))
        return 0;
    CameraPhotoStream &stream = backend->stream[camera_id];
    std::lock_guard<std::mutex> lock(stream.mutex);
    return stream.enabled ? 1 : 0;
}

extern "C" int camera_photo_set_response_offset(
    camera_photo_backend_t *backend, int camera_id, int64_t offset_ns)
{
    if (!backend || !valid_camera_id(camera_id))
        return CAMERA_PHOTO_ERR_ARGUMENT;
    CameraPhotoStream &stream = backend->stream[camera_id];
    std::lock_guard<std::mutex> lock(stream.mutex);
    stream.response_offset_ns = offset_ns;
    return CAMERA_PHOTO_OK;
}

extern "C" int camera_photo_get_response_offset(
    camera_photo_backend_t *backend, int camera_id, int64_t *offset_ns)
{
    if (!backend || !valid_camera_id(camera_id) || !offset_ns)
        return CAMERA_PHOTO_ERR_ARGUMENT;
    CameraPhotoStream &stream = backend->stream[camera_id];
    std::lock_guard<std::mutex> lock(stream.mutex);
    *offset_ns = stream.response_offset_ns;
    return CAMERA_PHOTO_OK;
}

extern "C" int camera_photo_note_unbound_frame(
    camera_photo_backend_t *backend, int camera_id)
{
    if (!backend || !valid_camera_id(camera_id))
        return CAMERA_PHOTO_ERR_ARGUMENT;
    CameraPhotoStream &stream = backend->stream[camera_id];
    std::lock_guard<std::mutex> lock(stream.mutex);
    if (stream.enabled)
        stream.frames_without_trigger++;
    return CAMERA_PHOTO_OK;
}

extern "C" int camera_photo_submit_nv12(
    camera_photo_backend_t *backend, int camera_id, const void *plane0,
    size_t plane0_size, const void *plane1, size_t plane1_size,
    const camera_photo_metadata_t *metadata)
{
    if (!backend || !valid_camera_id(camera_id) || !plane0 || !plane1 ||
        !metadata || metadata->camera_id != camera_id)
        return CAMERA_PHOTO_ERR_ARGUMENT;
    const size_t y_size =
        static_cast<size_t>(backend->config.width) * backend->config.height;
    const size_t uv_size = y_size / 2;
    if (plane0_size < y_size || plane1_size < uv_size)
        return CAMERA_PHOTO_ERR_RANGE;
    CameraPhotoStream &stream = backend->stream[camera_id];
    std::lock_guard<std::mutex> lock(stream.mutex);
    if (!stream.enabled)
        return CAMERA_PHOTO_ERR_NOT_RUNNING;
    if (!metadata->trigger_id || !metadata->trigger_realtime_ns ||
        !metadata->exposure_us || !metadata->iso) {
        stream.invalid_metadata++;
        return CAMERA_PHOTO_ERR_ARGUMENT;
    }
    PhotoWork work;
    work.nv12.resize(y_size + uv_size);
    std::memcpy(work.nv12.data(), plane0, y_size);
    std::memcpy(work.nv12.data() + y_size, plane1, uv_size);
    work.metadata = *metadata;
    if (stream.queue.size() >= backend->config.queue_depth) {
        stream.queue.pop_front();
        stream.queue_drops++;
    }
    stream.queue.push_back(std::move(work));
    stream.frames_submitted++;
    stream.condition.notify_one();
    return CAMERA_PHOTO_OK;
}

extern "C" int camera_photo_get_status(camera_photo_backend_t *backend,
                                        int camera_id,
                                        camera_photo_status_t *status)
{
    if (!backend || !valid_camera_id(camera_id) || !status)
        return CAMERA_PHOTO_ERR_ARGUMENT;
    CameraPhotoStream &stream = backend->stream[camera_id];
    std::lock_guard<std::mutex> lock(stream.mutex);
    std::memset(status, 0, sizeof(*status));
    status->camera_id = camera_id;
    status->enabled = stream.enabled;
    status->processing = stream.processing;
    status->last_error = stream.last_error;
    status->last_mpp_error = stream.last_mpp_error;
    status->last_errno = stream.last_errno;
    status->width = backend->config.width;
    status->height = backend->config.height;
    status->jpeg_quality = backend->config.jpeg_quality;
    status->queue_pending = stream.queue.size();
    status->sensor_response_offset_ns = stream.response_offset_ns;
    status->frames_submitted = stream.frames_submitted;
    status->photos_saved = stream.photos_saved;
    status->queue_drops = stream.queue_drops;
    status->frames_without_trigger = stream.frames_without_trigger;
    status->invalid_metadata = stream.invalid_metadata;
    status->encode_errors = stream.encode_errors;
    status->exif_errors = stream.exif_errors;
    status->write_errors = stream.write_errors;
    status->jpeg_bytes = stream.jpeg_bytes;
    status->last_frame_id = stream.last_frame_id;
    status->last_trigger_id = stream.last_trigger_id;
    std::snprintf(status->output_dir, sizeof(status->output_dir), "%s",
                  stream.output_dir.c_str());
    std::snprintf(status->metadata_csv, sizeof(status->metadata_csv), "%s",
                  stream.metadata_csv.c_str());
    std::snprintf(status->last_photo, sizeof(status->last_photo), "%s",
                  stream.last_photo.c_str());
    return CAMERA_PHOTO_OK;
}

extern "C" const char *camera_photo_strerror(int result)
{
    switch (result) {
    case CAMERA_PHOTO_OK:
        return "success";
    case CAMERA_PHOTO_ERR_ARGUMENT:
        return "invalid photo metadata or argument";
    case CAMERA_PHOTO_ERR_RANGE:
        return "NV12 plane is smaller than configured image";
    case CAMERA_PHOTO_ERR_IO:
        return "unable to create or write photo output";
    case CAMERA_PHOTO_ERR_ALREADY_RUNNING:
        return "photo output is already running";
    case CAMERA_PHOTO_ERR_NOT_RUNNING:
        return "photo output is not running";
    case CAMERA_PHOTO_ERR_MPP:
        return "MPP JPEG encoder failed";
    case CAMERA_PHOTO_ERR_EXIF:
        return "unable to insert JPEG EXIF";
    default:
        return "unknown photo output error";
    }
}
