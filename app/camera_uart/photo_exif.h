#ifndef PHOTO_EXIF_H
#define PHOTO_EXIF_H

#include <cstdint>
#include <string>
#include <vector>

namespace camera_photo {

struct Metadata {
    int camera_id = -1;
    uint32_t frame_id = 0;
    uint64_t trigger_id = 0;
    uint64_t trigger_monotonic_ns = 0;
    uint64_t trigger_realtime_ns = 0;
    uint64_t frame_monotonic_ns = 0;
    uint64_t frame_realtime_ns = 0;
    uint64_t exposure_start_realtime_ns = 0;
    uint64_t exposure_center_realtime_ns = 0;
    int64_t sensor_response_offset_ns = 0;
    int64_t trigger_to_frame_ns = 0;
    uint32_t exposure_us = 0;
    uint32_t gain_x1000 = 0;
    uint32_t iso = 0;
    bool utc_valid = false;
    bool iso_estimated = false;
    std::string trigger_source;
    std::string exposure_source;
};

enum {
    EXIF_OK = 0,
    EXIF_ERR_ARGUMENT = -1,
    EXIF_ERR_JPEG = -2,
    EXIF_ERR_TOO_LARGE = -3,
    EXIF_ERR_VERIFY = -4,
};

int insert_exif(const std::vector<uint8_t> &jpeg, const Metadata &metadata,
                std::vector<uint8_t> *output, std::string *error);

int self_test(std::string *report);

}  // namespace camera_photo

#endif
