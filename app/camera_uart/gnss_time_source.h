#ifndef GNSS_TIME_SOURCE_H
#define GNSS_TIME_SOURCE_H

#include "time_sync_service.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gnss_time_source gnss_time_source_t;

enum gnss_time_source_result {
    GNSS_TIME_OK = 0,
    GNSS_TIME_ERR_ARGUMENT = -500,
    GNSS_TIME_ERR_ALLOCATE = -501,
    GNSS_TIME_ERR_THREAD = -502,
    GNSS_TIME_ERR_IO = -503,
    GNSS_TIME_ERR_BAUD = -504,
    GNSS_TIME_ERR_PPS = -505,
    GNSS_TIME_ERR_NOT_LOCKED = -506,
};

typedef struct gnss_time_source_config {
    const char *uart_device;
    uint32_t uart_baud;
    const char *pps_device;
    uint32_t max_rmc_delay_ms;
} gnss_time_source_config_t;

typedef struct gnss_time_source_status {
    int running;
    int uart_connected;
    int pps_connected;
    uint32_t uart_baud;
    uint32_t max_rmc_delay_ms;
    uint64_t pps_events;
    uint64_t rmc_events;
    uint64_t invalid_rmc_events;
    uint64_t unpaired_rmc_events;
    uint64_t ignored_nmea_events;
    uint64_t uart_open_errors;
    uint64_t uart_read_errors;
    uint64_t pps_open_errors;
    uint64_t pps_fetch_errors;
    uint64_t time_mapping_errors;
    uint64_t last_pps_id;
    uint32_t last_kernel_pps_sequence;
    uint64_t last_pps_monotonic_ns;
    uint64_t last_pps_local_realtime_ns;
    uint64_t last_rmc_arrival_monotonic_ns;
    uint64_t last_clock_sample_span_ns;
    int last_error;
    int last_errno;
    char uart_device[128];
    char pps_device[128];
} gnss_time_source_status_t;

void gnss_time_source_default_config(gnss_time_source_config_t *config);
int gnss_time_source_create(const gnss_time_source_config_t *config,
                            time_sync_service_t *time_sync,
                            gnss_time_source_t **source_out);
void gnss_time_source_destroy(gnss_time_source_t *source);
int gnss_time_source_start(gnss_time_source_t *source);
int gnss_time_source_stop(gnss_time_source_t *source);
int gnss_time_source_get_status(gnss_time_source_t *source,
                                gnss_time_source_status_t *status);

/* Resolve a V4L2 CLOCK_MONOTONIC timestamp against the latest GNSS PPS. */
int gnss_time_source_resolve_monotonic_ns(
    gnss_time_source_t *source, uint64_t monotonic_ns,
    time_sync_resolution_t *resolution);

/* Deterministic event entry points used by the host self-test. */
int gnss_time_source_inject_pps(gnss_time_source_t *source,
                                uint32_t kernel_sequence,
                                uint64_t local_realtime_ns,
                                uint64_t monotonic_ns);
int gnss_time_source_inject_nmea(gnss_time_source_t *source,
                                 const char *sentence,
                                 uint64_t arrival_monotonic_ns);
int gnss_time_source_self_test(char *report, size_t report_capacity);

const char *gnss_time_source_strerror(int result);

#ifdef __cplusplus
}
#endif

#endif
