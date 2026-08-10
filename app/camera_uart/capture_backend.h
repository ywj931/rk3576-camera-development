#ifndef CAPTURE_BACKEND_H
#define CAPTURE_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAPTURE_BACKEND_CAMERA_COUNT 2
#define CAPTURE_BACKEND_PATH_MAX 512

typedef struct capture_backend capture_backend_t;

typedef void (*capture_backend_frame_callback_t)(
    int camera_id, const void *plane0, size_t plane0_size,
    const void *plane1, size_t plane1_size, uint32_t sequence,
    void *user_data);

/* Metadata-only observer invoked after VIDIOC_DQBUF and before re-queue. */
typedef void (*capture_backend_frame_event_callback_t)(
    int camera_id, uint32_t sequence, uint32_t buffer_flags,
    uint64_t v4l2_timestamp_ns, uint64_t realtime_dequeue_ns,
    void *user_data);

enum capture_backend_result {
    CAPTURE_BACKEND_OK = 0,
    CAPTURE_BACKEND_ERR_ARGUMENT = -100,
    CAPTURE_BACKEND_ERR_RANGE = -101,
    CAPTURE_BACKEND_ERR_IO = -102,
    CAPTURE_BACKEND_ERR_ALREADY_RUNNING = -103,
    CAPTURE_BACKEND_ERR_NOT_RUNNING = -104,
    CAPTURE_BACKEND_ERR_ALREADY_SAVING = -105,
    CAPTURE_BACKEND_ERR_NOT_SAVING = -106,
};

typedef struct capture_backend_config {
    uint32_t width;
    uint32_t height;
    const char *video_device[CAPTURE_BACKEND_CAMERA_COUNT];
} capture_backend_config_t;

typedef struct capture_backend_status {
    int camera_id;
    int running;
    int saving;
    int last_errno;
    uint32_t width;
    uint32_t height;
    uint32_t fps_x1000;
    uint32_t fps_target_x1000;
    int fps_stable;
    uint32_t fps_window_frames;
    uint64_t fps_window_duration_ns;
    uint64_t frames_captured;
    uint64_t frames_dropped;
    uint64_t frames_saved;
    uint32_t save_queue_pending;
    uint64_t save_queue_dropped;
    uint64_t save_failures;
    uint64_t bytes_saved;
    int timestamp_valid;
    uint32_t last_sequence;
    uint32_t last_buffer_flags;
    uint64_t last_v4l2_timestamp_ns;
    uint64_t last_realtime_dequeue_ns;
    char video_device[CAPTURE_BACKEND_PATH_MAX];
    char output_dir[CAPTURE_BACKEND_PATH_MAX];
    char last_saved_path[CAPTURE_BACKEND_PATH_MAX];
    char metadata_path[CAPTURE_BACKEND_PATH_MAX];
} capture_backend_status_t;

typedef struct capture_backend_sync_status {
    int valid;
    uint32_t cam0_sequence;
    uint32_t cam1_sequence;
    uint32_t cam0_buffer_flags;
    uint32_t cam1_buffer_flags;
    uint64_t cam0_timestamp_ns;
    uint64_t cam1_timestamp_ns;
    uint64_t delta_ns;
} capture_backend_sync_status_t;

void capture_backend_default_config(capture_backend_config_t *config);
int capture_backend_create(const capture_backend_config_t *config,
                           capture_backend_t **backend_out);
void capture_backend_destroy(capture_backend_t *backend);

int capture_backend_start_stream(capture_backend_t *backend, int camera_id);
int capture_backend_stop_stream(capture_backend_t *backend, int camera_id);
int capture_backend_set_frame_callback(
    capture_backend_t *backend, int camera_id,
    capture_backend_frame_callback_t callback, void *user_data);
int capture_backend_set_frame_event_callback(
    capture_backend_t *backend, int camera_id,
    capture_backend_frame_event_callback_t callback, void *user_data);

int capture_backend_start_save(capture_backend_t *backend, int camera_id,
                               const char *output_dir);
int capture_backend_stop_save(capture_backend_t *backend, int camera_id);
int capture_backend_get_status(capture_backend_t *backend, int camera_id,
                               capture_backend_status_t *status);
/* Starts a fresh sliding FPS measurement after changing sensor frame rate. */
int capture_backend_reset_fps_window(capture_backend_t *backend,
                                     int camera_id, uint32_t target_fps);
int capture_backend_get_sync_status(capture_backend_t *backend,
                                    capture_backend_sync_status_t *status);

const char *capture_backend_strerror(int result);

#ifdef __cplusplus
}
#endif

#endif
