#include "nv12_vertical_stitch.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

bool expect_result(int actual, int expected, const char *name)
{
    if (actual == expected)
        return true;
    std::fprintf(stderr, "NV12_STITCH_TEST_FAILED case=%s actual=%d expected=%d\n",
                 name, actual, expected);
    return false;
}

}  // namespace

int main()
{
    const std::vector<uint8_t> top_y = {1, 2, 3, 4, 5, 6, 7, 8};
    const std::vector<uint8_t> top_uv = {21, 22, 23, 24};
    const std::vector<uint8_t> bottom_y = {11, 12, 13, 14, 15, 16, 17, 18};
    const std::vector<uint8_t> bottom_uv = {31, 32, 33, 34};
    const std::vector<uint8_t> expected = {
        1, 2, 3, 4, 5, 6, 7, 8,
        11, 12, 13, 14, 15, 16, 17, 18,
        21, 22, 23, 24,
        31, 32, 33, 34,
    };
    std::vector<uint8_t> output = {99};
    bool passed = expect_result(
        camera_photo::stitch_nv12_vertical(
            4, 2, top_y.data(), top_y.size(), top_uv.data(), top_uv.size(),
            bottom_y.data(), bottom_y.size(), bottom_uv.data(),
            bottom_uv.size(), &output),
        camera_photo::NV12_STITCH_OK, "valid_layout");
    passed = passed && output == expected;

    const std::vector<uint8_t> unchanged = output;
    passed = expect_result(
                 camera_photo::stitch_nv12_vertical(
                     3, 2, top_y.data(), top_y.size(), top_uv.data(),
                     top_uv.size(), bottom_y.data(), bottom_y.size(),
                     bottom_uv.data(), bottom_uv.size(), &output),
                 camera_photo::NV12_STITCH_ERR_DIMENSIONS, "odd_width") &&
             passed;
    passed = expect_result(
                 camera_photo::stitch_nv12_vertical(
                     4, 3, top_y.data(), top_y.size(), top_uv.data(),
                     top_uv.size(), bottom_y.data(), bottom_y.size(),
                     bottom_uv.data(), bottom_uv.size(), &output),
                 camera_photo::NV12_STITCH_ERR_DIMENSIONS, "odd_height") &&
             passed;
    passed = expect_result(
                 camera_photo::stitch_nv12_vertical(
                     4, 2, top_y.data(), top_y.size() - 1U, top_uv.data(),
                     top_uv.size(), bottom_y.data(), bottom_y.size(),
                     bottom_uv.data(), bottom_uv.size(), &output),
                 camera_photo::NV12_STITCH_ERR_RANGE, "short_top_y") &&
             passed;
    passed = expect_result(
                 camera_photo::stitch_nv12_vertical(
                     4, 2, top_y.data(), top_y.size(), top_uv.data(),
                     top_uv.size(), bottom_y.data(), bottom_y.size(),
                     bottom_uv.data(), bottom_uv.size(), nullptr),
                 camera_photo::NV12_STITCH_ERR_ARGUMENT, "null_output") &&
             passed;
    passed = passed && output == unchanged;
    if (!passed) {
        std::fprintf(stderr, "NV12_STITCH_TEST_FAILED layout_or_strong_guarantee\n");
        return 1;
    }
    std::printf("NV12_STITCH_TEST_OK input=4x2 output=4x4 bytes=%zu "
                "layout=Y0,Y1,UV0,UV1\n",
                output.size());
    return 0;
}
