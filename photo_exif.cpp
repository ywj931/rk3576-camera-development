#include "photo_exif.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <numeric>

namespace camera_photo {
namespace {

constexpr uint16_t kTypeAscii = 2;
constexpr uint16_t kTypeShort = 3;
constexpr uint16_t kTypeLong = 4;
constexpr uint16_t kTypeRational = 5;
constexpr uint16_t kTypeUndefined = 7;

void append_u16(std::vector<uint8_t> *data, uint16_t value)
{
    data->push_back(static_cast<uint8_t>(value));
    data->push_back(static_cast<uint8_t>(value >> 8));
}

void append_u32(std::vector<uint8_t> *data, uint32_t value)
{
    data->push_back(static_cast<uint8_t>(value));
    data->push_back(static_cast<uint8_t>(value >> 8));
    data->push_back(static_cast<uint8_t>(value >> 16));
    data->push_back(static_cast<uint8_t>(value >> 24));
}

void append_entry(std::vector<uint8_t> *data, uint16_t tag, uint16_t type,
                  uint32_t count, uint32_t value)
{
    append_u16(data, tag);
    append_u16(data, type);
    append_u32(data, count);
    append_u32(data, value);
}

void append_bytes(std::vector<uint8_t> *data, const std::string &value)
{
    data->insert(data->end(), value.begin(), value.end());
}

uint32_t checked_u32(size_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(value);
}

std::string datetime_original(uint64_t realtime_ns)
{
    const time_t seconds = static_cast<time_t>(realtime_ns / 1000000000ULL);
    struct tm value = {};
    if (!gmtime_r(&seconds, &value))
        return "1970:01:01 00:00:00";
    char text[32];
    if (!std::strftime(text, sizeof(text), "%Y:%m:%d %H:%M:%S", &value))
        return "1970:01:01 00:00:00";
    return text;
}

std::string subsecond_original(uint64_t realtime_ns)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%09llu",
                  static_cast<unsigned long long>(realtime_ns % 1000000000ULL));
    return text;
}

std::string user_comment(const Metadata &metadata)
{
    char text[1024];
    std::snprintf(
        text, sizeof(text),
        "camera_id=%d;frame_id=%u;trigger_id=%llu;"
        "trigger_monotonic_ns=%llu;trigger_realtime_ns=%llu;"
        "pps_id=%llu;trigger_timer_tick=%llu;"
        "frame_monotonic_ns=%llu;frame_realtime_ns=%llu;"
        "exposure_start_realtime_ns=%llu;exposure_center_realtime_ns=%llu;"
        "exposure_us=%u;gain_x1000=%u;iso=%u;iso_estimated=%d;"
        "white_balance_valid=%d;white_balance_auto=%d;"
        "white_balance_converged=%d;white_balance_cct=%u;"
        "wb_r_gain_x1000=%u;wb_gr_gain_x1000=%u;"
        "wb_gb_gain_x1000=%u;wb_b_gain_x1000=%u;"
        "sensor_response_offset_ns=%lld;trigger_to_frame_ns=%lld;"
        "utc_valid=%d;trigger_monotonic_is_uart_arrival=%d;"
        "trigger_source=%s;exposure_source=%s",
        metadata.camera_id, metadata.frame_id,
        static_cast<unsigned long long>(metadata.trigger_id),
        static_cast<unsigned long long>(metadata.trigger_monotonic_ns),
        static_cast<unsigned long long>(metadata.trigger_realtime_ns),
        static_cast<unsigned long long>(metadata.pps_id),
        static_cast<unsigned long long>(metadata.trigger_timer_tick),
        static_cast<unsigned long long>(metadata.frame_monotonic_ns),
        static_cast<unsigned long long>(metadata.frame_realtime_ns),
        static_cast<unsigned long long>(metadata.exposure_start_realtime_ns),
        static_cast<unsigned long long>(metadata.exposure_center_realtime_ns),
        metadata.exposure_us, metadata.gain_x1000, metadata.iso,
        metadata.iso_estimated ? 1 : 0,
        metadata.white_balance_valid ? 1 : 0,
        metadata.white_balance_auto ? 1 : 0,
        metadata.white_balance_converged ? 1 : 0,
        metadata.white_balance_cct, metadata.wb_r_gain_x1000,
        metadata.wb_gr_gain_x1000, metadata.wb_gb_gain_x1000,
        metadata.wb_b_gain_x1000,
        static_cast<long long>(metadata.sensor_response_offset_ns),
        static_cast<long long>(metadata.trigger_to_frame_ns),
        metadata.utc_valid ? 1 : 0,
        metadata.trigger_monotonic_is_uart_arrival ? 1 : 0,
        metadata.trigger_source.c_str(),
        metadata.exposure_source.c_str());
    return text;
}

std::string composite_user_comment(const Metadata &top,
                                   const Metadata &bottom,
                                   uint64_t pair_id,
                                   int64_t frame_delta_ns)
{
    const uint64_t composite_timestamp =
        std::min(top.exposure_start_realtime_ns,
                 bottom.exposure_start_realtime_ns);
    const bool composite_utc_valid = top.utc_valid && bottom.utc_valid;
    return "layout=vertical;pair_id=" + std::to_string(pair_id) +
           ";composite_capture_realtime_ns=" +
           std::to_string(composite_timestamp) +
           ";composite_utc_valid=" +
           std::to_string(composite_utc_valid ? 1 : 0) +
           ";timestamp_basis=earliest_exposure_start;"
           "standard_exif_exposure_camera_id=" +
           std::to_string(top.camera_id) +
           ";bottom_minus_top_frame_ns=" +
           std::to_string(frame_delta_ns) + ";top={" + user_comment(top) +
           "};bottom={" + user_comment(bottom) + "}";
}

bool contains_bytes(const std::vector<uint8_t> &data, const char *text)
{
    const size_t length = std::strlen(text);
    return std::search(data.begin(), data.end(), text, text + length) !=
           data.end();
}

}  // namespace

static int insert_exif_with_comment(const std::vector<uint8_t> &jpeg,
                                    const Metadata &metadata,
                                    const std::string &plain_comment,
                                    std::vector<uint8_t> *output,
                                    std::string *error)
{
    if (!output || metadata.camera_id < 0 || metadata.exposure_us == 0 ||
        metadata.iso == 0) {
        if (error)
            *error = "invalid EXIF metadata";
        return EXIF_ERR_ARGUMENT;
    }
    if (jpeg.size() < 2 || jpeg[0] != 0xff || jpeg[1] != 0xd8) {
        if (error)
            *error = "input does not start with JPEG SOI";
        return EXIF_ERR_JPEG;
    }

    const std::string software = std::string("DAS Camera") + '\0';
    const std::string date = datetime_original(
                                 metadata.exposure_start_realtime_ns) +
                             '\0';
    const std::string subsec =
        subsecond_original(metadata.exposure_start_realtime_ns) + '\0';
    std::string comment("ASCII\0\0\0", 8);
    comment += plain_comment;
    comment.push_back('\0');

    std::vector<uint8_t> tiff;
    tiff.reserve(1024);
    tiff.push_back('I');
    tiff.push_back('I');
    append_u16(&tiff, 42);
    append_u32(&tiff, 8);

    const uint32_t ifd0_size = 2 + 2 * 12 + 4;
    const uint32_t software_offset = 8 + ifd0_size;
    uint32_t exif_ifd_offset =
        software_offset + checked_u32(software.size());
    if (exif_ifd_offset & 1U)
        ++exif_ifd_offset;

    append_u16(&tiff, 2);
    append_entry(&tiff, 0x0131, kTypeAscii, checked_u32(software.size()),
                 software_offset);
    append_entry(&tiff, 0x8769, kTypeLong, 1, exif_ifd_offset);
    append_u32(&tiff, 0);
    append_bytes(&tiff, software);
    while (tiff.size() < exif_ifd_offset)
        tiff.push_back(0);

    const uint16_t exif_entry_count =
        metadata.white_balance_valid ? 6U : 5U;
    const uint32_t exif_ifd_size = 2 + exif_entry_count * 12 + 4;
    const uint32_t exposure_offset = exif_ifd_offset + exif_ifd_size;
    const uint32_t date_offset = exposure_offset + 8;
    const uint32_t subsec_offset =
        date_offset + checked_u32(date.size());
    const uint32_t comment_offset =
        subsec_offset + checked_u32(subsec.size());

    append_u16(&tiff, exif_entry_count);
    append_entry(&tiff, 0x829a, kTypeRational, 1, exposure_offset);
    append_entry(&tiff, 0x8827, kTypeShort, 1,
                 std::min<uint32_t>(metadata.iso, UINT16_MAX));
    append_entry(&tiff, 0x9003, kTypeAscii, checked_u32(date.size()),
                 date_offset);
    append_entry(&tiff, 0x9291, kTypeAscii, checked_u32(subsec.size()),
                 subsec_offset);
    append_entry(&tiff, 0x9286, kTypeUndefined,
                 checked_u32(comment.size()), comment_offset);
    if (metadata.white_balance_valid) {
        append_entry(&tiff, 0xa403, kTypeShort, 1,
                     metadata.white_balance_auto ? 0U : 1U);
    }
    append_u32(&tiff, 0);

    const uint32_t divisor = std::gcd(metadata.exposure_us, 1000000U);
    append_u32(&tiff, metadata.exposure_us / divisor);
    append_u32(&tiff, 1000000U / divisor);
    append_bytes(&tiff, date);
    append_bytes(&tiff, subsec);
    append_bytes(&tiff, comment);

    const size_t app1_payload_size = 6 + tiff.size();
    if (app1_payload_size + 2 > UINT16_MAX) {
        if (error)
            *error = "EXIF APP1 segment exceeds JPEG limit";
        return EXIF_ERR_TOO_LARGE;
    }

    output->clear();
    output->reserve(jpeg.size() + app1_payload_size + 4);
    output->push_back(0xff);
    output->push_back(0xd8);
    output->push_back(0xff);
    output->push_back(0xe1);
    const uint16_t segment_length =
        static_cast<uint16_t>(app1_payload_size + 2);
    output->push_back(static_cast<uint8_t>(segment_length >> 8));
    output->push_back(static_cast<uint8_t>(segment_length));
    output->insert(output->end(), {'E', 'x', 'i', 'f', 0, 0});
    output->insert(output->end(), tiff.begin(), tiff.end());
    output->insert(output->end(), jpeg.begin() + 2, jpeg.end());
    if (error)
        error->clear();
    return EXIF_OK;
}

int insert_exif(const std::vector<uint8_t> &jpeg, const Metadata &metadata,
                std::vector<uint8_t> *output, std::string *error)
{
    return insert_exif_with_comment(jpeg, metadata, user_comment(metadata),
                                    output, error);
}

int insert_composite_exif(const std::vector<uint8_t> &jpeg,
                          const Metadata &top, const Metadata &bottom,
                          uint64_t pair_id, int64_t frame_delta_ns,
                          std::vector<uint8_t> *output, std::string *error)
{
    if (bottom.camera_id < 0 || !bottom.exposure_start_realtime_ns ||
        !bottom.exposure_us || !bottom.iso) {
        if (error)
            *error = "invalid bottom-camera EXIF metadata";
        return EXIF_ERR_ARGUMENT;
    }
    Metadata composite_anchor = top;
    composite_anchor.exposure_start_realtime_ns =
        std::min(top.exposure_start_realtime_ns,
                 bottom.exposure_start_realtime_ns);
    composite_anchor.utc_valid = top.utc_valid && bottom.utc_valid;
    return insert_exif_with_comment(
        jpeg, composite_anchor,
        composite_user_comment(top, bottom, pair_id, frame_delta_ns), output,
        error);
}

int self_test(std::string *report)
{
    Metadata metadata;
    metadata.camera_id = 1;
    metadata.frame_id = 42;
    metadata.trigger_id = 1007;
    metadata.trigger_monotonic_ns = 5000000000ULL;
    metadata.trigger_realtime_ns = 1710000000123456789ULL;
    metadata.pps_id = 77;
    metadata.trigger_timer_tick = 9123456ULL;
    metadata.utc_valid = true;
    metadata.trigger_monotonic_is_uart_arrival = true;
    metadata.frame_monotonic_ns = 5000012000ULL;
    metadata.frame_realtime_ns = 1710000000123468789ULL;
    metadata.exposure_start_realtime_ns = 1710000000123461789ULL;
    metadata.exposure_center_realtime_ns = 1710000000125961789ULL;
    metadata.exposure_us = 5000;
    metadata.gain_x1000 = 2000;
    metadata.iso = 200;
    metadata.white_balance_valid = true;
    metadata.white_balance_auto = true;
    metadata.white_balance_converged = true;
    metadata.white_balance_cct = 5500;
    metadata.wb_r_gain_x1000 = 1800;
    metadata.wb_gr_gain_x1000 = 1000;
    metadata.wb_gb_gain_x1000 = 1000;
    metadata.wb_b_gain_x1000 = 1650;
    metadata.sensor_response_offset_ns = 5000;
    metadata.trigger_to_frame_ns = 12000;
    metadata.trigger_source = "SIM";
    metadata.exposure_source = "RKAIQ_LATEST";

    const std::vector<uint8_t> mock_jpeg = {0xff, 0xd8, 0xff, 0xd9};
    std::vector<uint8_t> output;
    std::string error;
    const int result = insert_exif(mock_jpeg, metadata, &output, &error);
    const bool valid =
        result == EXIF_OK && output.size() > mock_jpeg.size() &&
        output[0] == 0xff && output[1] == 0xd8 && output[2] == 0xff &&
        output[3] == 0xe1 && contains_bytes(output, "Exif") &&
        contains_bytes(output, "camera_id=1;frame_id=42;trigger_id=1007") &&
        contains_bytes(output, "pps_id=77;trigger_timer_tick=9123456") &&
        contains_bytes(output,
                       "utc_valid=1;trigger_monotonic_is_uart_arrival=1") &&
        contains_bytes(output, "exposure_us=5000") &&
        contains_bytes(output,
                       "white_balance_valid=1;white_balance_auto=1") &&
        contains_bytes(output, "white_balance_cct=5500") &&
        contains_bytes(output, "trigger_source=SIM") &&
        contains_bytes(output, "2024:03:09 16:00:00") &&
        output[output.size() - 2] == 0xff && output.back() == 0xd9;
    if (!valid) {
        if (report)
            *report = error.empty() ? "EXIF structure verification failed"
                                    : error;
        return EXIF_ERR_VERIFY;
    }
    Metadata top = metadata;
    top.camera_id = 0;
    Metadata bottom = metadata;
    bottom.camera_id = 1;
    bottom.frame_id = 43;
    bottom.exposure_start_realtime_ns =
        metadata.exposure_start_realtime_ns - 1000000ULL;
    bottom.exposure_us = 7000;
    bottom.iso = 320;
    std::vector<uint8_t> composite;
    const int composite_result = insert_composite_exif(
        mock_jpeg, top, bottom, 1007, 18000, &composite, &error);
    const bool composite_valid =
        composite_result == EXIF_OK &&
        contains_bytes(composite,
                       "layout=vertical;pair_id=1007;"
                       "composite_capture_realtime_ns=1710000000122461789;"
                       "composite_utc_valid=1;"
                       "timestamp_basis=earliest_exposure_start;"
                       "standard_exif_exposure_camera_id=0;"
                       "bottom_minus_top_frame_ns=18000") &&
        contains_bytes(composite,
                       "top={camera_id=0;frame_id=42;trigger_id=1007") &&
        contains_bytes(composite,
                       "bottom={camera_id=1;frame_id=43;trigger_id=1007") &&
        contains_bytes(composite, "exposure_us=7000") &&
        contains_bytes(composite, "iso=320");
    if (!composite_valid) {
        if (report)
            *report = error.empty()
                          ? "composite EXIF verification failed"
                          : error;
        return EXIF_ERR_VERIFY;
    }
    if (report)
        *report = "single and vertical-composite JPEG EXIF verified";
    return EXIF_OK;
}

}  // namespace camera_photo
