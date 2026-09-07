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
#define CAMERA_PHOTO_TRANSFER_HOST_MAX 256
#define CAMERA_PHOTO_TRANSFER_ERROR_MAX 256

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
    CAMERA_PHOTO_ERR_QUEUE_FULL = -407,
};

typedef struct camera_photo_config {
    uint32_t width;
    uint32_t height;
    uint32_t camera_count;
    uint32_t jpeg_quality;
    uint32_t queue_depth;
    uint32_t stitch_pending_depth;
    uint64_t stitch_pair_tolerance_ns;
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
    int white_balance_valid;
    int white_balance_auto;
    int white_balance_converged;
    uint32_t white_balance_cct;
    uint32_t wb_r_gain_x1000;
    uint32_t wb_gr_gain_x1000;
    uint32_t wb_gb_gain_x1000;
    uint32_t wb_b_gain_x1000;
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

typedef struct camera_photo_stitch_status {
    int enabled;
    int processing;
    int last_error;
    int last_mpp_error;
    int last_errno;
    uint32_t width;
    uint32_t height;
    uint32_t jpeg_quality;
    uint32_t queue_pending;
    uint32_t pending_cam0;
    uint32_t pending_cam1;
    uint32_t assembling;
    uint64_t frames_submitted_cam0;
    uint64_t frames_submitted_cam1;
    uint64_t pairs_matched;
    uint64_t photos_saved;
    uint64_t unmatched_drops_cam0;
    uint64_t unmatched_drops_cam1;
    uint64_t queue_drops;
    uint64_t invalid_metadata;
    uint64_t encode_errors;
    uint64_t exif_errors;
    uint64_t write_errors;
    uint64_t jpeg_bytes;
    uint64_t last_pair_id;
    uint32_t last_top_frame_id;
    uint32_t last_bottom_frame_id;
    int64_t last_frame_delta_ns;
    uint64_t last_composite_capture_realtime_ns;
    uint64_t last_top_frame_realtime_ns;
    uint64_t last_bottom_frame_realtime_ns;
    uint32_t last_top_exposure_us;
    uint32_t last_bottom_exposure_us;
    uint32_t last_top_iso;
    uint32_t last_bottom_iso;
    int last_composite_utc_valid;
    char output_dir[CAMERA_PHOTO_PATH_MAX];
    char metadata_csv[CAMERA_PHOTO_PATH_MAX];
    char last_photo[CAMERA_PHOTO_PATH_MAX];
} camera_photo_stitch_status_t;

typedef struct camera_photo_stitch_transfer_status {
    int enabled;
    int processing;
    uint16_t port;
    uint64_t session_id;
    uint64_t enqueued;
    uint64_t delivered;
    uint64_t retries;
    uint64_t failed;
    uint64_t queue_rejected;
    uint64_t delete_errors;
    uint64_t queue_pending;
    uint64_t backlog;
    uint64_t backlog_peak;
    uint64_t deferred;
    uint64_t recovered;
    uint64_t spool_recovered;
    uint64_t orphan_recovered;
    uint64_t orphan_unrecoverable;
    uint64_t spool_recovery_errors;
    uint64_t bytes;
    int last_result;
    int last_errno;
    char host[CAMERA_PHOTO_TRANSFER_HOST_MAX];
    char last_error[CAMERA_PHOTO_TRANSFER_ERROR_MAX];
    char last_path[CAMERA_PHOTO_PATH_MAX];
} camera_photo_stitch_transfer_status_t;

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

int camera_photo_stitch_start(camera_photo_backend_t *backend,
                              const char *output_dir);
int camera_photo_stitch_stop(camera_photo_backend_t *backend);
int camera_photo_stitch_is_enabled(camera_photo_backend_t *backend);
int camera_photo_stitch_submit_nv12(
    camera_photo_backend_t *backend, int camera_id,
    const void *plane0, size_t plane0_size,
    const void *plane1, size_t plane1_size,
    const camera_photo_metadata_t *metadata);
int camera_photo_stitch_get_status(camera_photo_backend_t *backend,
                                   camera_photo_stitch_status_t *status);

int camera_photo_stitch_transfer_start(camera_photo_backend_t *backend,
                                       const char *output_dir,
                                       const char *host, uint16_t port);
int camera_photo_stitch_transfer_stop(camera_photo_backend_t *backend);
int camera_photo_stitch_transfer_get_status(
    camera_photo_backend_t *backend,
    camera_photo_stitch_transfer_status_t *status);

const char *camera_photo_strerror(int result);

#ifdef __cplusplus
}
#endif

#endif
