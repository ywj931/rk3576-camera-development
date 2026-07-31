#ifndef XVS_UART_CONTROLLER_H
#define XVS_UART_CONTROLLER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XVS_UART_PATH_MAX 256

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

int xvs_uart_protocol_self_test(void);
const char *xvs_uart_strerror(int result);

#ifdef __cplusplus
}
#endif

#endif
