#ifndef CAMERA_PHOTO_BACKEND_H
#define CAMERA_PHOTO_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAMERA_PHOTO_CAMERA_COUNT 2
#define CAMERA_PHOTO_PATH_MAX 512
#define CAMERA_PHOTO_SOURCE_MAX 32

typedef struct camera_photo_backend camera_photo_backend_t;

enum camera_photo_result {
    CAMERA_PHOTO_OK = 0,
    CAMERA_PHOTO_ERR_ARGUMENT = -400,
    CAMERA_PHOTO_ERR_RANGE = -401,
    CAMERA_PHOTO_ERR_IO = -402,
    CAMERA_PHOTO_ERR_ALREADY_RUNNING = -403,
    CAMERA_PHOTO_ERR_NOT_RUNNING = -404,
    CAMERA_PHOTO_ERR_MPP = -405,
    CAMERA_PHOTO_ERR_EXIF = -406,
};

typedef struct camera_photo_config {
    uint32_t width;
    uint32_t height;
    uint32_t jpeg_quality;
    uint32_t queue_depth;
} camera_photo_config_t;

typedef struct camera_photo_metadata {
    int camera_id;
    uint32_t frame_id;
    uint64_t trigger_id;
    uint64_t trigger_monotonic_ns;
    uint64_t trigger_realtime_ns;
    uint64_t pps_id;
    uint64_t trigger_timer_tick;
    uint64_t frame_monotonic_ns;
    uint64_t frame_realtime_ns;
    uint64_t exposure_start_realtime_ns;
    uint64_t exposure_center_realtime_ns;
    int64_t sensor_response_offset_ns;
    int64_t trigger_to_frame_ns;
    uint32_t exposure_us;
    uint32_t gain_x1000;
    uint32_t iso;
    int utc_valid;
    int trigger_monotonic_is_uart_arrival;
    int iso_estimated;
    char trigger_source[CAMERA_PHOTO_SOURCE_MAX];
    char exposure_source[CAMERA_PHOTO_SOURCE_MAX];
} camera_photo_metadata_t;

typedef struct camera_photo_status {
    int camera_id;
    int enabled;
    int processing;
    int last_error;
    int last_mpp_error;
    int last_errno;
    uint32_t width;
    uint32_t height;
    uint32_t jpeg_quality;
    uint32_t queue_pending;
    int64_t sensor_response_offset_ns;
    uint64_t frames_submitted;
    uint64_t photos_saved;
    uint64_t queue_drops;
    uint64_t frames_without_trigger;
    uint64_t invalid_metadata;
    uint64_t encode_errors;
    uint64_t exif_errors;
    uint64_t write_errors;
    uint64_t jpeg_bytes;
    uint32_t last_frame_id;
    uint64_t last_trigger_id;
    char output_dir[CAMERA_PHOTO_PATH_MAX];
    char metadata_csv[CAMERA_PHOTO_PATH_MAX];
    char last_photo[CAMERA_PHOTO_PATH_MAX];
} camera_photo_status_t;

void camera_photo_default_config(camera_photo_config_t *config);
int camera_photo_create(const camera_photo_config_t *config,
                        camera_photo_backend_t **backend_out);
void camera_photo_destroy(camera_photo_backend_t *backend);

int camera_photo_start(camera_photo_backend_t *backend, int camera_id,
                       const char *output_dir);
int camera_photo_stop(camera_photo_backend_t *backend, int camera_id);
int camera_photo_is_enabled(camera_photo_backend_t *backend, int camera_id);
int camera_photo_set_response_offset(camera_photo_backend_t *backend,
                                     int camera_id, int64_t offset_ns);
int camera_photo_get_response_offset(camera_photo_backend_t *backend,
                                     int camera_id, int64_t *offset_ns);
int camera_photo_note_unbound_frame(camera_photo_backend_t *backend,
                                    int camera_id);
int camera_photo_submit_nv12(camera_photo_backend_t *backend, int camera_id,
                             const void *plane0, size_t plane0_size,
                             const void *plane1, size_t plane1_size,
                             const camera_photo_metadata_t *metadata);
int camera_photo_get_status(camera_photo_backend_t *backend, int camera_id,
                            camera_photo_status_t *status);

const char *camera_photo_strerror(int result);

#ifdef __cplusplus
}
#endif

#endif
