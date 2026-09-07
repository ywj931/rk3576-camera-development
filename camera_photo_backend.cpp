#define MODULE_TAG "camera_photo"

#include "camera_photo_backend.h"
#include "nv12_vertical_stitch.h"
#include "photo_exif.h"
#include "stitch_transfer_worker.h"

extern "C" {
#include "rk_mpi.h"
#include "mpp_buffer.h"
#include "mpp_frame.h"
#include "mpp_packet.h"
#include "rk_venc_cfg.h"
}

#include <algorithm>
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

struct StitchFrame {
    std::vector<uint8_t> nv12;
    camera_photo_metadata_t metadata{};
};

struct StitchWork {
    std::vector<uint8_t> nv12;
    camera_photo_metadata_t top{};
    camera_photo_metadata_t bottom{};
    uint64_t pair_id = 0;
    int64_t frame_delta_ns = 0;
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
    mpp_enc_cfg_set_s32(config_, "rc:fps_in_num", 4);
    mpp_enc_cfg_set_s32(config_, "rc:fps_in_denom", 1);
    mpp_enc_cfg_set_s32(config_, "rc:fps_out_num", 4);
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

bool valid_metadata(const camera_photo_metadata_t &metadata)
{
    return metadata.frame_monotonic_ns && metadata.frame_realtime_ns &&
           metadata.exposure_start_realtime_ns && metadata.exposure_us &&
           metadata.iso &&
           (!metadata.trigger_id || metadata.trigger_realtime_ns);
}

uint64_t timestamp_distance(uint64_t left, uint64_t right)
{
    return left > right ? left - right : right - left;
}

int64_t signed_timestamp_delta(uint64_t bottom, uint64_t top)
{
    if (bottom >= top) {
        const uint64_t delta = bottom - top;
        return delta > static_cast<uint64_t>(INT64_MAX)
                   ? INT64_MAX
                   : static_cast<int64_t>(delta);
    }
    const uint64_t delta = top - bottom;
    return delta > static_cast<uint64_t>(INT64_MAX)
               ? INT64_MIN
               : -static_cast<int64_t>(delta);
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
    output.white_balance_valid = input.white_balance_valid != 0;
    output.white_balance_auto = input.white_balance_auto != 0;
    output.white_balance_converged = input.white_balance_converged != 0;
    output.white_balance_cct = input.white_balance_cct;
    output.wb_r_gain_x1000 = input.wb_r_gain_x1000;
    output.wb_gr_gain_x1000 = input.wb_gr_gain_x1000;
    output.wb_gb_gain_x1000 = input.wb_gb_gain_x1000;
    output.wb_b_gain_x1000 = input.wb_b_gain_x1000;
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

struct CameraPhotoStitch {
    camera_photo_backend *backend = nullptr;
    std::mutex mutex;
    std::condition_variable condition;
    std::condition_variable idle;
    std::thread worker;
    bool stop_worker = false;
    bool enabled = false;
    bool stopping = false;
    bool processing = false;
    uint32_t assembling = 0;
    uint64_t generation = 0;
    int last_error = CAMERA_PHOTO_OK;
    int last_mpp_error = MPP_OK;
    int last_errno = 0;
    std::string output_dir;
    std::string metadata_csv;
    std::string last_photo;
    FILE *csv = nullptr;
    std::deque<StitchFrame> pending[CAMERA_PHOTO_CAMERA_COUNT];
    std::deque<StitchWork> queue;
    MppJpegEncoder encoder;
    uint64_t frames_submitted[CAMERA_PHOTO_CAMERA_COUNT] = {};
    uint64_t pairs_matched = 0;
    uint64_t photos_saved = 0;
    uint64_t unmatched_drops[CAMERA_PHOTO_CAMERA_COUNT] = {};
    uint64_t queue_drops = 0;
    uint64_t invalid_metadata = 0;
    uint64_t encode_errors = 0;
    uint64_t exif_errors = 0;
    uint64_t write_errors = 0;
    uint64_t jpeg_bytes = 0;
    uint64_t last_pair_id = 0;
    uint32_t last_top_frame_id = 0;
    uint32_t last_bottom_frame_id = 0;
    int64_t last_frame_delta_ns = 0;
    uint64_t last_composite_capture_realtime_ns = 0;
    uint64_t last_top_frame_realtime_ns = 0;
    uint64_t last_bottom_frame_realtime_ns = 0;
    uint32_t last_top_exposure_us = 0;
    uint32_t last_bottom_exposure_us = 0;
    uint32_t last_top_iso = 0;
    uint32_t last_bottom_iso = 0;
    int last_composite_utc_valid = 0;
};

struct camera_photo_backend {
    camera_photo_config_t config{};
    CameraPhotoStream stream[CAMERA_PHOTO_CAMERA_COUNT];
    std::mutex stitch_lifecycle_mutex;
    CameraPhotoStitch stitch;
    camera_stitch_transfer::worker stitch_transfer;
};

static int camera_photo_stitch_stop_internal(camera_photo_backend_t *backend,
                                             bool transfer_mode);

namespace {

bool write_csv_header(FILE *file)
{
    const int result = std::fputs(
        "camera_id,frame_id,trigger_id,trigger_source,trigger_monotonic_ns,"
        "trigger_realtime_ns,pps_id,trigger_timer_tick,utc_valid,"
        "trigger_monotonic_is_uart_arrival,frame_monotonic_ns,frame_realtime_ns,"
        "exposure_start_realtime_ns,exposure_center_realtime_ns,"
        "exposure_us,gain_x1000,iso,white_balance_valid,white_balance_auto,"
        "white_balance_converged,white_balance_cct,wb_r_gain_x1000,"
        "wb_gr_gain_x1000,wb_gb_gain_x1000,wb_b_gain_x1000,iso_estimated,"
        "response_offset_ns,"
        "trigger_to_frame_ns,exposure_source,jpeg_path\n",
        file);
    return result >= 0 && std::fflush(file) == 0;
}

bool write_csv_record(FILE *file, const camera_photo_metadata_t &m,
                      const std::string &path)
{
    if (!file)
        return false;
    const int result = std::fprintf(
        file,
        "%d,%u,%llu,%s,%llu,%llu,%llu,%llu,%d,%d,%llu,%llu,%llu,%llu,"
        "%u,%u,%u,%d,%d,%d,%u,%u,%u,%u,%u,%d,%lld,%lld,%s,%s\n",
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
        m.exposure_us, m.gain_x1000, m.iso, m.white_balance_valid,
        m.white_balance_auto, m.white_balance_converged,
        m.white_balance_cct, m.wb_r_gain_x1000, m.wb_gr_gain_x1000,
        m.wb_gb_gain_x1000, m.wb_b_gain_x1000, m.iso_estimated,
        static_cast<long long>(m.sensor_response_offset_ns),
        static_cast<long long>(m.trigger_to_frame_ns), m.exposure_source,
        path.c_str());
    return result >= 0 && std::fflush(file) == 0;
}

bool write_stitch_csv_header(FILE *file)
{
    const int result = std::fputs(
        "pair_id,trigger_id,top_camera_id,top_frame_id,bottom_camera_id,"
        "bottom_frame_id,bottom_minus_top_frame_ns,top_frame_monotonic_ns,"
        "bottom_frame_monotonic_ns,top_exposure_start_realtime_ns,"
        "bottom_exposure_start_realtime_ns,top_exposure_us,bottom_exposure_us,"
        "top_iso,bottom_iso,top_white_balance_cct,bottom_white_balance_cct,"
        "top_wb_r_gain_x1000,top_wb_gr_gain_x1000,top_wb_gb_gain_x1000,"
        "top_wb_b_gain_x1000,bottom_wb_r_gain_x1000,"
        "bottom_wb_gr_gain_x1000,bottom_wb_gb_gain_x1000,"
        "bottom_wb_b_gain_x1000,jpeg_path,"
        "composite_capture_realtime_ns,composite_utc_valid,"
        "top_frame_realtime_ns,bottom_frame_realtime_ns,"
        "top_exposure_center_realtime_ns,"
        "bottom_exposure_center_realtime_ns,top_utc_valid,bottom_utc_valid,"
        "top_gain_x1000,bottom_gain_x1000,top_iso_estimated,"
        "bottom_iso_estimated,top_exposure_source,bottom_exposure_source\n",
        file);
    return result >= 0 && std::fflush(file) == 0;
}

bool write_stitch_csv_record(FILE *file, const StitchWork &work,
                             const std::string &path)
{
    if (!file)
        return false;
    const camera_photo_metadata_t &top = work.top;
    const camera_photo_metadata_t &bottom = work.bottom;
    const uint64_t trigger_id =
        top.trigger_id && top.trigger_id == bottom.trigger_id
            ? top.trigger_id
            : 0;
    const uint64_t composite_capture_realtime_ns =
        std::min(top.exposure_start_realtime_ns,
                 bottom.exposure_start_realtime_ns);
    const int composite_utc_valid = top.utc_valid && bottom.utc_valid;
    const int result = std::fprintf(
        file,
        "%llu,%llu,%d,%u,%d,%u,%lld,%llu,%llu,%llu,%llu,%u,%u,%u,%u,"
        "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%s,%llu,%d,%llu,%llu,%llu,%llu,"
        "%d,%d,%u,%u,%d,%d,%s,%s\n",
        static_cast<unsigned long long>(work.pair_id),
        static_cast<unsigned long long>(trigger_id), top.camera_id,
        top.frame_id, bottom.camera_id, bottom.frame_id,
        static_cast<long long>(work.frame_delta_ns),
        static_cast<unsigned long long>(top.frame_monotonic_ns),
        static_cast<unsigned long long>(bottom.frame_monotonic_ns),
        static_cast<unsigned long long>(top.exposure_start_realtime_ns),
        static_cast<unsigned long long>(bottom.exposure_start_realtime_ns),
        top.exposure_us, bottom.exposure_us, top.iso, bottom.iso,
        top.white_balance_cct, bottom.white_balance_cct,
        top.wb_r_gain_x1000, top.wb_gr_gain_x1000, top.wb_gb_gain_x1000,
        top.wb_b_gain_x1000, bottom.wb_r_gain_x1000,
        bottom.wb_gr_gain_x1000, bottom.wb_gb_gain_x1000,
        bottom.wb_b_gain_x1000, path.c_str(),
        static_cast<unsigned long long>(composite_capture_realtime_ns),
        composite_utc_valid,
        static_cast<unsigned long long>(top.frame_realtime_ns),
        static_cast<unsigned long long>(bottom.frame_realtime_ns),
        static_cast<unsigned long long>(top.exposure_center_realtime_ns),
        static_cast<unsigned long long>(bottom.exposure_center_realtime_ns),
        top.utc_valid, bottom.utc_valid, top.gain_x1000, bottom.gain_x1000,
        top.iso_estimated, bottom.iso_estimated, top.exposure_source,
        bottom.exposure_source);
    return result >= 0 && std::fflush(file) == 0 &&
           fsync(fileno(file)) == 0;
}

int sync_parent_directory(const std::string &path)
{
    const size_t separator = path.rfind('/');
    const std::string directory =
        separator == std::string::npos
            ? "."
            : (separator == 0 ? "/" : path.substr(0, separator));
    const int fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return errno ? errno : EIO;
    int result = fsync(fd) == 0 ? 0 : (errno ? errno : EIO);
    if (close(fd) != 0 && !result)
        result = errno ? errno : EIO;
    return result;
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
    if (!saved_errno)
        saved_errno = sync_parent_directory(path);
    if (saved_errno)
        unlink(temporary.c_str());
    return saved_errno;
}

void stitch_worker(CameraPhotoStitch *stitch)
{
    for (;;) {
        StitchWork work;
        {
            std::unique_lock<std::mutex> lock(stitch->mutex);
            stitch->condition.wait(lock, [stitch] {
                return stitch->stop_worker || !stitch->queue.empty();
            });
            if (stitch->stop_worker && stitch->queue.empty())
                break;
            work = std::move(stitch->queue.front());
            stitch->queue.pop_front();
            stitch->processing = true;
        }

        std::vector<uint8_t> jpeg;
        int result = stitch->encoder.encode(work.nv12, &jpeg);
        if (result != MPP_OK) {
            std::lock_guard<std::mutex> lock(stitch->mutex);
            stitch->encode_errors++;
            stitch->last_mpp_error = result;
            stitch->last_error = CAMERA_PHOTO_ERR_MPP;
        } else {
            std::vector<uint8_t> with_exif;
            std::string exif_error;
            const int exif_result = camera_photo::insert_composite_exif(
                jpeg, convert_metadata(work.top), convert_metadata(work.bottom),
                work.pair_id, work.frame_delta_ns, &with_exif, &exif_error);
            if (exif_result != camera_photo::EXIF_OK) {
                std::lock_guard<std::mutex> lock(stitch->mutex);
                stitch->exif_errors++;
                stitch->last_error = CAMERA_PHOTO_ERR_EXIF;
            } else {
                /* File name = unix-ns of the earlier exposure start. */
                char name[256];
                std::snprintf(
                    name, sizeof(name), "/%llu.jpg",
                    static_cast<unsigned long long>(
                        std::min(work.top.exposure_start_realtime_ns,
                                 work.bottom.exposure_start_realtime_ns)));
                const std::string path = stitch->output_dir + name;
                int write_error = write_atomic(path, with_exif);
                if (!write_error &&
                    !write_stitch_csv_record(stitch->csv, work, path)) {
                    write_error = errno ? errno : EIO;
                    unlink(path.c_str());
                    sync_parent_directory(path);
                }
                {
                    std::lock_guard<std::mutex> lock(stitch->mutex);
                    if (write_error) {
                        stitch->write_errors++;
                        stitch->last_errno = write_error;
                        stitch->last_error = CAMERA_PHOTO_ERR_IO;
                    } else {
                        stitch->photos_saved++;
                        stitch->jpeg_bytes += with_exif.size();
                        stitch->last_pair_id = work.pair_id;
                        stitch->last_top_frame_id = work.top.frame_id;
                        stitch->last_bottom_frame_id = work.bottom.frame_id;
                        stitch->last_frame_delta_ns = work.frame_delta_ns;
                        stitch->last_composite_capture_realtime_ns =
                            std::min(work.top.exposure_start_realtime_ns,
                                     work.bottom.exposure_start_realtime_ns);
                        stitch->last_top_frame_realtime_ns =
                            work.top.frame_realtime_ns;
                        stitch->last_bottom_frame_realtime_ns =
                            work.bottom.frame_realtime_ns;
                        stitch->last_top_exposure_us = work.top.exposure_us;
                        stitch->last_bottom_exposure_us =
                            work.bottom.exposure_us;
                        stitch->last_top_iso = work.top.iso;
                        stitch->last_bottom_iso = work.bottom.iso;
                        stitch->last_composite_utc_valid =
                            work.top.utc_valid && work.bottom.utc_valid;
                        stitch->last_photo = path;
                    }
                }
                if (!write_error) {
                    camera_stitch_transfer::item transfer_work;
                    transfer_work.path = path;
                    transfer_work.pair_id = work.pair_id;
                    transfer_work.exposure_start_utc_ns =
                        std::min(work.top.exposure_start_realtime_ns,
                                 work.bottom.exposure_start_realtime_ns);
                    transfer_work.exposure_duration_ns =
                        static_cast<uint64_t>(work.top.exposure_us) * 1000ULL;
                    transfer_work.payload_size = with_exif.size();
                    stitch->backend->stitch_transfer.enqueue(
                        std::move(transfer_work));
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(stitch->mutex);
            stitch->processing = false;
            if (stitch->queue.empty() && stitch->assembling == 0)
                stitch->idle.notify_all();
        }
    }
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
                int write_error = write_atomic(path, with_exif);
                if (!write_error &&
                    !write_csv_record(stream->csv, work.metadata, path)) {
                    write_error = errno ? errno : EIO;
                    unlink(path.c_str());
                }
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
    config->camera_count = CAMERA_PHOTO_CAMERA_COUNT;
    config->jpeg_quality = 90;
    config->queue_depth = 2;
    config->stitch_pending_depth = 4;
    config->stitch_pair_tolerance_ns = 125000000ULL;
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
    if (!selected.width || !selected.height || !selected.camera_count ||
        selected.camera_count > CAMERA_PHOTO_CAMERA_COUNT ||
        !selected.queue_depth ||
        !selected.stitch_pending_depth ||
        !selected.stitch_pair_tolerance_ns ||
        selected.jpeg_quality < 1 || selected.jpeg_quality > 99)
        return CAMERA_PHOTO_ERR_ARGUMENT;
    camera_photo_backend_t *backend =
        new (std::nothrow) camera_photo_backend;
    if (!backend)
        return CAMERA_PHOTO_ERR_IO;
    backend->config = selected;
    for (int camera_id = 0;
         camera_id < static_cast<int>(selected.camera_count);
         ++camera_id) {
        CameraPhotoStream &stream = backend->stream[camera_id];
        stream.backend = backend;
        stream.camera_id = camera_id;
        stream.worker = std::thread(photo_worker, &stream);
    }
    backend->stitch.backend = backend;
    backend->stitch.worker = std::thread(stitch_worker, &backend->stitch);
    *backend_out = backend;
    return CAMERA_PHOTO_OK;
}

extern "C" void camera_photo_destroy(camera_photo_backend_t *backend)
{
    if (!backend)
        return;
    {
        std::lock_guard<std::mutex> lifecycle(
            backend->stitch_lifecycle_mutex);
        camera_photo_stitch_stop_internal(backend, true);
        if (backend->stitch_transfer.get_status().enabled)
            backend->stitch_transfer.stop();
    }
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
    {
        std::lock_guard<std::mutex> lock(backend->stitch.mutex);
        backend->stitch.stop_worker = true;
        backend->stitch.condition.notify_all();
    }
    if (backend->stitch.worker.joinable())
        backend->stitch.worker.join();
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
    if (new_file && !write_csv_header(stream.csv)) {
        stream.last_errno = errno ? errno : EIO;
        stream.last_error = CAMERA_PHOTO_ERR_IO;
        std::fclose(stream.csv);
        stream.csv = nullptr;
        unlink(stream.metadata_csv.c_str());
        stream.encoder.shutdown();
        return CAMERA_PHOTO_ERR_IO;
    }
    stream.enabled = true;
    stream.last_error = CAMERA_PHOTO_OK;
    stream.last_mpp_error = MPP_OK;
    stream.last_errno = 0;
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
    int session_result = stream.last_error;
    if (stream.csv) {
        if ((std::fflush(stream.csv) != 0 || fsync(fileno(stream.csv)) != 0) &&
            session_result == CAMERA_PHOTO_OK) {
            stream.last_errno = errno ? errno : EIO;
            stream.last_error = CAMERA_PHOTO_ERR_IO;
            session_result = CAMERA_PHOTO_ERR_IO;
        }
        if (std::fclose(stream.csv) != 0 &&
            session_result == CAMERA_PHOTO_OK) {
            stream.last_errno = errno ? errno : EIO;
            stream.last_error = CAMERA_PHOTO_ERR_IO;
            session_result = CAMERA_PHOTO_ERR_IO;
        }
        stream.csv = nullptr;
    }
    stream.encoder.shutdown();
    return session_result;
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
    if (!metadata->frame_realtime_ns ||
        !metadata->exposure_start_realtime_ns || !metadata->exposure_us ||
        !metadata->iso ||
        (metadata->trigger_id && !metadata->trigger_realtime_ns)) {
        stream.invalid_metadata++;
        stream.last_error = CAMERA_PHOTO_ERR_ARGUMENT;
        return CAMERA_PHOTO_ERR_ARGUMENT;
    }
    try {
        PhotoWork work;
        work.nv12.resize(y_size + uv_size);
        std::memcpy(work.nv12.data(), plane0, y_size);
        std::memcpy(work.nv12.data() + y_size, plane1, uv_size);
        work.metadata = *metadata;
        if (stream.queue.size() >= backend->config.queue_depth) {
            stream.queue.pop_front();
            stream.queue_drops++;
            stream.last_error = CAMERA_PHOTO_ERR_QUEUE_FULL;
        }
        stream.queue.push_back(std::move(work));
    } catch (const std::bad_alloc &) {
        stream.queue_drops++;
        stream.last_errno = ENOMEM;
        stream.last_error = CAMERA_PHOTO_ERR_IO;
        return CAMERA_PHOTO_ERR_IO;
    }
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

static int camera_photo_stitch_start_internal(camera_photo_backend_t *backend,
                                              const char *output_dir,
                                              bool transfer_mode)
{
    if (!backend || !output_dir || !*output_dir ||
        backend->config.camera_count != CAMERA_PHOTO_CAMERA_COUNT ||
        backend->config.height > UINT32_MAX / 2U)
        return CAMERA_PHOTO_ERR_ARGUMENT;
    if (!transfer_mode && backend->stitch_transfer.get_status().enabled)
        return CAMERA_PHOTO_ERR_ALREADY_RUNNING;
    CameraPhotoStitch &stitch = backend->stitch;
    std::lock_guard<std::mutex> lock(stitch.mutex);
    if (stitch.enabled || stitch.stopping || stitch.processing ||
        stitch.assembling || !stitch.queue.empty() || stitch.csv)
        return CAMERA_PHOTO_ERR_ALREADY_RUNNING;
    const int directory_error = make_directories(output_dir);
    if (directory_error) {
        stitch.last_errno = directory_error;
        return CAMERA_PHOTO_ERR_IO;
    }
    const int mpp_result = stitch.encoder.initialize(
        backend->config.width, backend->config.height * 2U,
        backend->config.jpeg_quality);
    if (mpp_result != MPP_OK) {
        stitch.last_mpp_error = mpp_result;
        stitch.last_error = CAMERA_PHOTO_ERR_MPP;
        stitch.encoder.shutdown();
        return CAMERA_PHOTO_ERR_MPP;
    }
    stitch.output_dir = output_dir;
    while (stitch.output_dir.size() > 1 &&
           stitch.output_dir.back() == '/') {
        stitch.output_dir.pop_back();
    }
    stitch.metadata_csv = stitch.output_dir + "/stitch_metadata_v2.csv";
    const bool new_file = access(stitch.metadata_csv.c_str(), F_OK) != 0;
    stitch.csv = std::fopen(stitch.metadata_csv.c_str(), "a");
    if (!stitch.csv) {
        stitch.last_errno = errno;
        stitch.encoder.shutdown();
        return CAMERA_PHOTO_ERR_IO;
    }
    if (new_file && !write_stitch_csv_header(stitch.csv)) {
        stitch.last_errno = errno ? errno : EIO;
        stitch.last_error = CAMERA_PHOTO_ERR_IO;
        std::fclose(stitch.csv);
        stitch.csv = nullptr;
        unlink(stitch.metadata_csv.c_str());
        stitch.encoder.shutdown();
        return CAMERA_PHOTO_ERR_IO;
    }
    for (auto &pending : stitch.pending)
        pending.clear();
    stitch.queue.clear();
    stitch.generation++;
    if (!stitch.generation)
        stitch.generation = 1;
    stitch.enabled = true;
    stitch.last_error = CAMERA_PHOTO_OK;
    stitch.last_mpp_error = MPP_OK;
    stitch.last_errno = 0;
    return CAMERA_PHOTO_OK;
}

extern "C" int camera_photo_stitch_start(camera_photo_backend_t *backend,
                                          const char *output_dir)
{
    if (!backend)
        return CAMERA_PHOTO_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lifecycle(backend->stitch_lifecycle_mutex);
    return camera_photo_stitch_start_internal(backend, output_dir, false);
}

static int camera_photo_stitch_stop_internal(camera_photo_backend_t *backend,
                                             bool transfer_mode)
{
    if (!backend)
        return CAMERA_PHOTO_ERR_ARGUMENT;
    if (!transfer_mode && backend->stitch_transfer.get_status().enabled)
        return CAMERA_PHOTO_ERR_ALREADY_RUNNING;
    CameraPhotoStitch &stitch = backend->stitch;
    std::unique_lock<std::mutex> lock(stitch.mutex);
    if (!stitch.enabled) {
        if (stitch.csv) {
            std::fclose(stitch.csv);
            stitch.csv = nullptr;
        }
        return CAMERA_PHOTO_ERR_NOT_RUNNING;
    }
    stitch.stopping = true;
    stitch.enabled = false;
    for (int camera_id = 0; camera_id < CAMERA_PHOTO_CAMERA_COUNT;
         ++camera_id) {
        stitch.unmatched_drops[camera_id] +=
            stitch.pending[camera_id].size();
        stitch.pending[camera_id].clear();
    }
    stitch.idle.wait(lock, [&stitch] {
        return stitch.assembling == 0 && stitch.queue.empty() &&
               !stitch.processing;
    });
    int session_result = stitch.last_error;
    if (stitch.csv) {
        if ((std::fflush(stitch.csv) != 0 ||
             fsync(fileno(stitch.csv)) != 0) &&
            session_result == CAMERA_PHOTO_OK) {
            stitch.last_errno = errno ? errno : EIO;
            stitch.last_error = CAMERA_PHOTO_ERR_IO;
            session_result = CAMERA_PHOTO_ERR_IO;
        }
        if (std::fclose(stitch.csv) != 0 &&
            session_result == CAMERA_PHOTO_OK) {
            stitch.last_errno = errno ? errno : EIO;
            stitch.last_error = CAMERA_PHOTO_ERR_IO;
            session_result = CAMERA_PHOTO_ERR_IO;
        }
        stitch.csv = nullptr;
    }
    stitch.encoder.shutdown();
    stitch.stopping = false;
    stitch.idle.notify_all();
    return session_result;
}

extern "C" int camera_photo_stitch_stop(camera_photo_backend_t *backend)
{
    if (!backend)
        return CAMERA_PHOTO_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lifecycle(backend->stitch_lifecycle_mutex);
    return camera_photo_stitch_stop_internal(backend, false);
}

extern "C" int camera_photo_stitch_is_enabled(
    camera_photo_backend_t *backend)
{
    if (!backend)
        return 0;
    std::lock_guard<std::mutex> lock(backend->stitch.mutex);
    return backend->stitch.enabled ? 1 : 0;
}

extern "C" int camera_photo_stitch_submit_nv12(
    camera_photo_backend_t *backend, int camera_id, const void *plane0,
    size_t plane0_size, const void *plane1, size_t plane1_size,
    const camera_photo_metadata_t *metadata)
{
    if (!backend || !valid_camera_id(camera_id) || !plane0 || !plane1 ||
        !metadata || metadata->camera_id != camera_id)
        return CAMERA_PHOTO_ERR_ARGUMENT;
    const size_t y_size =
        static_cast<size_t>(backend->config.width) * backend->config.height;
    const size_t uv_size = y_size / 2U;
    if (plane0_size < y_size || plane1_size < uv_size)
        return CAMERA_PHOTO_ERR_RANGE;

    CameraPhotoStitch &stitch = backend->stitch;
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(stitch.mutex);
        if (!stitch.enabled)
            return CAMERA_PHOTO_ERR_NOT_RUNNING;
        if (!valid_metadata(*metadata)) {
            stitch.invalid_metadata++;
            stitch.last_error = CAMERA_PHOTO_ERR_ARGUMENT;
            return CAMERA_PHOTO_ERR_ARGUMENT;
        }
        generation = stitch.generation;
    }

    StitchFrame incoming;
    try {
        incoming.nv12.resize(y_size + uv_size);
        std::memcpy(incoming.nv12.data(), plane0, y_size);
        std::memcpy(incoming.nv12.data() + y_size, plane1, uv_size);
        incoming.metadata = *metadata;
    } catch (const std::bad_alloc &) {
        std::lock_guard<std::mutex> lock(stitch.mutex);
        stitch.queue_drops++;
        stitch.last_errno = ENOMEM;
        stitch.last_error = CAMERA_PHOTO_ERR_IO;
        return CAMERA_PHOTO_ERR_IO;
    }

    StitchFrame matched;
    StitchWork work;
    {
        std::unique_lock<std::mutex> lock(stitch.mutex);
        if (!stitch.enabled || stitch.generation != generation)
            return CAMERA_PHOTO_ERR_NOT_RUNNING;
        stitch.frames_submitted[camera_id]++;
        const int other_camera = 1 - camera_id;
        auto &candidates = stitch.pending[other_camera];
        auto selected = candidates.end();
        if (metadata->trigger_id) {
            selected = std::find_if(
                candidates.begin(), candidates.end(), [metadata](const auto &f) {
                    return f.metadata.trigger_id == metadata->trigger_id;
                });
        } else {
            uint64_t best_distance = UINT64_MAX;
            for (auto candidate = candidates.begin();
                 candidate != candidates.end(); ++candidate) {
                if (candidate->metadata.trigger_id)
                    continue;
                const uint64_t distance = timestamp_distance(
                    candidate->metadata.frame_monotonic_ns,
                    metadata->frame_monotonic_ns);
                if (distance < best_distance) {
                    best_distance = distance;
                    selected = candidate;
                }
            }
            if (selected != candidates.end() &&
                best_distance > backend->config.stitch_pair_tolerance_ns) {
                selected = candidates.end();
            }
        }
        if (selected == candidates.end()) {
            auto &same_camera = stitch.pending[camera_id];
            if (same_camera.size() >=
                backend->config.stitch_pending_depth) {
                same_camera.pop_front();
                stitch.unmatched_drops[camera_id]++;
            }
            same_camera.push_back(std::move(incoming));
            return CAMERA_PHOTO_OK;
        }
        matched = std::move(*selected);
        candidates.erase(selected);
        StitchFrame top = camera_id == 0 ? std::move(incoming)
                                         : std::move(matched);
        StitchFrame bottom = camera_id == 1 ? std::move(incoming)
                                            : std::move(matched);
        work.top = top.metadata;
        work.bottom = bottom.metadata;
        work.frame_delta_ns = signed_timestamp_delta(
            work.bottom.frame_monotonic_ns, work.top.frame_monotonic_ns);
        stitch.pairs_matched++;
        if (work.top.trigger_id &&
            work.top.trigger_id == work.bottom.trigger_id) {
            work.pair_id = work.top.trigger_id;
        } else {
            work.pair_id =
                (static_cast<uint64_t>(work.top.frame_id) << 32U) |
                work.bottom.frame_id;
            if (!work.pair_id)
                work.pair_id = stitch.pairs_matched;
        }
        stitch.assembling++;
        lock.unlock();

        const int stitch_result = camera_photo::stitch_nv12_vertical(
            backend->config.width, backend->config.height,
            top.nv12.data(), y_size, top.nv12.data() + y_size, uv_size,
            bottom.nv12.data(), y_size, bottom.nv12.data() + y_size, uv_size,
            &work.nv12);

        lock.lock();
        stitch.assembling--;
        if (stitch_result != 0) {
            stitch.queue_drops++;
            stitch.last_error = CAMERA_PHOTO_ERR_RANGE;
            if (stitch.assembling == 0 && stitch.queue.empty() &&
                !stitch.processing) {
                stitch.idle.notify_all();
            }
            return CAMERA_PHOTO_ERR_RANGE;
        }
        if (!stitch.enabled || stitch.generation != generation) {
            stitch.queue_drops++;
            if (stitch.assembling == 0 && stitch.queue.empty() &&
                !stitch.processing) {
                stitch.idle.notify_all();
            }
            return CAMERA_PHOTO_ERR_NOT_RUNNING;
        }
        if (stitch.queue.size() >= backend->config.queue_depth) {
            stitch.queue.pop_front();
            stitch.queue_drops++;
            stitch.last_error = CAMERA_PHOTO_ERR_QUEUE_FULL;
        }
        stitch.queue.push_back(std::move(work));
        stitch.condition.notify_one();
    }
    return CAMERA_PHOTO_OK;
}

extern "C" int camera_photo_stitch_get_status(
    camera_photo_backend_t *backend, camera_photo_stitch_status_t *status)
{
    if (!backend || !status)
        return CAMERA_PHOTO_ERR_ARGUMENT;
    CameraPhotoStitch &stitch = backend->stitch;
    std::lock_guard<std::mutex> lock(stitch.mutex);
    std::memset(status, 0, sizeof(*status));
    status->enabled = stitch.enabled;
    status->processing = stitch.processing;
    status->last_error = stitch.last_error;
    status->last_mpp_error = stitch.last_mpp_error;
    status->last_errno = stitch.last_errno;
    status->width = backend->config.width;
    status->height = backend->config.height * 2U;
    status->jpeg_quality = backend->config.jpeg_quality;
    status->queue_pending = stitch.queue.size();
    status->pending_cam0 = stitch.pending[0].size();
    status->pending_cam1 = stitch.pending[1].size();
    status->assembling = stitch.assembling;
    status->frames_submitted_cam0 = stitch.frames_submitted[0];
    status->frames_submitted_cam1 = stitch.frames_submitted[1];
    status->pairs_matched = stitch.pairs_matched;
    status->photos_saved = stitch.photos_saved;
    status->unmatched_drops_cam0 = stitch.unmatched_drops[0];
    status->unmatched_drops_cam1 = stitch.unmatched_drops[1];
    status->queue_drops = stitch.queue_drops;
    status->invalid_metadata = stitch.invalid_metadata;
    status->encode_errors = stitch.encode_errors;
    status->exif_errors = stitch.exif_errors;
    status->write_errors = stitch.write_errors;
    status->jpeg_bytes = stitch.jpeg_bytes;
    status->last_pair_id = stitch.last_pair_id;
    status->last_top_frame_id = stitch.last_top_frame_id;
    status->last_bottom_frame_id = stitch.last_bottom_frame_id;
    status->last_frame_delta_ns = stitch.last_frame_delta_ns;
    status->last_composite_capture_realtime_ns =
        stitch.last_composite_capture_realtime_ns;
    status->last_top_frame_realtime_ns = stitch.last_top_frame_realtime_ns;
    status->last_bottom_frame_realtime_ns =
        stitch.last_bottom_frame_realtime_ns;
    status->last_top_exposure_us = stitch.last_top_exposure_us;
    status->last_bottom_exposure_us = stitch.last_bottom_exposure_us;
    status->last_top_iso = stitch.last_top_iso;
    status->last_bottom_iso = stitch.last_bottom_iso;
    status->last_composite_utc_valid = stitch.last_composite_utc_valid;
    std::snprintf(status->output_dir, sizeof(status->output_dir), "%s",
                  stitch.output_dir.c_str());
    std::snprintf(status->metadata_csv, sizeof(status->metadata_csv), "%s",
                  stitch.metadata_csv.c_str());
    std::snprintf(status->last_photo, sizeof(status->last_photo), "%s",
                  stitch.last_photo.c_str());
    return CAMERA_PHOTO_OK;
}

extern "C" int camera_photo_stitch_transfer_start(
    camera_photo_backend_t *backend, const char *output_dir,
    const char *host, uint16_t port)
{
    if (!backend || !output_dir || !*output_dir || !host || !*host || !port)
        return CAMERA_PHOTO_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lifecycle(backend->stitch_lifecycle_mutex);
    if (camera_photo_stitch_is_enabled(backend) ||
        backend->stitch_transfer.get_status().enabled) {
        return CAMERA_PHOTO_ERR_ALREADY_RUNNING;
    }
    const int directory_error = make_directories(output_dir);
    if (directory_error)
        return CAMERA_PHOTO_ERR_IO;
    const int transfer_result =
        backend->stitch_transfer.start(host, port, output_dir);
    if (transfer_result != camera_stitch_transfer::ok)
        return transfer_result == camera_stitch_transfer::err_already_running
                   ? CAMERA_PHOTO_ERR_ALREADY_RUNNING
                   : CAMERA_PHOTO_ERR_ARGUMENT;
    const int stitch_result =
        camera_photo_stitch_start_internal(backend, output_dir, true);
    if (stitch_result != CAMERA_PHOTO_OK) {
        backend->stitch_transfer.stop();
        return stitch_result;
    }
    return CAMERA_PHOTO_OK;
}

extern "C" int camera_photo_stitch_transfer_stop(
    camera_photo_backend_t *backend)
{
    if (!backend)
        return CAMERA_PHOTO_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lifecycle(backend->stitch_lifecycle_mutex);
    const bool stitch_enabled = camera_photo_stitch_is_enabled(backend) != 0;
    const bool transfer_enabled = backend->stitch_transfer.get_status().enabled;
    if (!stitch_enabled && !transfer_enabled)
        return CAMERA_PHOTO_ERR_NOT_RUNNING;

    int stitch_result = CAMERA_PHOTO_OK;
    if (stitch_enabled)
        stitch_result = camera_photo_stitch_stop_internal(backend, true);
    int transfer_result = camera_stitch_transfer::ok;
    if (transfer_enabled)
        transfer_result = backend->stitch_transfer.stop();
    if (stitch_result != CAMERA_PHOTO_OK)
        return stitch_result;
    return transfer_result == camera_stitch_transfer::ok
               ? CAMERA_PHOTO_OK
               : CAMERA_PHOTO_ERR_IO;
}

extern "C" int camera_photo_stitch_transfer_get_status(
    camera_photo_backend_t *backend,
    camera_photo_stitch_transfer_status_t *status)
{
    if (!backend || !status)
        return CAMERA_PHOTO_ERR_ARGUMENT;
    const camera_stitch_transfer::status source =
        backend->stitch_transfer.get_status();
    std::memset(status, 0, sizeof(*status));
    status->enabled = source.enabled;
    status->processing = source.processing;
    status->port = source.port;
    status->session_id = source.session_id;
    status->enqueued = source.enqueued;
    status->delivered = source.delivered;
    status->retries = source.retries;
    status->failed = source.failed;
    status->queue_rejected = source.queue_rejected;
    status->delete_errors = source.delete_errors;
    status->queue_pending = source.queue_pending;
    status->backlog = source.backlog;
    status->backlog_peak = source.backlog_peak;
    status->deferred = source.deferred;
    status->recovered = source.recovered;
    status->spool_recovered = source.spool_recovered;
    status->orphan_recovered = source.orphan_recovered;
    status->orphan_unrecoverable = source.orphan_unrecoverable;
    status->spool_recovery_errors = source.spool_recovery_errors;
    status->bytes = source.bytes;
    status->last_result = source.last_result;
    status->last_errno = source.last_errno;
    std::snprintf(status->host, sizeof(status->host), "%s",
                  source.host.c_str());
    std::snprintf(status->last_error, sizeof(status->last_error), "%s",
                  source.last_error.c_str());
    std::snprintf(status->last_path, sizeof(status->last_path), "%s",
                  source.last_path.c_str());
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
    case CAMERA_PHOTO_ERR_QUEUE_FULL:
        return "photo queue overflowed and a frame was dropped";
    default:
        return "unknown photo output error";
    }
}
