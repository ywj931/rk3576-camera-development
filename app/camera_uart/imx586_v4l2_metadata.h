#ifndef IMX586_V4L2_METADATA_H
#define IMX586_V4L2_METADATA_H

#include <cstdint>

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

bool convert_controls(const ControlSnapshot &controls, Metadata *metadata);
bool self_test();

}  // namespace imx586_v4l2

#endif
