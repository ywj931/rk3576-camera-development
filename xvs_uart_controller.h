#ifndef XVS_UART_CONTROLLER_H
#define XVS_UART_CONTROLLER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XVS_UART_PATH_MAX 256
#define XVS_UART_NMEA_MAX 192

typedef struct xvs_uart_controller xvs_uart_controller_t;

enum xvs_uart_result {
    XVS_UART_OK = 0,
    XVS_UART_ERR_ARGUMENT = -200,
    XVS_UART_ERR_OPEN = -201,
    XVS_UART_ERR_CONFIGURE = -202,
    XVS_UART_ERR_IO = -203,
    XVS_UART_ERR_TIMEOUT = -204,
    XVS_UART_ERR_PROTOCOL = -205,
    XVS_UART_ERR_MCU = -206,
};

typedef struct xvs_uart_status {
    int connected;
    int valid;
    char state[24];
    uint32_t frequency_millihz;
    uint32_t low_pulse_us;
    uint64_t pulse_count;
    uint64_t last_trigger_id;
    char device[XVS_UART_PATH_MAX];
} xvs_uart_status_t;

enum xvs_uart_event_type {
    XVS_UART_EVENT_PPS = 1,
    XVS_UART_EVENT_RMC = 2,
    XVS_UART_EVENT_NMEA = 3,
    XVS_UART_EVENT_XVS = 4,
};

typedef struct xvs_uart_event {
    enum xvs_uart_event_type type;
    uint64_t pps_id;
    uint64_t trigger_id;
    uint64_t timer_tick;
    int64_t utc_sec;
    int valid;
    uint64_t uart_receive_monotonic_ns;
    uint64_t uart_receive_realtime_ns;
    char nmea[XVS_UART_NMEA_MAX];
} xvs_uart_event_t;

typedef void (*xvs_uart_event_callback_t)(const xvs_uart_event_t *event,
                                          void *user_data);
typedef int (*xvs_uart_control_callback_t)(
    const char *request, char *response, size_t response_capacity,
    size_t *response_size, void *user_data);

int xvs_uart_create(const char *device, xvs_uart_controller_t **controller_out);
void xvs_uart_destroy(xvs_uart_controller_t *controller);

int xvs_uart_ping(xvs_uart_controller_t *controller);
int xvs_uart_idle(xvs_uart_controller_t *controller);
int xvs_uart_start(xvs_uart_controller_t *controller,
                   uint32_t frequency_hz, uint32_t low_pulse_us);
int xvs_uart_count(xvs_uart_controller_t *controller,
                   uint32_t frequency_hz, uint32_t low_pulse_us,
                   uint32_t pulse_count);
int xvs_uart_stop(xvs_uart_controller_t *controller);
int xvs_uart_get_status(xvs_uart_controller_t *controller,
                        xvs_uart_status_t *status);
int xvs_uart_set_event_callback(xvs_uart_controller_t *controller,
                                xvs_uart_event_callback_t callback,
                                void *user_data);
int xvs_uart_set_control_callback(xvs_uart_controller_t *controller,
                                  xvs_uart_control_callback_t callback,
                                  void *user_data);

int xvs_uart_protocol_self_test(void);
const char *xvs_uart_strerror(int result);

#ifdef __cplusplus
}
#endif

#endif
