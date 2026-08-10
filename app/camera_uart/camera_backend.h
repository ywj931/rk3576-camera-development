#ifndef CAMERA_BACKEND_H
#define CAMERA_BACKEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAMERA_BACKEND_CAMERA_COUNT 2
#define CAMERA_BACKEND_NAME_MAX 64
#define CAMERA_BACKEND_PATH_MAX 256

typedef struct camera_backend camera_backend_t;

enum camera_backend_result {
    CAMERA_BACKEND_OK = 0,
    CAMERA_BACKEND_ERR_ARGUMENT = -1,
    CAMERA_BACKEND_ERR_RANGE = -2,
    CAMERA_BACKEND_ERR_IO = -3,
    CAMERA_BACKEND_ERR_AIQ = -4,
    CAMERA_BACKEND_ERR_NOT_READY = -5,
    CAMERA_BACKEND_ERR_SENSOR_MISMATCH = -6,
    CAMERA_BACKEND_ERR_VERIFY = -7,
};

typedef struct camera_backend_config {
    uint32_t width;
    uint32_t height;
    const char *iq_dir[CAMERA_BACKEND_CAMERA_COUNT];
    const char *expected_sensor[CAMERA_BACKEND_CAMERA_COUNT];
    const char *params_device[CAMERA_BACKEND_CAMERA_COUNT];
    /* Optional override; otherwise discovered from the sensor entity name. */
    const char *sensor_device[CAMERA_BACKEND_CAMERA_COUNT];
} camera_backend_config_t;

typedef struct camera_backend_status {
    int camera_id;
    int online;
    int started;
    int manual_mode;
    int query_valid;
    int converged;
    int iso;
    int aiq_iso;
    int iso_estimated;
    int manual_settings_verified;
    int last_aiq_error;
    uint32_t exposure_us;
    uint32_t gain_x1000;
    uint32_t digital_gain_x1000;
    uint32_t isp_dgain_x1000;
    uint32_t fps_x1000;
    uint32_t requested_exposure_us;
    uint32_t requested_gain_x1000;
    uint32_t requested_iso;
    uint32_t requested_fps_x1000;
    int xvs_config_valid;
    uint32_t xvs_input_thin;
    float mean_luma;
    char sensor_name[CAMERA_BACKEND_NAME_MAX];
    char iq_dir[CAMERA_BACKEND_PATH_MAX];
    char sensor_device[CAMERA_BACKEND_PATH_MAX];
} camera_backend_status_t;

/* Fills the RK3576/dual-IMX586 defaults used by camera_aiq_test. */
void camera_backend_default_config(camera_backend_config_t *config);

/*
 * Owns both RKAIQ contexts. Do not run rkaiq_3A_server at the same time.
 * The V4L2 capture processes remain separate and can keep using the current
 * dual-capture command line.
 */
int camera_backend_create(const camera_backend_config_t *config,
                          camera_backend_t **backend_out);
void camera_backend_destroy(camera_backend_t *backend);

int camera_backend_set_auto(camera_backend_t *backend, int camera_id);
/* Sets exposure while preserving the current measured analog gain. */
int camera_backend_set_exposure(camera_backend_t *backend, int camera_id,
                                uint32_t exposure_us);
/* Sets analog gain while preserving the current measured exposure time. */
int camera_backend_set_gain(camera_backend_t *backend, int camera_id,
                            uint32_t gain_x1000);
/* ISO is mapped to total gain using the RKAIQ base sensitivity ISO 50. */
int camera_backend_set_iso(camera_backend_t *backend, int camera_id,
                           uint32_t iso);
int camera_backend_set_fps(camera_backend_t *backend, int camera_id,
                           uint32_t fps);
/* External XVS must remain at 4 Hz; each sensor independently selects 4/2 Hz. */
int camera_backend_set_xvs_fps(camera_backend_t *backend, int camera_id,
                               uint32_t fps);
int camera_backend_get_xvs_fps(camera_backend_t *backend, int camera_id,
                               uint32_t *fps);
int camera_backend_get_status(camera_backend_t *backend, int camera_id,
                              camera_backend_status_t *status);

const char *camera_backend_strerror(int result);

#ifdef __cplusplus
}
#endif

#endif
