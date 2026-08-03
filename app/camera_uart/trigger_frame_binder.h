#ifndef TRIGGER_FRAME_BINDER_H
#define TRIGGER_FRAME_BINDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRIGGER_FRAME_BINDER_PATH_MAX 512
#define TRIGGER_FRAME_BINDER_SOURCE_MAX 24

typedef struct trigger_frame_binder trigger_frame_binder_t;

enum trigger_frame_binder_result {
    TRIGGER_FRAME_BINDER_OK = 0,
    TRIGGER_FRAME_BINDER_ERR_ARGUMENT = -300,
    TRIGGER_FRAME_BINDER_ERR_IO = -301,
};

typedef struct trigger_frame_binder_config {
    uint32_t max_pending_triggers;
    uint32_t completed_history_depth;
} trigger_frame_binder_config_t;

typedef struct trigger_frame_event {
    int camera_id;
    uint32_t sequence;
    uint32_t buffer_flags;
    uint64_t v4l2_timestamp_ns;
    uint64_t realtime_dequeue_ns;
} trigger_frame_event_t;

typedef struct trigger_frame_trigger {
    uint64_t trigger_id;
    uint64_t monotonic_ns;
    uint64_t realtime_ns;
    uint64_t pps_id;
    uint64_t timer_tick;
    int utc_valid;
    int monotonic_is_uart_arrival;
    const char *source;
} trigger_frame_trigger_t;

typedef struct trigger_frame_binding {
    int valid;
    uint64_t trigger_id;
    uint64_t trigger_monotonic_ns;
    uint64_t trigger_realtime_ns;
    uint64_t pps_id;
    uint64_t trigger_timer_tick;
    int utc_valid;
    int monotonic_is_uart_arrival;
    char source[TRIGGER_FRAME_BINDER_SOURCE_MAX];
    uint32_t cam0_sequence;
    uint32_t cam0_buffer_flags;
    uint64_t cam0_timestamp_ns;
    uint64_t cam0_realtime_dequeue_ns;
    uint32_t cam1_sequence;
    uint32_t cam1_buffer_flags;
    uint64_t cam1_timestamp_ns;
    uint64_t cam1_realtime_dequeue_ns;
    int64_t cam0_delay_ns;
    int64_t cam1_delay_ns;
    uint64_t frame_delta_ns;
} trigger_frame_binding_t;

/*
 * A camera-local view of a trigger/frame association. Unlike
 * trigger_frame_binding_t, this record is available as soon as one camera
 * frame has arrived; it does not wait for the peer camera. The photo path
 * uses it immediately after VIDIOC_DQBUF so the image pixels and metadata
 * keep the same v4l2 sequence number.
 */
typedef struct trigger_frame_match {
    int valid;
    int pair_complete;
    int camera_id;
    uint64_t trigger_id;
    uint64_t trigger_monotonic_ns;
    uint64_t trigger_realtime_ns;
    uint64_t pps_id;
    uint64_t trigger_timer_tick;
    int utc_valid;
    int monotonic_is_uart_arrival;
    char source[TRIGGER_FRAME_BINDER_SOURCE_MAX];
    uint32_t sequence;
    uint32_t buffer_flags;
    uint64_t v4l2_timestamp_ns;
    uint64_t realtime_dequeue_ns;
    int64_t trigger_to_frame_ns;
} trigger_frame_match_t;

typedef struct trigger_frame_binder_status {
    uint64_t triggers_received;
    uint64_t triggers_ignored;
    uint64_t trigger_id_gaps;
    uint64_t duplicate_triggers;
    uint64_t pending_triggers;
    uint64_t pending_trigger_overflows;
    uint64_t frames_received[2];
    uint64_t frames_bound[2];
    uint64_t frames_without_trigger[2];
    uint64_t complete_pairs;
    uint64_t last_trigger_id;
    uint64_t last_completed_trigger_id;
    uint64_t last_frame_delta_ns;
    char csv_path[TRIGGER_FRAME_BINDER_PATH_MAX];
} trigger_frame_binder_status_t;

void trigger_frame_binder_default_config(trigger_frame_binder_config_t *config);
int trigger_frame_binder_create(const trigger_frame_binder_config_t *config,
                                trigger_frame_binder_t **binder_out);
void trigger_frame_binder_destroy(trigger_frame_binder_t *binder);

/* Discard any in-flight capture epoch before the next measurement. */
int trigger_frame_binder_reset(trigger_frame_binder_t *binder,
                               uint32_t triggers_to_ignore);
int trigger_frame_binder_set_csv_path(trigger_frame_binder_t *binder,
                                      const char *path);

/* All trigger timestamps must use CLOCK_MONOTONIC and CLOCK_REALTIME. */
int trigger_frame_binder_on_trigger(trigger_frame_binder_t *binder,
                                    uint64_t trigger_id,
                                    uint64_t monotonic_ns,
                                    uint64_t realtime_ns,
                                    const char *source);
int trigger_frame_binder_on_trigger_ex(trigger_frame_binder_t *binder,
                                       const trigger_frame_trigger_t *trigger);
int trigger_frame_binder_on_frame(trigger_frame_binder_t *binder,
                                  const trigger_frame_event_t *frame);
int trigger_frame_binder_get_status(trigger_frame_binder_t *binder,
                                    trigger_frame_binder_status_t *status);
int trigger_frame_binder_get_last_binding(
    trigger_frame_binder_t *binder, trigger_frame_binding_t *binding);
int trigger_frame_binder_find_frame(trigger_frame_binder_t *binder,
                                    int camera_id, uint32_t sequence,
                                    trigger_frame_match_t *match);

const char *trigger_frame_binder_strerror(int result);

#ifdef __cplusplus
}
#endif

#endif
