#ifndef TRIGGER_SIMULATOR_H
#define TRIGGER_SIMULATOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct trigger_simulator trigger_simulator_t;

typedef void (*trigger_simulator_callback_t)(uint64_t trigger_id,
                                             uint64_t monotonic_ns,
                                             uint64_t realtime_ns,
                                             void *user_data);

enum trigger_simulator_result {
    TRIGGER_SIMULATOR_OK = 0,
    TRIGGER_SIMULATOR_ERR_ARGUMENT = -320,
    TRIGGER_SIMULATOR_ERR_RUNNING = -321,
    TRIGGER_SIMULATOR_ERR_IO = -322,
};

typedef struct trigger_simulator_status {
    int running;
    uint32_t frequency_hz;
    uint32_t requested_count;
    uint64_t emitted_count;
    uint64_t last_trigger_id;
} trigger_simulator_status_t;

int trigger_simulator_create(trigger_simulator_callback_t callback,
                             void *user_data,
                             trigger_simulator_t **simulator_out);
void trigger_simulator_destroy(trigger_simulator_t *simulator);

/* count=0 requests continuous simulated trigger events. */
int trigger_simulator_start(trigger_simulator_t *simulator,
                            uint32_t frequency_hz, uint32_t count);
int trigger_simulator_stop(trigger_simulator_t *simulator);
int trigger_simulator_get_status(trigger_simulator_t *simulator,
                                 trigger_simulator_status_t *status);
const char *trigger_simulator_strerror(int result);

#ifdef __cplusplus
}
#endif

#endif
