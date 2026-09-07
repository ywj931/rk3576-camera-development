#ifndef IMX586_V4L2_METADATA_H
#define IMX586_V4L2_METADATA_H

#include <cstdint>
#include <string>

namespace imx586_v4l2 {

struct ControlSnapshot {
    uint32_t exposure_lines_x16 = 0;
    uint32_t active_width = 0;
    uint32_t horizontal_blanking = 0;
    uint64_t pixel_rate = 0;
    uint32_t gain_code = 0;
};

struct Metadata {
    uint32_t exposure_us = 0;
    uint32_t gain_x1000 = 0;
    uint32_t iso = 0;
};

/* Live IMX586 timing registers read over I2C (the register method for the
   exposure-edge derivation). */
struct TimingRegisters {
    bool valid = false;
    uint32_t exposure_lines = 0;    /* 0x0202/0x0203 COARSE_INTEG_TIME */
    uint32_t vts_lines = 0;         /* 0x0340/0x0341 FRM_LENGTH_LINES */
    uint32_t hts_pck = 0;           /* 0x0342/0x0343 LINE_LENGTH_PCK */
    uint32_t xvs_offset_lines = 0;  /* 0x3f68 INTERNAL_XVS_OFFSET_LINE */
    uint64_t read_monotonic_ns = 0; /* stamped at the register read */
};

/* One line time of the 4000x3000@4fps mode: VTS 22980 lines span the
   250 ms frame period, so line_time = 250ms / 22980 ~= 10.879 us. */
constexpr uint64_t kImx586LineTimeNs = 10879;

/* The internal offset (in lines) from the XVS-anchored frame start to the
   first row's readout.  Zero by default; calibrate against a scope
   measurement if the exposure-edge comparison shows a fixed residual. */
constexpr int64_t kImx586ExposureMarginLines = 0;

/* Read the exposure/frame-length/line-length/offset registers from an
   IMX586 at slave_addr on i2c_bus_device (e.g. "/dev/i2c-3", 0x1a).
   Returns false if any register read fails. */
bool read_timing_registers(const char *i2c_bus_device, uint8_t slave_addr,
                           TimingRegisters *timing);

/* Program the XVS slave registers from userspace, mirroring
   imx586_configure_xvs_slave() in the PCL kernel driver, for boards whose
   flashed device tree lacks "sony,xvs-slave-mode".  Must be called while
   the sensor is powered; writing while streaming is tolerated (the sensor
   switches to the XVS-triggered cadence at the next XVS edge).
   exposure_lines < 0 reads the live COARSE_INTEG_TIME for the PRSH
   calculation.  Returns false with a human-readable report on failure. */
bool write_xvs_slave_registers(const char *i2c_bus_device, uint8_t slave_addr,
                               long exposure_lines, std::string *report);

bool convert_controls(const ControlSnapshot &controls, Metadata *metadata);
bool self_test();

}  // namespace imx586_v4l2

#endif
