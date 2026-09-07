#ifndef NV12_VERTICAL_STITCH_H
#define NV12_VERTICAL_STITCH_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace camera_photo {

enum nv12_stitch_result {
    NV12_STITCH_OK = 0,
    NV12_STITCH_ERR_ARGUMENT = -1,
    NV12_STITCH_ERR_DIMENSIONS = -2,
    NV12_STITCH_ERR_RANGE = -3,
    NV12_STITCH_ERR_OVERFLOW = -4,
    NV12_STITCH_ERR_MEMORY = -5,
};

int stitch_nv12_vertical(
    uint32_t width, uint32_t height,
    const void *top_y, size_t top_y_size,
    const void *top_uv, size_t top_uv_size,
    const void *bottom_y, size_t bottom_y_size,
    const void *bottom_uv, size_t bottom_uv_size,
    std::vector<uint8_t> *output);

}  // namespace camera_photo

#endif
