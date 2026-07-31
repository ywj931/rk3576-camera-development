#define MODULE_TAG "camera_net"

#include "camera_net_backend.h"

extern "C" {
#include "rk_mpi.h"
#include "mpp_buffer.h"
#include "mpp_frame.h"
#include "mpp_packet.h"
#include "rk_venc_cfg.h"
}

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <netinet/in.h>
#include <new>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr size_t kQueueDepth = 1;
constexpr size_t kMaxHttpRequest = 4096;
constexpr const char *kMjpegBoundary = "frame";

struct Nv12Frame {
    std::vector<uint8_t> bytes;
    uint32_t sequence = 0;
};

class MppJpegEncoder {
public:
    int initialize(uint32_t width, uint32_t height, uint32_t fps,
                   uint32_t quality);
    int encode(const std::vector<uint8_t> &nv12,
               std::vector<uint8_t> *jpeg);
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

int MppJpegEncoder::initialize(uint32_t width, uint32_t height, uint32_t fps,
                               uint32_t quality)
{
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

}  // namespace

struct camera_net_backend;

struct CameraNetStream {
    camera_net_backend *backend = nullptr;
    int camera_id = -1;
    std::string path;
    std::mutex mutex;
    std::condition_variable condition;
    std::thread worker;
    bool enabled = false;
    bool stop_worker = false;
    int source_camera_id = -1;
    int last_error = CAMERA_NET_OK;
    int last_mpp_error = MPP_OK;
    int last_socket_errno = 0;
    uint32_t last_sequence = 0;
    uint64_t frames_submitted = 0;
    uint64_t frames_encoded = 0;
    uint64_t frames_sent = 0;
    uint64_t queue_drops = 0;
    uint64_t encode_errors = 0;
    uint64_t client_disconnects = 0;
    uint64_t http_errors = 0;
    uint64_t jpeg_bytes = 0;
    bool pacing_started = false;
    std::chrono::steady_clock::time_point next_emit;
    std::deque<Nv12Frame> queue;
    MppJpegEncoder encoder;
    std::mutex clients_mutex;
    std::vector<int> clients;
};

struct camera_net_backend {
    camera_net_config_t config{};
    CameraNetStream streams[CAMERA_NET_CAMERA_COUNT];
    std::mutex server_mutex;
    std::thread server_thread;
    std::atomic<bool> stop_server{false};
    int listen_fd = -1;
    bool server_running = false;
};

namespace {

bool valid_camera_id(int camera_id)
{
    return camera_id >= 0 && camera_id < CAMERA_NET_CAMERA_COUNT;
}

bool send_all(int fd, const void *data, size_t length)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    size_t sent = 0;
    while (sent < length) {
        const ssize_t result = send(fd, bytes + sent, length - sent,
                                    MSG_NOSIGNAL);
        if (result > 0) {
            sent += static_cast<size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

void close_clients(CameraNetStream *stream)
{
    std::lock_guard<std::mutex> lock(stream->clients_mutex);
    for (int fd : stream->clients) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    stream->clients.clear();
}

void broadcast_jpeg(CameraNetStream *stream, const std::vector<uint8_t> &jpeg,
                    uint32_t sequence)
{
    char part_header[256];
    const int part_header_length = std::snprintf(
        part_header, sizeof(part_header),
        "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n"
        "X-Sequence: %u\r\n\r\n",
        kMjpegBoundary, jpeg.size(), sequence);
    if (part_header_length <= 0 ||
        static_cast<size_t>(part_header_length) >= sizeof(part_header)) {
        std::lock_guard<std::mutex> lock(stream->mutex);
        stream->http_errors++;
        stream->last_error = CAMERA_NET_ERR_HTTP;
        return;
    }

    uint64_t delivered = 0;
    uint64_t disconnects = 0;
    int socket_error = 0;
    {
        std::lock_guard<std::mutex> lock(stream->clients_mutex);
        auto client = stream->clients.begin();
        while (client != stream->clients.end()) {
            const bool ok = send_all(*client, part_header,
                                     static_cast<size_t>(part_header_length)) &&
                            send_all(*client, jpeg.data(), jpeg.size()) &&
                            send_all(*client, "\r\n", 2);
            if (ok) {
                ++delivered;
                ++client;
            } else {
                socket_error = errno;
                ++disconnects;
                shutdown(*client, SHUT_RDWR);
                close(*client);
                client = stream->clients.erase(client);
            }
        }
    }
    std::lock_guard<std::mutex> lock(stream->mutex);
    if (delivered)
        stream->frames_sent++;
    if (disconnects) {
        stream->client_disconnects += disconnects;
        stream->last_socket_errno = socket_error;
    }
}

void network_worker(CameraNetStream *stream)
{
    camera_net_backend *backend = stream->backend;
    const uint32_t fps = backend->config.fps ? backend->config.fps : 1;
    const auto frame_period =
        std::chrono::nanoseconds(1000000000ULL / fps);

    for (;;) {
        Nv12Frame frame;
        {
            std::unique_lock<std::mutex> lock(stream->mutex);
            stream->condition.wait(lock, [stream] {
                return stream->stop_worker || !stream->queue.empty();
            });
            if (stream->stop_worker && stream->queue.empty())
                break;
            if (stream->pacing_started) {
                stream->condition.wait_until(lock, stream->next_emit,
                                             [stream] {
                    return stream->stop_worker;
                });
                if (stream->stop_worker && stream->queue.empty())
                    break;
            }
            if (stream->queue.empty())
                continue;
            frame = std::move(stream->queue.back());
            stream->queue.clear();
            stream->next_emit = std::chrono::steady_clock::now() +
                                frame_period;
            stream->pacing_started = true;
        }

        std::vector<uint8_t> jpeg;
        const int mpp_ret = stream->encoder.encode(frame.bytes, &jpeg);
        if (mpp_ret != MPP_OK) {
            std::lock_guard<std::mutex> lock(stream->mutex);
            stream->encode_errors++;
            stream->last_error = CAMERA_NET_ERR_MPP;
            stream->last_mpp_error = mpp_ret;
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(stream->mutex);
            stream->frames_encoded++;
            stream->jpeg_bytes += jpeg.size();
            stream->last_sequence = frame.sequence;
        }
        broadcast_jpeg(stream, jpeg, frame.sequence);
    }
}

void send_text_response(int fd, const char *status, const char *content_type,
                        const std::string &body)
{
    char header[512];
    const int length = std::snprintf(
        header, sizeof(header),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        status, content_type, body.size());
    if (length > 0 && static_cast<size_t>(length) < sizeof(header)) {
        send_all(fd, header, static_cast<size_t>(length));
        send_all(fd, body.data(), body.size());
    }
    close(fd);
}

CameraNetStream *find_stream(camera_net_backend *backend,
                             const std::string &path)
{
    for (CameraNetStream &stream : backend->streams) {
        if (path == stream.path)
            return &stream;
    }
    return nullptr;
}

void handle_http_client(camera_net_backend *backend, int fd)
{
    struct timeval timeout = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    std::string request;
    char buffer[1024];
    while (request.size() < kMaxHttpRequest &&
           request.find("\r\n\r\n") == std::string::npos) {
        const ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
        if (received > 0) {
            request.append(buffer, static_cast<size_t>(received));
            continue;
        }
        if (received < 0 && errno == EINTR)
            continue;
        close(fd);
        return;
    }

    const size_t first_space = request.find(' ');
    const size_t second_space = first_space == std::string::npos
                                    ? std::string::npos
                                    : request.find(' ', first_space + 1);
    if (request.compare(0, first_space, "GET") != 0 ||
        second_space == std::string::npos) {
        send_text_response(fd, "400 Bad Request", "text/plain",
                           "Bad request\n");
        return;
    }
    std::string path = request.substr(first_space + 1,
                                      second_space - first_space - 1);
    const size_t query = path.find('?');
    if (query != std::string::npos)
        path.erase(query);

    if (path == "/" || path == "/index.html") {
        const std::string body =
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>Camera HTTP Output</title><style>body{margin:0;background:#111;"
            "color:#eee;font-family:sans-serif}main{display:grid;gap:12px;padding:12px}"
            "figure{margin:0}img{display:block;width:100%;height:auto;background:#222}"
            "figcaption{padding:6px 0}@media(min-width:900px){main{grid-template-columns:1fr 1fr}}"
            "</style></head><body><main><figure><img src=\"/cam0\"><figcaption>cam0"
            "</figcaption></figure><figure><img src=\"/cam1\"><figcaption>cam1"
            "</figcaption></figure></main></body></html>";
        send_text_response(fd, "200 OK", "text/html; charset=utf-8", body);
        return;
    }

    CameraNetStream *stream = find_stream(backend, path);
    if (!stream) {
        send_text_response(fd, "404 Not Found", "text/plain",
                           "Camera path not found\n");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(stream->mutex);
        if (!stream->enabled) {
            send_text_response(fd, "503 Service Unavailable", "text/plain",
                               "Camera stream is not running\n");
            return;
        }
    }

    char header[512];
    const int length = std::snprintf(
        header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace;"
        "boundary=%s\r\nCache-Control: no-store, no-cache, must-revalidate\r\n"
        "Pragma: no-cache\r\nAccess-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        kMjpegBoundary);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(header) ||
        !send_all(fd, header, static_cast<size_t>(length))) {
        close(fd);
        return;
    }

    std::lock_guard<std::mutex> clients_lock(stream->clients_mutex);
    std::lock_guard<std::mutex> stream_lock(stream->mutex);
    if (!stream->enabled) {
        close(fd);
        return;
    }
    stream->clients.push_back(fd);
}

void http_server_worker(camera_net_backend *backend)
{
    for (;;) {
        const int fd = accept(backend->listen_fd, nullptr, nullptr);
        if (fd >= 0) {
            handle_http_client(backend, fd);
            continue;
        }
        if (errno == EINTR)
            continue;
        if (backend->stop_server.load())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

int start_http_server(camera_net_backend *backend)
{
    std::lock_guard<std::mutex> lock(backend->server_mutex);
    if (backend->server_running)
        return CAMERA_NET_OK;

    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return CAMERA_NET_ERR_HTTP;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(backend->config.port);
    if (bind(fd, reinterpret_cast<struct sockaddr *>(&address),
             sizeof(address)) < 0 || listen(fd, 16) < 0) {
        close(fd);
        return CAMERA_NET_ERR_HTTP;
    }

    backend->listen_fd = fd;
    backend->stop_server.store(false);
    try {
        backend->server_thread = std::thread(http_server_worker, backend);
    } catch (...) {
        close(fd);
        backend->listen_fd = -1;
        return CAMERA_NET_ERR_NOMEM;
    }
    backend->server_running = true;
    return CAMERA_NET_OK;
}

void stop_http_server(camera_net_backend *backend)
{
    std::thread server_thread;
    {
        std::lock_guard<std::mutex> lock(backend->server_mutex);
        if (!backend->server_running)
            return;
        backend->stop_server.store(true);
        if (backend->listen_fd >= 0) {
            shutdown(backend->listen_fd, SHUT_RDWR);
            close(backend->listen_fd);
            backend->listen_fd = -1;
        }
        server_thread = std::move(backend->server_thread);
        backend->server_running = false;
    }
    if (server_thread.joinable())
        server_thread.join();
}

bool any_stream_enabled(camera_net_backend *backend)
{
    for (CameraNetStream &stream : backend->streams) {
        std::lock_guard<std::mutex> lock(stream.mutex);
        if (stream.enabled)
            return true;
    }
    return false;
}

void reset_stream(CameraNetStream *stream)
{
    stream->source_camera_id = stream->camera_id;
    stream->last_error = CAMERA_NET_OK;
    stream->last_mpp_error = MPP_OK;
    stream->last_socket_errno = 0;
    stream->last_sequence = 0;
    stream->frames_submitted = 0;
    stream->frames_encoded = 0;
    stream->frames_sent = 0;
    stream->queue_drops = 0;
    stream->encode_errors = 0;
    stream->client_disconnects = 0;
    stream->http_errors = 0;
    stream->jpeg_bytes = 0;
    stream->queue.clear();
    stream->stop_worker = false;
    stream->pacing_started = false;
    stream->next_emit = std::chrono::steady_clock::time_point{};
}

}  // namespace

extern "C" {

void camera_net_default_config(camera_net_config_t *config)
{
    if (!config)
        return;
    config->width = CAMERA_NET_WIDTH;
    config->height = CAMERA_NET_HEIGHT;
    config->fps = CAMERA_NET_FPS;
    config->jpeg_quality = 75;
    config->port = CAMERA_NET_PORT;
    config->path[0] = "/cam0";
    config->path[1] = "/cam1";
}

int camera_net_create(const camera_net_config_t *config,
                      camera_net_backend_t **backend_out)
{
    if (!config || !backend_out || !config->width || !config->height ||
        !config->fps || !config->port || config->jpeg_quality < 1 ||
        config->jpeg_quality > 99) {
        return CAMERA_NET_ERR_ARGUMENT;
    }
    if (config->width != CAMERA_NET_WIDTH ||
        config->height != CAMERA_NET_HEIGHT || config->fps > 60) {
        return CAMERA_NET_ERR_UNSUPPORTED;
    }
    for (int camera_id = 0; camera_id < CAMERA_NET_CAMERA_COUNT; ++camera_id) {
        if (!config->path[camera_id] || config->path[camera_id][0] != '/')
            return CAMERA_NET_ERR_ARGUMENT;
    }

    camera_net_backend *backend = new (std::nothrow) camera_net_backend();
    if (!backend)
        return CAMERA_NET_ERR_NOMEM;
    backend->config = *config;
    for (int camera_id = 0; camera_id < CAMERA_NET_CAMERA_COUNT; ++camera_id) {
        CameraNetStream &stream = backend->streams[camera_id];
        stream.backend = backend;
        stream.camera_id = camera_id;
        stream.path = config->path[camera_id];
        backend->config.path[camera_id] = stream.path.c_str();
    }
    *backend_out = backend;
    return CAMERA_NET_OK;
}

int camera_net_start(camera_net_backend_t *backend, int camera_id)
{
    if (!backend || !valid_camera_id(camera_id))
        return CAMERA_NET_ERR_ARGUMENT;
    CameraNetStream &stream = backend->streams[camera_id];
    {
        std::lock_guard<std::mutex> lock(stream.mutex);
        if (stream.enabled || stream.worker.joinable())
            return CAMERA_NET_ERR_STATE;
        reset_stream(&stream);
    }

    int result = stream.encoder.initialize(
        backend->config.width, backend->config.height, backend->config.fps,
        backend->config.jpeg_quality);
    if (result != MPP_OK) {
        stream.encoder.shutdown();
        std::lock_guard<std::mutex> lock(stream.mutex);
        stream.last_error = CAMERA_NET_ERR_MPP;
        stream.last_mpp_error = result;
        return CAMERA_NET_ERR_MPP;
    }
    result = start_http_server(backend);
    if (result != CAMERA_NET_OK) {
        const int socket_error = errno;
        stream.encoder.shutdown();
        std::lock_guard<std::mutex> lock(stream.mutex);
        stream.last_error = result;
        stream.last_socket_errno = socket_error;
        return result;
    }

    {
        std::lock_guard<std::mutex> lock(stream.mutex);
        stream.enabled = true;
        try {
            stream.worker = std::thread(network_worker, &stream);
        } catch (...) {
            stream.enabled = false;
            stream.last_error = CAMERA_NET_ERR_NOMEM;
        }
    }
    if (!stream.worker.joinable()) {
        stream.encoder.shutdown();
        if (!any_stream_enabled(backend))
            stop_http_server(backend);
        return CAMERA_NET_ERR_NOMEM;
    }
    return CAMERA_NET_OK;
}

int camera_net_stop_camera(camera_net_backend_t *backend, int camera_id)
{
    if (!backend || !valid_camera_id(camera_id))
        return CAMERA_NET_ERR_ARGUMENT;
    CameraNetStream &stream = backend->streams[camera_id];
    {
        std::lock_guard<std::mutex> lock(stream.mutex);
        if (!stream.enabled && !stream.worker.joinable())
            return CAMERA_NET_ERR_STATE;
        stream.enabled = false;
        stream.stop_worker = true;
        stream.queue.clear();
    }
    stream.condition.notify_all();
    close_clients(&stream);
    if (stream.worker.joinable())
        stream.worker.join();
    stream.encoder.shutdown();
    {
        std::lock_guard<std::mutex> lock(stream.mutex);
        stream.stop_worker = false;
    }
    if (!any_stream_enabled(backend))
        stop_http_server(backend);
    return CAMERA_NET_OK;
}

int camera_net_stop(camera_net_backend_t *backend)
{
    if (!backend)
        return CAMERA_NET_ERR_ARGUMENT;
    bool stopped = false;
    for (int camera_id = 0; camera_id < CAMERA_NET_CAMERA_COUNT; ++camera_id) {
        const int result = camera_net_stop_camera(backend, camera_id);
        if (result == CAMERA_NET_OK)
            stopped = true;
        else if (result != CAMERA_NET_ERR_STATE)
            return result;
    }
    return stopped ? CAMERA_NET_OK : CAMERA_NET_ERR_STATE;
}

int camera_net_submit_nv12(camera_net_backend_t *backend, int camera_id,
                           const void *plane0, size_t plane0_size,
                           const void *plane1, size_t plane1_size,
                           uint32_t sequence)
{
    if (!backend || !plane0 || !plane1 || !valid_camera_id(camera_id))
        return CAMERA_NET_ERR_ARGUMENT;
    CameraNetStream &stream = backend->streams[camera_id];
    const size_t y_size = static_cast<size_t>(backend->config.width) *
                          backend->config.height;
    const size_t uv_size = y_size / 2;
    if (plane0_size < y_size || plane1_size < uv_size)
        return CAMERA_NET_ERR_ARGUMENT;

    {
        std::lock_guard<std::mutex> lock(stream.mutex);
        if (!stream.enabled)
            return CAMERA_NET_ERR_STATE;
        Nv12Frame frame;
        try {
            frame.bytes.resize(y_size + uv_size);
        } catch (...) {
            stream.last_error = CAMERA_NET_ERR_NOMEM;
            return CAMERA_NET_ERR_NOMEM;
        }
        std::memcpy(frame.bytes.data(), plane0, y_size);
        std::memcpy(frame.bytes.data() + y_size, plane1, uv_size);
        frame.sequence = sequence;
        stream.frames_submitted++;
        if (stream.queue.size() >= kQueueDepth) {
            stream.queue.pop_front();
            stream.queue_drops++;
        }
        stream.queue.emplace_back(std::move(frame));
        stream.last_sequence = sequence;
    }
    stream.condition.notify_one();
    return CAMERA_NET_OK;
}

int camera_net_get_status(camera_net_backend_t *backend,
                          camera_net_status_t *status)
{
    return camera_net_get_camera_status(backend, 0, status);
}

int camera_net_get_camera_status(camera_net_backend_t *backend, int camera_id,
                                 camera_net_status_t *status)
{
    if (!backend || !status || !valid_camera_id(camera_id))
        return CAMERA_NET_ERR_ARGUMENT;
    CameraNetStream &stream = backend->streams[camera_id];
    {
        std::lock_guard<std::mutex> lock(stream.mutex);
        status->camera_id = camera_id;
        status->enabled = stream.enabled;
        status->source_camera_id = stream.source_camera_id;
        status->last_error = stream.last_error;
        status->last_mpp_error = stream.last_mpp_error;
        status->last_socket_errno = stream.last_socket_errno;
        status->width = backend->config.width;
        status->height = backend->config.height;
        status->fps = backend->config.fps;
        status->jpeg_quality = backend->config.jpeg_quality;
        status->port = backend->config.port;
        status->path = stream.path.c_str();
        status->last_sequence = stream.last_sequence;
        status->queue_pending = static_cast<uint32_t>(stream.queue.size());
        status->frames_submitted = stream.frames_submitted;
        status->frames_encoded = stream.frames_encoded;
        status->frames_sent = stream.frames_sent;
        status->queue_drops = stream.queue_drops;
        status->encode_errors = stream.encode_errors;
        status->client_disconnects = stream.client_disconnects;
        status->http_errors = stream.http_errors;
        status->jpeg_bytes = stream.jpeg_bytes;
    }
    {
        std::lock_guard<std::mutex> lock(stream.clients_mutex);
        status->connected_clients =
            static_cast<uint32_t>(stream.clients.size());
    }
    {
        std::lock_guard<std::mutex> lock(backend->server_mutex);
        status->server_running = backend->server_running;
    }
    return CAMERA_NET_OK;
}

void camera_net_destroy(camera_net_backend_t *backend)
{
    if (!backend)
        return;
    camera_net_stop(backend);
    stop_http_server(backend);
    delete backend;
}

const char *camera_net_strerror(int result)
{
    switch (result) {
    case CAMERA_NET_OK: return "ok";
    case CAMERA_NET_ERR_ARGUMENT: return "invalid argument";
    case CAMERA_NET_ERR_STATE: return "invalid state";
    case CAMERA_NET_ERR_MPP: return "MPP JPEG encoder error";
    case CAMERA_NET_ERR_HTTP: return "HTTP server error";
    case CAMERA_NET_ERR_NOMEM: return "out of memory";
    case CAMERA_NET_ERR_UNSUPPORTED: return "unsupported configuration";
    default: return "unknown error";
    }
}

}  // extern "C"
