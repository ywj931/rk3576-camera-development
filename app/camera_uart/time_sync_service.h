#ifndef TIME_SYNC_SERVICE_H
#define TIME_SYNC_SERVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct time_sync_service time_sync_service_t;

enum time_sync_result {
    TIME_SYNC_OK = 0,
    TIME_SYNC_ERR_ARGUMENT = -400,
    TIME_SYNC_ERR_ALLOCATE = -401,
    TIME_SYNC_ERR_SEQUENCE = -402,
    TIME_SYNC_ERR_NMEA = -403,
    TIME_SYNC_ERR_NOT_LOCKED = -404,
    TIME_SYNC_ERR_RANGE = -405,
};

enum time_sync_state {
    TIME_SYNC_STATE_UNLOCKED = 0,
    TIME_SYNC_STATE_PPS_ONLY = 1,
    TIME_SYNC_STATE_UTC_LOCKED = 2,
    TIME_SYNC_STATE_HOLDOVER = 3,
};

typedef struct time_sync_config {
    uint64_t timer_frequency_hz;
    uint32_t max_holdover_pps;
} time_sync_config_t;

typedef struct time_sync_resolution {
    int valid;
    enum time_sync_state state;
    uint64_t pps_id;
    uint64_t timer_tick;
    uint64_t utc_ns;
} time_sync_resolution_t;

typedef struct time_sync_status {
    enum time_sync_state state;
    int utc_valid;
    uint64_t timer_frequency_hz;
    uint32_t max_holdover_pps;
    uint32_t holdover_age_pps;
    uint64_t pps_events;
    uint64_t rmc_events;
    uint64_t invalid_rmc_events;
    uint64_t resolved_events;
    uint64_t unresolved_events;
    uint64_t last_pps_id;
    uint64_t last_pps_tick;
    uint64_t last_rmc_pps_id;
    int64_t last_rmc_utc_sec;
    uint64_t reference_pps_id;
    uint64_t reference_tick;
    uint64_t reference_utc_ns;
} time_sync_status_t;

void time_sync_default_config(time_sync_config_t *config);
int time_sync_create(const time_sync_config_t *config,
                     time_sync_service_t **service_out);
void time_sync_destroy(time_sync_service_t *service);
int time_sync_reset(time_sync_service_t *service);

/* RMC for pps_id arrives after that PPS; pps_id + 1 locks to utc_sec + 1. */
int time_sync_on_pps(time_sync_service_t *service, uint64_t pps_id,
                     uint64_t timer_tick);
int time_sync_on_rmc_utc(time_sync_service_t *service, uint64_t pps_id,
                         int64_t utc_sec, int valid);
int time_sync_on_nmea_rmc(time_sync_service_t *service, uint64_t pps_id,
                          const char *sentence);
int time_sync_resolve_tick(time_sync_service_t *service, uint64_t pps_id,
                           uint64_t timer_tick,
                           time_sync_resolution_t *resolution);
int time_sync_get_status(time_sync_service_t *service,
                         time_sync_status_t *status);

int time_sync_parse_nmea_rmc(const char *sentence, int64_t *utc_sec,
                             int *valid);
const char *time_sync_state_name(enum time_sync_state state);
const char *time_sync_strerror(int result);

#ifdef __cplusplus
}
#endif

#endif
