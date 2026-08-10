#ifndef CAMERA_UVC_BACKEND_H
#define CAMERA_UVC_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAMERA_UVC_WIDTH 4000
#define CAMERA_UVC_HEIGHT 3000
#define CAMERA_UVC_FPS 4
#define CAMERA_UVC_CAMERA_COUNT 2

typedef struct camera_uvc_backend camera_uvc_backend_t;

enum camera_uvc_result {
    CAMERA_UVC_OK = 0,
    CAMERA_UVC_ERR_ARGUMENT = -200,
    CAMERA_UVC_ERR_STATE = -201,
    CAMERA_UVC_ERR_NODE = -202,
    CAMERA_UVC_ERR_MPP = -203,
    CAMERA_UVC_ERR_NOMEM = -204,
    CAMERA_UVC_ERR_UNSUPPORTED = -205,
};

typedef struct camera_uvc_config {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t jpeg_quality;
} camera_uvc_config_t;

typedef struct camera_uvc_status {
    int enabled;
    int host_streaming;
    int source_camera_id;
    int last_error;
    int last_mpp_error;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t negotiated_width;
    uint32_t negotiated_height;
    uint32_t negotiated_fps;
    uint32_t negotiated_fcc;
    uint32_t last_sequence;
    uint32_t queue_pending;
    uint64_t frames_submitted;
    uint64_t frames_encoded;
    uint64_t frames_sent;
    uint64_t frames_skipped_no_host;
    uint64_t frames_rate_limited;
    uint64_t queue_drops;
    uint64_t encode_errors;
    uint64_t jpeg_bytes;
} camera_uvc_status_t;

void camera_uvc_default_config(camera_uvc_config_t *config);
int camera_uvc_create(const camera_uvc_config_t *config,
                      camera_uvc_backend_t **backend_out);
void camera_uvc_destroy(camera_uvc_backend_t *backend);

int camera_uvc_start(camera_uvc_backend_t *backend, int camera_id);
int camera_uvc_start_all(camera_uvc_backend_t *backend);
int camera_uvc_stop_camera(camera_uvc_backend_t *backend, int camera_id);
/* Stops both video producers while keeping UVC control and USB Gadget alive. */
int camera_uvc_stop(camera_uvc_backend_t *backend);
int camera_uvc_set_source_fps(camera_uvc_backend_t *backend, int camera_id,
                              uint32_t fps);
int camera_uvc_submit_nv12(camera_uvc_backend_t *backend, int camera_id,
                           const void *plane0, size_t plane0_size,
                           const void *plane1, size_t plane1_size,
                           uint32_t sequence);
int camera_uvc_get_status(camera_uvc_backend_t *backend, int camera_id,
                          camera_uvc_status_t *status);

const char *camera_uvc_strerror(int result);

#ifdef __cplusplus
}
#endif

#endif
