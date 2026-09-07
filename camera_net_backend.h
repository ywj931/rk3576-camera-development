#ifndef CAMERA_NET_BACKEND_H
#define CAMERA_NET_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAMERA_NET_WIDTH 4000
#define CAMERA_NET_HEIGHT 3000
#define CAMERA_NET_FPS 10
#define CAMERA_NET_PORT 8080
#define CAMERA_NET_CAMERA_COUNT 2

typedef struct camera_net_backend camera_net_backend_t;

enum camera_net_result {
    CAMERA_NET_OK = 0,
    CAMERA_NET_ERR_ARGUMENT = -300,
    CAMERA_NET_ERR_STATE = -301,
    CAMERA_NET_ERR_MPP = -302,
    CAMERA_NET_ERR_HTTP = -303,
    CAMERA_NET_ERR_NOMEM = -304,
    CAMERA_NET_ERR_UNSUPPORTED = -305,
};

typedef struct camera_net_config {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t jpeg_quality;
    uint16_t port;
    const char *path[CAMERA_NET_CAMERA_COUNT];
} camera_net_config_t;

typedef struct camera_net_status {
    int camera_id;
    int enabled;
    int server_running;
    int source_camera_id;
    int last_error;
    int last_mpp_error;
    int last_socket_errno;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t jpeg_quality;
    uint16_t port;
    const char *path;
    uint32_t connected_clients;
    uint32_t last_sequence;
    uint32_t queue_pending;
    uint64_t frames_submitted;
    uint64_t frames_encoded;
    uint64_t frames_sent;
    uint64_t queue_drops;
    uint64_t encode_errors;
    uint64_t client_disconnects;
    uint64_t http_errors;
    uint64_t jpeg_bytes;
} camera_net_status_t;

void camera_net_default_config(camera_net_config_t *config);
int camera_net_create(const camera_net_config_t *config,
                      camera_net_backend_t **backend_out);
void camera_net_destroy(camera_net_backend_t *backend);

int camera_net_start(camera_net_backend_t *backend, int camera_id);
int camera_net_stop_camera(camera_net_backend_t *backend, int camera_id);
int camera_net_stop(camera_net_backend_t *backend);
int camera_net_submit_nv12(camera_net_backend_t *backend, int camera_id,
                           const void *plane0, size_t plane0_size,
                           const void *plane1, size_t plane1_size,
                           uint32_t sequence);
int camera_net_get_status(camera_net_backend_t *backend,
                          camera_net_status_t *status);
int camera_net_get_camera_status(camera_net_backend_t *backend, int camera_id,
                                 camera_net_status_t *status);

const char *camera_net_strerror(int result);

#ifdef __cplusplus
}
#endif

#endif
