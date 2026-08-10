#include "capture_backend.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <mutex>
#include <new>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr unsigned int kBufferCount = 4;
constexpr unsigned int kPlaneCount = 2;
constexpr size_t kSaveQueueDepth = 4;
constexpr size_t kRecentTimestampCount = 32;

struct MappedPlane {
    void *address = nullptr;
    size_t length = 0;
};

struct MappedBuffer {
    MappedPlane planes[kPlaneCount];
};

struct SaveFrame {
    std::vector<uint8_t> data;
    uint32_t sequence = 0;
    uint32_t buffer_flags = 0;
    uint64_t monotonic_timestamp_ns = 0;
    uint64_t realtime_timestamp_ns = 0;
};

struct FrameTimestamp {
    uint32_t sequence = 0;
    uint32_t buffer_flags = 0;
    uint64_t timestamp_ns = 0;
};

struct CaptureSlot {
    std::mutex state_mutex;
    std::mutex save_mutex;
    std::condition_variable save_condition;
    std::condition_variable save_drained;
    std::thread capture_thread;
    std::thread writer_thread;
    std::atomic<bool> stop_capture{false};
    bool stop_writer = false;
    bool running = false;
    bool saving = false;
    bool writer_busy = false;
    int camera_id = -1;
    int video_fd = -1;
    int metadata_fd = -1;
    int last_errno = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps_x1000 = 0;
    uint32_t fps_measurement_target = 0;
    uint32_t fps_transition_intervals = 0;
    bool fps_stable = false;
    uint32_t mapped_count = 0;
    bool have_sequence = false;
    uint32_t last_sequence = 0;
    uint32_t last_buffer_flags = 0;
    uint64_t last_v4l2_timestamp_ns = 0;
    uint64_t last_realtime_dequeue_ns = 0;
    uint64_t frames_captured = 0;
    uint64_t frames_dropped = 0;
    uint64_t frames_saved = 0;
    uint64_t save_queue_dropped = 0;
    uint64_t save_failures = 0;
    uint64_t bytes_saved = 0;
    std::chrono::steady_clock::time_point first_frame_time;
    std::chrono::steady_clock::time_point last_frame_time;
    std::string video_device;
    std::string output_dir;
    std::string last_saved_path;
    std::string metadata_path;
    capture_backend_frame_callback_t frame_callback = nullptr;
    void *frame_callback_user = nullptr;
    capture_backend_frame_event_callback_t frame_event_callback = nullptr;
    void *frame_event_callback_user = nullptr;
    MappedBuffer mapped[kBufferCount];
    std::deque<SaveFrame> save_queue;
    std::deque<FrameTimestamp> recent_timestamps;
};

int xioctl(int fd, unsigned long request, void *argument)
{
    int result;
    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}

uint64_t realtime_ns()
{
    struct timespec now = {};
    clock_gettime(CLOCK_REALTIME, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(now.tv_nsec);
}

uint64_t timeval_ns(const struct timeval &value)
{
    return static_cast<uint64_t>(value.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(value.tv_usec) * 1000ULL;
}

bool make_directory_tree(const char *path)
{
    if (path == nullptr || path[0] != '/') {
        errno = EINVAL;
        return false;
    }

    std::string current;
    std::string input(path);
    for (size_t index = 1; index <= input.size(); ++index) {
        if (index != input.size() && input[index] != '/')
            continue;
        if (index == 1)
            continue;
        current = input.substr(0, index);
        if (current.empty())
            continue;
        if (mkdir(current.c_str(), 0755) < 0 && errno != EEXIST)
            return false;
        struct stat information = {};
        if (stat(current.c_str(), &information) < 0 ||
            !S_ISDIR(information.st_mode)) {
            errno = ENOTDIR;
            return false;
        }
    }
    return true;
}

bool write_all(int fd, const uint8_t *data, size_t length)
{
    while (length > 0) {
        ssize_t written = write(fd, data, length);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (written == 0) {
            errno = EIO;
            return false;
        }
        data += static_cast<size_t>(written);
        length -= static_cast<size_t>(written);
    }
    return true;
}

bool write_text(int fd, const char *text, size_t length)
{
    return write_all(fd, reinterpret_cast<const uint8_t *>(text), length);
}

bool is_monotonic_timestamp(uint32_t buffer_flags)
{
    return (buffer_flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) ==
           V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
}

bool save_frame(CaptureSlot *camera, const SaveFrame &frame,
                uint64_t file_index,
                std::string *saved_path)
{
    char filename[256];
    std::snprintf(filename, sizeof(filename),
                  "cam%d_frame_%010llu_seq_%010u_rt_%019llu.nv12",
                  camera->camera_id,
                  static_cast<unsigned long long>(file_index),
                  frame.sequence,
                  static_cast<unsigned long long>(frame.realtime_timestamp_ns));
    const std::string output_path = camera->output_dir + "/" + filename;
    const std::string temporary_path = output_path + ".tmp";

    int output_fd = open(temporary_path.c_str(),
                         O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
    if (output_fd < 0)
        return false;
    bool written = write_all(output_fd, frame.data.data(), frame.data.size());
    int write_error = errno;
    if (close(output_fd) < 0 && written) {
        written = false;
        write_error = errno;
    }
    if (!written) {
        unlink(temporary_path.c_str());
        errno = write_error;
        return false;
    }
    if (rename(temporary_path.c_str(), output_path.c_str()) < 0) {
        int rename_error = errno;
        unlink(temporary_path.c_str());
        errno = rename_error;
        return false;
    }

    char metadata[1024];
    int metadata_length = std::snprintf(
        metadata, sizeof(metadata),
        "%d,%llu,%u,0x%08x,%llu,%llu,%zu,%s\n",
        camera->camera_id, static_cast<unsigned long long>(file_index),
        frame.sequence, frame.buffer_flags,
        static_cast<unsigned long long>(frame.monotonic_timestamp_ns),
        static_cast<unsigned long long>(frame.realtime_timestamp_ns),
        frame.data.size(), filename);
    if (camera->metadata_fd < 0 || metadata_length < 0 ||
        static_cast<size_t>(metadata_length) >= sizeof(metadata) ||
        !write_text(camera->metadata_fd, metadata,
                    static_cast<size_t>(metadata_length))) {
        int metadata_error = errno != 0 ? errno : EIO;
        unlink(output_path.c_str());
        errno = metadata_error;
        return false;
    }
    *saved_path = output_path;
    return true;
}

void writer_worker(CaptureSlot *camera)
{
    for (;;) {
        SaveFrame frame;
        {
            std::unique_lock<std::mutex> lock(camera->save_mutex);
            camera->save_condition.wait(lock, [camera] {
                return camera->stop_writer || !camera->save_queue.empty();
            });
            if (camera->stop_writer && camera->save_queue.empty())
                break;
            frame = std::move(camera->save_queue.front());
            camera->save_queue.pop_front();
            camera->writer_busy = true;
        }

        std::string saved_path;
        uint64_t file_index = 0;
        {
            std::lock_guard<std::mutex> state_lock(camera->state_mutex);
            file_index = camera->frames_saved + 1;
        }
        bool saved = save_frame(camera, frame, file_index, &saved_path);
        int save_error = saved ? 0 : errno;

        {
            std::lock_guard<std::mutex> state_lock(camera->state_mutex);
            if (saved) {
                ++camera->frames_saved;
                camera->bytes_saved += frame.data.size();
                camera->last_saved_path = saved_path;
            } else {
                ++camera->save_failures;
                camera->last_errno = save_error;
            }
        }
        {
            std::lock_guard<std::mutex> save_lock(camera->save_mutex);
            camera->writer_busy = false;
            if (!saved) {
                camera->saving = false;
                camera->save_queue.clear();
                if (camera->metadata_fd >= 0) {
                    close(camera->metadata_fd);
                    camera->metadata_fd = -1;
                }
            }
            if (camera->save_queue.empty())
                camera->save_drained.notify_all();
        }

        if (!saved) {
            std::fprintf(stderr,
                         "CAPTURE_SAVE_ERROR camera_id=%d errno=%d reason=%s\n",
                         camera->camera_id, save_error,
                         std::strerror(save_error));
        }
    }
}

void capture_worker(CaptureSlot *camera)
{
    while (!camera->stop_capture.load()) {
        struct pollfd descriptor = {camera->video_fd, POLLIN, 0};
        int poll_result = poll(&descriptor, 1, 2000);
        if (poll_result < 0) {
            if (errno == EINTR)
                continue;
            std::lock_guard<std::mutex> lock(camera->state_mutex);
            camera->last_errno = errno;
            break;
        }
        if (poll_result == 0)
            continue;
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            std::lock_guard<std::mutex> lock(camera->state_mutex);
            camera->last_errno = EIO;
            break;
        }

        struct v4l2_buffer buffer = {};
        struct v4l2_plane planes[kPlaneCount] = {};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.m.planes = planes;
        buffer.length = kPlaneCount;
        if (xioctl(camera->video_fd, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN)
                continue;
            std::lock_guard<std::mutex> lock(camera->state_mutex);
            camera->last_errno = errno;
            break;
        }
        if (buffer.index >= camera->mapped_count) {
            std::lock_guard<std::mutex> lock(camera->state_mutex);
            camera->last_errno = EPROTO;
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        const uint64_t frame_timestamp_ns = timeval_ns(buffer.timestamp);
        const uint64_t dequeue_realtime_ns = realtime_ns();
        {
            std::lock_guard<std::mutex> lock(camera->state_mutex);
            ++camera->frames_captured;
            if (!camera->have_sequence) {
                camera->have_sequence = true;
                camera->first_frame_time = now;
            } else {
                uint32_t difference = buffer.sequence - camera->last_sequence;
                if (difference > 1 && difference < 0x80000000U)
                    camera->frames_dropped += difference - 1;
            }
            camera->last_sequence = buffer.sequence;
            camera->last_buffer_flags = buffer.flags;
            camera->last_v4l2_timestamp_ns = frame_timestamp_ns;
            camera->last_realtime_dequeue_ns = dequeue_realtime_ns;
            if (camera->fps_measurement_target != 0 &&
                !camera->recent_timestamps.empty()) {
                const uint64_t previous_ns =
                    camera->recent_timestamps.back().timestamp_ns;
                const uint64_t expected_interval_ns =
                    1000000000ULL / camera->fps_measurement_target;
                const uint64_t minimum_interval_ns =
                    expected_interval_ns * 3ULL / 4ULL;
                const uint64_t maximum_interval_ns =
                    expected_interval_ns * 5ULL / 4ULL;
                const uint64_t interval_ns =
                    frame_timestamp_ns > previous_ns
                        ? frame_timestamp_ns - previous_ns
                        : 0;
                if (interval_ns < minimum_interval_ns ||
                    interval_ns > maximum_interval_ns) {
                    camera->recent_timestamps.clear();
                    camera->fps_transition_intervals = 0;
                    camera->fps_stable = false;
                    camera->fps_x1000 = 0;
                } else if (camera->fps_transition_intervals < UINT32_MAX) {
                    ++camera->fps_transition_intervals;
                    if (camera->fps_transition_intervals >= 3)
                        camera->fps_stable = true;
                }
            }
            camera->recent_timestamps.push_back(
                {buffer.sequence, buffer.flags, frame_timestamp_ns});
            if (camera->recent_timestamps.size() > kRecentTimestampCount)
                camera->recent_timestamps.pop_front();
            camera->last_frame_time = now;
            if (camera->recent_timestamps.size() > 1 &&
                (camera->fps_measurement_target == 0 ||
                 camera->fps_stable)) {
                const uint64_t first_ns =
                    camera->recent_timestamps.front().timestamp_ns;
                const uint64_t last_ns =
                    camera->recent_timestamps.back().timestamp_ns;
                if (last_ns > first_ns) {
                    const uint64_t elapsed_ns = last_ns - first_ns;
                    const uint64_t intervals =
                        camera->recent_timestamps.size() - 1;
                    camera->fps_x1000 = static_cast<uint32_t>(
                        (intervals * 1000000000000ULL + elapsed_ns / 2) /
                        elapsed_ns);
                }
            }
        }

        capture_backend_frame_callback_t frame_callback = nullptr;
        void *frame_callback_user = nullptr;
        capture_backend_frame_event_callback_t frame_event_callback = nullptr;
        void *frame_event_callback_user = nullptr;
        {
            std::lock_guard<std::mutex> lock(camera->state_mutex);
            frame_callback = camera->frame_callback;
            frame_callback_user = camera->frame_callback_user;
            frame_event_callback = camera->frame_event_callback;
            frame_event_callback_user = camera->frame_event_callback_user;
        }
        if (frame_event_callback != nullptr) {
            frame_event_callback(camera->camera_id, buffer.sequence,
                                 buffer.flags, frame_timestamp_ns,
                                 dequeue_realtime_ns,
                                 frame_event_callback_user);
        }
        if (frame_callback != nullptr) {
            size_t plane_size[kPlaneCount] = {};
            for (unsigned int plane = 0; plane < kPlaneCount; ++plane) {
                plane_size[plane] = planes[plane].bytesused;
                if (plane_size[plane] == 0 ||
                    plane_size[plane] >
                        camera->mapped[buffer.index].planes[plane].length) {
                    plane_size[plane] =
                        camera->mapped[buffer.index].planes[plane].length;
                }
            }
            frame_callback(
                camera->camera_id,
                camera->mapped[buffer.index].planes[0].address,
                plane_size[0],
                camera->mapped[buffer.index].planes[1].address,
                plane_size[1], buffer.sequence, frame_callback_user);
        }

        {
            std::lock_guard<std::mutex> save_lock(camera->save_mutex);
            if (camera->saving) {
                if (camera->save_queue.size() >= kSaveQueueDepth) {
                    std::lock_guard<std::mutex> state_lock(camera->state_mutex);
                    ++camera->save_queue_dropped;
                } else {
                    try {
                        SaveFrame frame;
                        frame.sequence = buffer.sequence;
                        frame.buffer_flags = buffer.flags;
                        frame.monotonic_timestamp_ns = frame_timestamp_ns;
                        frame.realtime_timestamp_ns = dequeue_realtime_ns;
                        size_t total_size = 0;
                        for (unsigned int plane = 0; plane < kPlaneCount;
                             ++plane) {
                            size_t bytes_used = planes[plane].bytesused;
                            if (bytes_used == 0 ||
                                bytes_used > camera->mapped[buffer.index]
                                                 .planes[plane]
                                                 .length) {
                                bytes_used = camera->mapped[buffer.index]
                                                 .planes[plane]
                                                 .length;
                            }
                            total_size += bytes_used;
                        }
                        frame.data.reserve(total_size);
                        for (unsigned int plane = 0; plane < kPlaneCount;
                             ++plane) {
                            size_t bytes_used = planes[plane].bytesused;
                            if (bytes_used == 0 ||
                                bytes_used > camera->mapped[buffer.index]
                                                 .planes[plane]
                                                 .length) {
                                bytes_used = camera->mapped[buffer.index]
                                                 .planes[plane]
                                                 .length;
                            }
                            const uint8_t *begin =
                                static_cast<const uint8_t *>(
                                    camera->mapped[buffer.index]
                                        .planes[plane]
                                        .address);
                            frame.data.insert(frame.data.end(), begin,
                                              begin + bytes_used);
                        }
                        camera->save_queue.push_back(std::move(frame));
                        camera->save_condition.notify_one();
                    } catch (...) {
                        std::lock_guard<std::mutex> state_lock(
                            camera->state_mutex);
                        ++camera->save_queue_dropped;
                        camera->last_errno = ENOMEM;
                    }
                }
            }
        }

        if (xioctl(camera->video_fd, VIDIOC_QBUF, &buffer) < 0) {
            std::lock_guard<std::mutex> lock(camera->state_mutex);
            camera->last_errno = errno;
            break;
        }
    }

    std::lock_guard<std::mutex> lock(camera->state_mutex);
    camera->running = false;
}

int validate_camera_id(const capture_backend_t *backend, int camera_id)
{
    if (backend == nullptr)
        return CAPTURE_BACKEND_ERR_ARGUMENT;
    if (camera_id < 0 || camera_id >= CAPTURE_BACKEND_CAMERA_COUNT)
        return CAPTURE_BACKEND_ERR_RANGE;
    return CAPTURE_BACKEND_OK;
}

void reset_statistics(CaptureSlot &camera)
{
    camera.last_errno = 0;
    camera.fps_x1000 = 0;
    camera.fps_measurement_target = 0;
    camera.fps_transition_intervals = 0;
    camera.fps_stable = false;
    camera.have_sequence = false;
    camera.last_sequence = 0;
    camera.last_buffer_flags = 0;
    camera.last_v4l2_timestamp_ns = 0;
    camera.last_realtime_dequeue_ns = 0;
    camera.frames_captured = 0;
    camera.frames_dropped = 0;
    camera.frames_saved = 0;
    camera.save_queue_dropped = 0;
    camera.save_failures = 0;
    camera.bytes_saved = 0;
    camera.output_dir.clear();
    camera.last_saved_path.clear();
    camera.metadata_path.clear();
    camera.recent_timestamps.clear();
}

void close_metadata(CaptureSlot &camera)
{
    if (camera.metadata_fd >= 0) {
        close(camera.metadata_fd);
        camera.metadata_fd = -1;
    }
}

void release_stream(CaptureSlot &camera)
{
    if (camera.video_fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        xioctl(camera.video_fd, VIDIOC_STREAMOFF, &type);
    }
    for (unsigned int index = 0; index < camera.mapped_count; ++index) {
        for (unsigned int plane = 0; plane < kPlaneCount; ++plane) {
            MappedPlane &mapped = camera.mapped[index].planes[plane];
            if (mapped.address != nullptr) {
                munmap(mapped.address, mapped.length);
                mapped.address = nullptr;
                mapped.length = 0;
            }
        }
    }
    camera.mapped_count = 0;
    if (camera.video_fd >= 0) {
        close(camera.video_fd);
        camera.video_fd = -1;
    }
}

}  // namespace

struct capture_backend {
    CaptureSlot cameras[CAPTURE_BACKEND_CAMERA_COUNT];
};

extern "C" void capture_backend_default_config(capture_backend_config_t *config)
{
    if (config == nullptr)
        return;
    std::memset(config, 0, sizeof(*config));
    config->width = 4000;
    config->height = 3000;
    config->video_device[0] = "/dev/video22";
    config->video_device[1] = "/dev/video31";
}

extern "C" int capture_backend_create(const capture_backend_config_t *config,
                                      capture_backend_t **backend_out)
{
    if (config == nullptr || backend_out == nullptr || config->width == 0 ||
        config->height == 0) {
        return CAPTURE_BACKEND_ERR_ARGUMENT;
    }
    *backend_out = nullptr;
    for (int camera_id = 0; camera_id < CAPTURE_BACKEND_CAMERA_COUNT;
         ++camera_id) {
        if (config->video_device[camera_id] == nullptr ||
            config->video_device[camera_id][0] == '\0') {
            return CAPTURE_BACKEND_ERR_ARGUMENT;
        }
    }

    capture_backend_t *backend = new (std::nothrow) capture_backend_t;
    if (backend == nullptr)
        return CAPTURE_BACKEND_ERR_IO;
    for (int camera_id = 0; camera_id < CAPTURE_BACKEND_CAMERA_COUNT;
         ++camera_id) {
        CaptureSlot &camera = backend->cameras[camera_id];
        camera.camera_id = camera_id;
        camera.width = config->width;
        camera.height = config->height;
        camera.video_device = config->video_device[camera_id];
    }
    *backend_out = backend;
    return CAPTURE_BACKEND_OK;
}

extern "C" void capture_backend_destroy(capture_backend_t *backend)
{
    if (backend == nullptr)
        return;
    for (int camera_id = 0; camera_id < CAPTURE_BACKEND_CAMERA_COUNT;
         ++camera_id) {
        capture_backend_stop_stream(backend, camera_id);
    }
    delete backend;
}

extern "C" int capture_backend_start_stream(capture_backend_t *backend,
                                             int camera_id)
{
    int result = validate_camera_id(backend, camera_id);
    if (result != CAPTURE_BACKEND_OK)
        return result;
    CaptureSlot &camera = backend->cameras[camera_id];

    {
        std::lock_guard<std::mutex> lock(camera.state_mutex);
        if (camera.running)
            return CAPTURE_BACKEND_ERR_ALREADY_RUNNING;
    }
    if (camera.capture_thread.joinable() || camera.writer_thread.joinable())
        capture_backend_stop_stream(backend, camera_id);

    reset_statistics(camera);
    camera.stop_capture.store(false);
    camera.stop_writer = false;
    camera.saving = false;
    camera.writer_busy = false;
    camera.save_queue.clear();

    camera.video_fd =
        open(camera.video_device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (camera.video_fd < 0) {
        camera.last_errno = errno;
        return CAPTURE_BACKEND_ERR_IO;
    }

    struct v4l2_capability capabilities = {};
    if (xioctl(camera.video_fd, VIDIOC_QUERYCAP, &capabilities) < 0) {
        camera.last_errno = errno;
        release_stream(camera);
        return CAPTURE_BACKEND_ERR_IO;
    }
    uint32_t device_caps = capabilities.device_caps != 0
                               ? capabilities.device_caps
                               : capabilities.capabilities;
    if ((device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) == 0 ||
        (device_caps & V4L2_CAP_STREAMING) == 0) {
        camera.last_errno = ENOTSUP;
        release_stream(camera);
        return CAPTURE_BACKEND_ERR_IO;
    }

    struct v4l2_format format = {};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    format.fmt.pix_mp.width = camera.width;
    format.fmt.pix_mp.height = camera.height;
    format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;
    format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    format.fmt.pix_mp.num_planes = kPlaneCount;
    if (xioctl(camera.video_fd, VIDIOC_S_FMT, &format) < 0 ||
        format.fmt.pix_mp.width != camera.width ||
        format.fmt.pix_mp.height != camera.height ||
        format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12M ||
        format.fmt.pix_mp.num_planes != kPlaneCount) {
        camera.last_errno = errno != 0 ? errno : EINVAL;
        release_stream(camera);
        return CAPTURE_BACKEND_ERR_IO;
    }

    struct v4l2_requestbuffers request = {};
    request.count = kBufferCount;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    request.memory = V4L2_MEMORY_MMAP;
    if (xioctl(camera.video_fd, VIDIOC_REQBUFS, &request) < 0 ||
        request.count < 2 || request.count > kBufferCount) {
        camera.last_errno = errno != 0 ? errno : ENOMEM;
        release_stream(camera);
        return CAPTURE_BACKEND_ERR_IO;
    }
    camera.mapped_count = request.count;

    for (unsigned int index = 0; index < request.count; ++index) {
        struct v4l2_buffer buffer = {};
        struct v4l2_plane planes[kPlaneCount] = {};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        buffer.m.planes = planes;
        buffer.length = kPlaneCount;
        if (xioctl(camera.video_fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            camera.last_errno = errno;
            release_stream(camera);
            return CAPTURE_BACKEND_ERR_IO;
        }
        for (unsigned int plane = 0; plane < kPlaneCount; ++plane) {
            MappedPlane &mapped = camera.mapped[index].planes[plane];
            mapped.length = planes[plane].length;
            mapped.address = mmap(nullptr, mapped.length,
                                  PROT_READ | PROT_WRITE, MAP_SHARED,
                                  camera.video_fd, planes[plane].m.mem_offset);
            if (mapped.address == MAP_FAILED) {
                mapped.address = nullptr;
                camera.last_errno = errno;
                release_stream(camera);
                return CAPTURE_BACKEND_ERR_IO;
            }
        }
        if (xioctl(camera.video_fd, VIDIOC_QBUF, &buffer) < 0) {
            camera.last_errno = errno;
            release_stream(camera);
            return CAPTURE_BACKEND_ERR_IO;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(camera.video_fd, VIDIOC_STREAMON, &type) < 0) {
        camera.last_errno = errno;
        release_stream(camera);
        return CAPTURE_BACKEND_ERR_IO;
    }

    {
        std::lock_guard<std::mutex> lock(camera.state_mutex);
        camera.running = true;
    }
    try {
        camera.writer_thread = std::thread(writer_worker, &camera);
        camera.capture_thread = std::thread(capture_worker, &camera);
    } catch (...) {
        camera.stop_capture.store(true);
        camera.stop_writer = true;
        camera.save_condition.notify_all();
        if (camera.capture_thread.joinable())
            camera.capture_thread.join();
        if (camera.writer_thread.joinable())
            camera.writer_thread.join();
        {
            std::lock_guard<std::mutex> lock(camera.state_mutex);
            camera.running = false;
            camera.last_errno = EAGAIN;
        }
        release_stream(camera);
        return CAPTURE_BACKEND_ERR_IO;
    }
    std::fprintf(stdout,
                 "CAPTURE_STREAM camera_id=%d state=STARTED device=%s size=%ux%u format=NV12M\n",
                 camera_id, camera.video_device.c_str(), camera.width,
                 camera.height);
    std::fflush(stdout);
    return CAPTURE_BACKEND_OK;
}

extern "C" int capture_backend_stop_stream(capture_backend_t *backend,
                                            int camera_id)
{
    int result = validate_camera_id(backend, camera_id);
    if (result != CAPTURE_BACKEND_OK)
        return result;
    CaptureSlot &camera = backend->cameras[camera_id];
    if (!camera.capture_thread.joinable() && !camera.writer_thread.joinable() &&
        camera.video_fd < 0) {
        return CAPTURE_BACKEND_ERR_NOT_RUNNING;
    }

    {
        std::unique_lock<std::mutex> lock(camera.save_mutex);
        camera.saving = false;
        camera.save_drained.wait(lock, [&camera] {
            return camera.save_queue.empty() && !camera.writer_busy;
        });
        close_metadata(camera);
    }
    camera.stop_capture.store(true);
    if (camera.capture_thread.joinable())
        camera.capture_thread.join();
    {
        std::lock_guard<std::mutex> lock(camera.save_mutex);
        camera.stop_writer = true;
        camera.save_condition.notify_all();
    }
    if (camera.writer_thread.joinable())
        camera.writer_thread.join();
    release_stream(camera);
    {
        std::lock_guard<std::mutex> lock(camera.state_mutex);
        camera.running = false;
    }
    std::fprintf(stdout,
                 "CAPTURE_STREAM camera_id=%d state=STOPPED frames=%llu saved=%llu dropped=%llu\n",
                 camera_id,
                 static_cast<unsigned long long>(camera.frames_captured),
                 static_cast<unsigned long long>(camera.frames_saved),
                 static_cast<unsigned long long>(camera.frames_dropped));
    std::fflush(stdout);
    return CAPTURE_BACKEND_OK;
}

extern "C" int capture_backend_set_frame_callback(
    capture_backend_t *backend, int camera_id,
    capture_backend_frame_callback_t callback, void *user_data)
{
    int result = validate_camera_id(backend, camera_id);
    if (result != CAPTURE_BACKEND_OK)
        return result;
    CaptureSlot &camera = backend->cameras[camera_id];
    std::lock_guard<std::mutex> lock(camera.state_mutex);
    if (camera.running)
        return CAPTURE_BACKEND_ERR_ALREADY_RUNNING;
    camera.frame_callback = callback;
    camera.frame_callback_user = callback != nullptr ? user_data : nullptr;
    return CAPTURE_BACKEND_OK;
}

extern "C" int capture_backend_set_frame_event_callback(
    capture_backend_t *backend, int camera_id,
    capture_backend_frame_event_callback_t callback, void *user_data)
{
    int result = validate_camera_id(backend, camera_id);
    if (result != CAPTURE_BACKEND_OK)
        return result;
    CaptureSlot &camera = backend->cameras[camera_id];
    std::lock_guard<std::mutex> lock(camera.state_mutex);
    if (camera.running)
        return CAPTURE_BACKEND_ERR_ALREADY_RUNNING;
    camera.frame_event_callback = callback;
    camera.frame_event_callback_user =
        callback != nullptr ? user_data : nullptr;
    return CAPTURE_BACKEND_OK;
}

extern "C" int capture_backend_start_save(capture_backend_t *backend,
                                           int camera_id,
                                           const char *output_dir)
{
    int result = validate_camera_id(backend, camera_id);
    if (result != CAPTURE_BACKEND_OK)
        return result;
    if (output_dir == nullptr || output_dir[0] == '\0')
        return CAPTURE_BACKEND_ERR_ARGUMENT;
    CaptureSlot &camera = backend->cameras[camera_id];
    {
        std::lock_guard<std::mutex> lock(camera.state_mutex);
        if (!camera.running)
            return CAPTURE_BACKEND_ERR_NOT_RUNNING;
    }
    {
        std::lock_guard<std::mutex> lock(camera.save_mutex);
        if (camera.saving)
            return CAPTURE_BACKEND_ERR_ALREADY_SAVING;
    }
    if (!make_directory_tree(output_dir)) {
        std::lock_guard<std::mutex> lock(camera.state_mutex);
        camera.last_errno = errno;
        return CAPTURE_BACKEND_ERR_IO;
    }
    char metadata_name[64];
    std::snprintf(metadata_name, sizeof(metadata_name), "cam%d_frames.csv",
                  camera_id);
    const std::string metadata_path =
        std::string(output_dir) + "/" + metadata_name;
    int metadata_fd = open(metadata_path.c_str(),
                           O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
    if (metadata_fd < 0) {
        std::lock_guard<std::mutex> lock(camera.state_mutex);
        camera.last_errno = errno;
        return CAPTURE_BACKEND_ERR_IO;
    }
    struct stat metadata_information = {};
    if (fstat(metadata_fd, &metadata_information) < 0) {
        int metadata_error = errno;
        close(metadata_fd);
        std::lock_guard<std::mutex> lock(camera.state_mutex);
        camera.last_errno = metadata_error;
        return CAPTURE_BACKEND_ERR_IO;
    }
    static const char kMetadataHeader[] =
        "camera_id,file_index,v4l2_sequence,buffer_flags,"
        "v4l2_timestamp_ns,realtime_dequeue_ns,bytes,filename\n";
    if (metadata_information.st_size == 0 &&
        !write_text(metadata_fd, kMetadataHeader,
                    sizeof(kMetadataHeader) - 1)) {
        int metadata_error = errno;
        close(metadata_fd);
        std::lock_guard<std::mutex> lock(camera.state_mutex);
        camera.last_errno = metadata_error;
        return CAPTURE_BACKEND_ERR_IO;
    }
    {
        std::lock_guard<std::mutex> lock(camera.save_mutex);
        if (camera.saving) {
            close(metadata_fd);
            return CAPTURE_BACKEND_ERR_ALREADY_SAVING;
        }
        camera.output_dir = output_dir;
        camera.metadata_path = metadata_path;
        camera.metadata_fd = metadata_fd;
        camera.saving = true;
    }
    std::fprintf(stdout,
                 "CAPTURE_SAVE camera_id=%d state=STARTED dir=%s metadata=%s\n",
                 camera_id, output_dir, metadata_path.c_str());
    std::fflush(stdout);
    return CAPTURE_BACKEND_OK;
}

extern "C" int capture_backend_stop_save(capture_backend_t *backend,
                                          int camera_id)
{
    int result = validate_camera_id(backend, camera_id);
    if (result != CAPTURE_BACKEND_OK)
        return result;
    CaptureSlot &camera = backend->cameras[camera_id];
    {
        std::unique_lock<std::mutex> lock(camera.save_mutex);
        if (!camera.saving)
            return CAPTURE_BACKEND_ERR_NOT_SAVING;
        camera.saving = false;
        camera.save_drained.wait(lock, [&camera] {
            return camera.save_queue.empty() && !camera.writer_busy;
        });
        close_metadata(camera);
    }
    std::fprintf(stdout,
                 "CAPTURE_SAVE camera_id=%d state=STOPPED frames_saved=%llu bytes_saved=%llu\n",
                 camera_id,
                 static_cast<unsigned long long>(camera.frames_saved),
                 static_cast<unsigned long long>(camera.bytes_saved));
    std::fflush(stdout);
    return CAPTURE_BACKEND_OK;
}

extern "C" int capture_backend_get_status(capture_backend_t *backend,
                                           int camera_id,
                                           capture_backend_status_t *status)
{
    int result = validate_camera_id(backend, camera_id);
    if (result != CAPTURE_BACKEND_OK)
        return result;
    if (status == nullptr)
        return CAPTURE_BACKEND_ERR_ARGUMENT;
    CaptureSlot &camera = backend->cameras[camera_id];
    std::memset(status, 0, sizeof(*status));
    std::scoped_lock lock(camera.state_mutex, camera.save_mutex);
    status->camera_id = camera_id;
    status->running = camera.running ? 1 : 0;
    status->saving = camera.saving ? 1 : 0;
    status->last_errno = camera.last_errno;
    status->width = camera.width;
    status->height = camera.height;
    status->fps_x1000 = camera.fps_x1000;
    status->fps_target_x1000 = camera.fps_measurement_target * 1000U;
    status->fps_stable = camera.fps_stable ? 1 : 0;
    status->fps_window_frames =
        static_cast<uint32_t>(camera.recent_timestamps.size());
    if (camera.recent_timestamps.size() > 1) {
        const uint64_t first_ns = camera.recent_timestamps.front().timestamp_ns;
        const uint64_t last_ns = camera.recent_timestamps.back().timestamp_ns;
        if (last_ns > first_ns)
            status->fps_window_duration_ns = last_ns - first_ns;
    }
    status->frames_captured = camera.frames_captured;
    status->frames_dropped = camera.frames_dropped;
    status->frames_saved = camera.frames_saved;
    status->save_queue_pending =
        static_cast<uint32_t>(camera.save_queue.size());
    status->save_queue_dropped = camera.save_queue_dropped;
    status->save_failures = camera.save_failures;
    status->bytes_saved = camera.bytes_saved;
    status->timestamp_valid =
        camera.have_sequence && camera.last_v4l2_timestamp_ns != 0;
    status->last_sequence = camera.last_sequence;
    status->last_buffer_flags = camera.last_buffer_flags;
    status->last_v4l2_timestamp_ns = camera.last_v4l2_timestamp_ns;
    status->last_realtime_dequeue_ns = camera.last_realtime_dequeue_ns;
    std::snprintf(status->video_device, sizeof(status->video_device), "%s",
                  camera.video_device.c_str());
    std::snprintf(status->output_dir, sizeof(status->output_dir), "%s",
                  camera.output_dir.c_str());
    std::snprintf(status->last_saved_path, sizeof(status->last_saved_path),
                  "%s", camera.last_saved_path.c_str());
    std::snprintf(status->metadata_path, sizeof(status->metadata_path), "%s",
                  camera.metadata_path.c_str());
    return CAPTURE_BACKEND_OK;
}

extern "C" int capture_backend_reset_fps_window(capture_backend_t *backend,
                                                   int camera_id,
                                                   uint32_t target_fps)
{
    int result = validate_camera_id(backend, camera_id);
    if (result != CAPTURE_BACKEND_OK)
        return result;
    CaptureSlot &camera = backend->cameras[camera_id];
    std::lock_guard<std::mutex> lock(camera.state_mutex);
    camera.recent_timestamps.clear();
    camera.fps_x1000 = 0;
    camera.fps_measurement_target = target_fps;
    camera.fps_transition_intervals = 0;
    camera.fps_stable = false;
    return CAPTURE_BACKEND_OK;
}

extern "C" int capture_backend_get_sync_status(
    capture_backend_t *backend, capture_backend_sync_status_t *status)
{
    if (backend == nullptr || status == nullptr)
        return CAPTURE_BACKEND_ERR_ARGUMENT;
    std::memset(status, 0, sizeof(*status));
    CaptureSlot &cam0 = backend->cameras[0];
    CaptureSlot &cam1 = backend->cameras[1];
    std::scoped_lock lock(cam0.state_mutex, cam1.state_mutex);

    if (!cam0.running || !cam1.running || cam0.recent_timestamps.empty() ||
        cam1.recent_timestamps.empty()) {
        return CAPTURE_BACKEND_OK;
    }

    const FrameTimestamp &cam0_latest = cam0.recent_timestamps.back();
    const FrameTimestamp &cam1_latest = cam1.recent_timestamps.back();
    const bool anchor_is_cam0 =
        cam0_latest.timestamp_ns >= cam1_latest.timestamp_ns;
    const FrameTimestamp &anchor = anchor_is_cam0 ? cam0_latest : cam1_latest;
    const std::deque<FrameTimestamp> &candidates =
        anchor_is_cam0 ? cam1.recent_timestamps : cam0.recent_timestamps;
    if (anchor.timestamp_ns == 0 ||
        !is_monotonic_timestamp(anchor.buffer_flags)) {
        return CAPTURE_BACKEND_OK;
    }

    uint64_t best_delta = UINT64_MAX;
    const FrameTimestamp *best = nullptr;
    for (const FrameTimestamp &candidate : candidates) {
        if (candidate.timestamp_ns == 0 ||
            !is_monotonic_timestamp(candidate.buffer_flags)) {
            continue;
        }
        uint64_t delta = anchor.timestamp_ns >= candidate.timestamp_ns
                             ? anchor.timestamp_ns - candidate.timestamp_ns
                             : candidate.timestamp_ns - anchor.timestamp_ns;
        if (delta < best_delta) {
            best_delta = delta;
            best = &candidate;
        }
    }
    if (best == nullptr)
        return CAPTURE_BACKEND_OK;

    const FrameTimestamp &left = anchor_is_cam0 ? anchor : *best;
    const FrameTimestamp &right = anchor_is_cam0 ? *best : anchor;
    status->valid = 1;
    status->cam0_sequence = left.sequence;
    status->cam1_sequence = right.sequence;
    status->cam0_buffer_flags = left.buffer_flags;
    status->cam1_buffer_flags = right.buffer_flags;
    status->cam0_timestamp_ns = left.timestamp_ns;
    status->cam1_timestamp_ns = right.timestamp_ns;
    status->delta_ns = best_delta;
    return CAPTURE_BACKEND_OK;
}

extern "C" const char *capture_backend_strerror(int result)
{
    switch (result) {
    case CAPTURE_BACKEND_OK:
        return "success";
    case CAPTURE_BACKEND_ERR_ARGUMENT:
        return "invalid argument";
    case CAPTURE_BACKEND_ERR_RANGE:
        return "camera id out of range";
    case CAPTURE_BACKEND_ERR_IO:
        return "V4L2 or storage I/O failed";
    case CAPTURE_BACKEND_ERR_ALREADY_RUNNING:
        return "capture stream is already running";
    case CAPTURE_BACKEND_ERR_NOT_RUNNING:
        return "capture stream is not running";
    case CAPTURE_BACKEND_ERR_ALREADY_SAVING:
        return "saving is already active";
    case CAPTURE_BACKEND_ERR_NOT_SAVING:
        return "saving is not active";
    default:
        return "unknown capture error";
    }
}
