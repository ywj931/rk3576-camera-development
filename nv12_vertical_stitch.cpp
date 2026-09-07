#include "nv12_vertical_stitch.h"

#include <cstring>
#include <limits>
#include <new>

namespace camera_photo {

int stitch_nv12_vertical(
    uint32_t width, uint32_t height,
    const void *top_y, size_t top_y_size,
    const void *top_uv, size_t top_uv_size,
    const void *bottom_y, size_t bottom_y_size,
    const void *bottom_uv, size_t bottom_uv_size,
    std::vector<uint8_t> *output)
{
    if (!output || !top_y || !top_uv || !bottom_y || !bottom_uv)
        return NV12_STITCH_ERR_ARGUMENT;
    if (!width || !height || (width & 1U) || (height & 1U) ||
        height > std::numeric_limits<uint32_t>::max() / 2U)
        return NV12_STITCH_ERR_DIMENSIONS;
    if (static_cast<size_t>(width) >
        std::numeric_limits<size_t>::max() / height)
        return NV12_STITCH_ERR_OVERFLOW;
    const size_t y_size = static_cast<size_t>(width) * height;
    const size_t uv_size = y_size / 2U;
    if (top_y_size < y_size || bottom_y_size < y_size ||
        top_uv_size < uv_size || bottom_uv_size < uv_size)
        return NV12_STITCH_ERR_RANGE;
    if (y_size > std::numeric_limits<size_t>::max() / 3U)
        return NV12_STITCH_ERR_OVERFLOW;

    try {
        std::vector<uint8_t> stitched(y_size * 3U);
        uint8_t *destination = stitched.data();
        std::memcpy(destination, top_y, y_size);
        std::memcpy(destination + y_size, bottom_y, y_size);
        std::memcpy(destination + y_size * 2U, top_uv, uv_size);
        std::memcpy(destination + y_size * 2U + uv_size, bottom_uv,
                    uv_size);
        *output = std::move(stitched);
    } catch (const std::bad_alloc &) {
        return NV12_STITCH_ERR_MEMORY;
    }
    return NV12_STITCH_OK;
}

}  // namespace camera_photo
