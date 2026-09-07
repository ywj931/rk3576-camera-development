#include "camera_photo_backend.h"
#include "xdas_camera_protocol.h"
#include "xdas_camera_service.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

bool contains(const std::vector<std::uint8_t> &bytes, const std::string &text)
{
    return std::search(bytes.begin(), bytes.end(), text.begin(), text.end()) !=
           bytes.end();
}

bool read_binary(const char *path, std::vector<std::uint8_t> *bytes)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    bytes->assign(std::istreambuf_iterator<char>(input),
                  std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

bool read_text(const char *path, std::string *text)
{
    std::ifstream input(path);
    if (!input)
        return false;
    text->assign(std::istreambuf_iterator<char>(input),
                 std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

bool exchange(const std::vector<std::uint8_t> &request,
              const std::vector<std::uint8_t> &expected,
              const xdas_camera_protocol::command_handler &handler)
{
    std::vector<std::uint8_t> response;
    return xdas_camera_protocol::process_frame(
               request.data(), request.size(), handler, &response) ==
               xdas_camera_protocol::OK &&
           response == expected;
}

bool exchange_error(const std::vector<std::uint8_t> &request,
                    std::uint8_t expected_error,
                    const xdas_camera_protocol::command_handler &handler)
{
    std::vector<std::uint8_t> response;
    return xdas_camera_protocol::process_frame(
               request.data(), request.size(), handler, &response) ==
               xdas_camera_protocol::OK &&
           response.size() == 6U && response[2] == request[2] &&
           response[3] == request[3] && response[4] == expected_error &&
           xdas_camera_protocol::crc8(response.data(), response.size() - 1U) ==
               response.back();
}

camera_photo_metadata_t metadata_for(int camera_id)
{
    camera_photo_metadata_t metadata = {};
    metadata.camera_id = camera_id;
    metadata.frame_id = 100U + static_cast<std::uint32_t>(camera_id);
    metadata.trigger_id = 42U + static_cast<std::uint64_t>(camera_id);
    metadata.trigger_monotonic_ns = 5000000000ULL;
    metadata.trigger_realtime_ns = 1710000000123456789ULL;
    metadata.pps_id = 77;
    metadata.trigger_timer_tick = 9123456;
    metadata.frame_monotonic_ns = 5000012000ULL;
    metadata.frame_realtime_ns = 1710000000123468789ULL;
    metadata.exposure_start_realtime_ns = 1710000000123461789ULL;
    metadata.exposure_center_realtime_ns = 1710000000125961789ULL;
    metadata.sensor_response_offset_ns = 5000;
    metadata.trigger_to_frame_ns = 12000;
    metadata.exposure_us = 5000;
    metadata.gain_x1000 = 2000;
    metadata.iso = 200;
    metadata.utc_valid = 1;
    std::snprintf(metadata.trigger_source, sizeof(metadata.trigger_source),
                  "BOARD_TEST");
    std::snprintf(metadata.exposure_source, sizeof(metadata.exposure_source),
                  "TEST_VALUE");
    return metadata;
}

bool parse_dimension(const char *text, std::uint32_t *value)
{
    if (!text || !*text || !value || text[0] == '-')
        return false;
    errno = 0;
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (errno || end == text || *end != '\0' || parsed == 0 ||
        parsed > 16384UL)
        return false;
    *value = static_cast<std::uint32_t>(parsed);
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 1 && argc != 2 && argc != 5 && argc != 6) {
        std::fprintf(stderr,
                     "usage: %s [OUTPUT_PARENT [NV12_FILE WIDTH HEIGHT "
                     "[CAMERA_COUNT]]]\n",
                     argv[0]);
        return 2;
    }
    std::string output_parent = argc == 2 ? argv[1] : "/tmp";
    if (argc >= 5)
        output_parent = argv[1];
    while (output_parent.size() > 1U && output_parent.back() == '/')
        output_parent.pop_back();
    if (output_parent.empty() || output_parent[0] != '/') {
        std::fprintf(stderr, "OUTPUT_PARENT must be an absolute path\n");
        return 2;
    }
    std::string output_template =
        output_parent + "/xdas_camera_save_test.XXXXXX";
    std::vector<char> mutable_template(output_template.begin(),
                                       output_template.end());
    mutable_template.push_back('\0');
    const char *output_root = mkdtemp(mutable_template.data());
    if (!output_root) {
        std::perror("mkdtemp");
        return 1;
    }

    camera_photo_config_t photo_config = {};
    camera_photo_default_config(&photo_config);
    photo_config.width = 640;
    photo_config.height = 480;
    if (argc >= 5 &&
        (!parse_dimension(argv[3], &photo_config.width) ||
         !parse_dimension(argv[4], &photo_config.height))) {
        std::fprintf(stderr, "invalid WIDTH or HEIGHT\n");
        return 2;
    }
    std::uint32_t camera_count = CAMERA_PHOTO_CAMERA_COUNT;
    if (argc == 6 &&
        (!parse_dimension(argv[5], &camera_count) || camera_count == 0U ||
         camera_count > CAMERA_PHOTO_CAMERA_COUNT)) {
        std::fprintf(stderr, "CAMERA_COUNT must be 1 or %d\n",
                     CAMERA_PHOTO_CAMERA_COUNT);
        return 2;
    }
    photo_config.queue_depth = 4;
    camera_photo_backend_t *photo = nullptr;
    const int create_result = camera_photo_create(&photo_config, &photo);
    if (create_result != CAMERA_PHOTO_OK) {
        std::fprintf(stderr, "XDAS_SAVE_TEST_FAILED stage=create code=%d\n",
                     create_result);
        return 1;
    }

    xdas_camera_service::config service_config;
    service_config.camera_count = static_cast<std::uint8_t>(camera_count);
    service_config.save_root = output_root;
    xdas_camera_service::operations operations;
    operations.capture_running = [](std::uint8_t) { return true; };
    operations.save_enabled = [&](std::uint8_t camera_id) {
        return camera_photo_is_enabled(photo, camera_id) != 0;
    };
    operations.save_session_error = [&](std::uint8_t camera_id) -> int {
        camera_photo_status_t status = {};
        if (camera_photo_get_status(photo, camera_id, &status) !=
            CAMERA_PHOTO_OK)
            return CAMERA_PHOTO_ERR_ARGUMENT;
        return status.last_error;
    };
    operations.start_save = [&](std::uint8_t camera_id,
                                const std::string &path) {
        return camera_photo_start(photo, camera_id, path.c_str());
    };
    operations.stop_save = [&](std::uint8_t camera_id) {
        return camera_photo_stop(photo, camera_id);
    };
    const auto handler = [&](const xdas_camera_protocol::request &request,
                             xdas_camera_protocol::reply *reply) {
        xdas_camera_service::handle_command(request, reply, service_config,
                                            operations);
    };

    const std::vector<std::uint8_t> save_on =
        {0xaa, 0x05, 0x01, 0x11, 0x73};
    const std::vector<std::uint8_t> save_on_ack =
        {0x55, 0x06, 0x01, 0x11, 0x00, 0xa7};
    const std::vector<std::uint8_t> save_off =
        {0xaa, 0x05, 0x01, 0x12, 0xd9};
    const std::vector<std::uint8_t> save_off_ack =
        {0x55, 0x06, 0x01, 0x12, 0x00, 0xba};

    bool passed = exchange(save_on, save_on_ack, handler);
    if (!passed) {
        std::fprintf(stderr, "XDAS_SAVE_TEST_FAILED stage=save_on root=%s\n",
                     output_root);
        camera_photo_destroy(photo);
        return 1;
    }

    const std::size_t y_size =
        static_cast<std::size_t>(photo_config.width) * photo_config.height;
    if (photo_config.height != 0U &&
        y_size / photo_config.height != photo_config.width) {
        std::fprintf(stderr, "image dimensions overflow size_t\n");
        camera_photo_destroy(photo);
        return 2;
    }
    std::vector<std::uint8_t> y(y_size);
    std::vector<std::uint8_t> uv(y_size / 2U, 128U);
    if (argc >= 5) {
        std::ifstream input(argv[2], std::ios::binary);
        if (!input ||
            !input.read(reinterpret_cast<char *>(y.data()), y.size()) ||
            !input.read(reinterpret_cast<char *>(uv.data()), uv.size())) {
            std::fprintf(stderr, "unable to read one NV12 frame from %s\n",
                         argv[2]);
            camera_photo_destroy(photo);
            return 1;
        }
    } else {
        for (std::size_t index = 0; index < y.size(); ++index)
            y[index] = static_cast<std::uint8_t>(16U + (index % 220U));
    }

    camera_photo_metadata_t invalid_metadata = metadata_for(0);
    invalid_metadata.iso = 0;
    passed = passed &&
             camera_photo_submit_nv12(photo, 0, y.data(), y.size(), uv.data(),
                                      uv.size(), &invalid_metadata) ==
                 CAMERA_PHOTO_ERR_ARGUMENT &&
             exchange_error(save_off, xdas_camera_protocol::ERROR_BAD_COMMAND,
                            handler) &&
             exchange_error(save_off, xdas_camera_protocol::ERROR_BAD_COMMAND,
                            handler) &&
             !camera_photo_is_enabled(photo, 0) &&
             !camera_photo_is_enabled(photo, 1) &&
             exchange(save_on, save_on_ack, handler) &&
             exchange(save_on, save_on_ack, handler);

    for (std::uint32_t camera_id = 0; camera_id < camera_count; ++camera_id) {
        const camera_photo_metadata_t metadata = metadata_for(camera_id);
        if (camera_photo_submit_nv12(photo, camera_id, y.data(), y.size(),
                                     uv.data(), uv.size(), &metadata) !=
            CAMERA_PHOTO_OK) {
            passed = false;
        }
    }

    passed = passed && exchange(save_off, save_off_ack, handler) &&
             exchange(save_off, save_off_ack, handler);

    for (std::uint32_t camera_id = 0; camera_id < camera_count; ++camera_id) {
        camera_photo_status_t status = {};
        if (camera_photo_get_status(photo, camera_id, &status) !=
                CAMERA_PHOTO_OK ||
            status.enabled || status.photos_saved != 1U ||
            status.frames_submitted != 1U || status.queue_drops != 0U ||
            status.invalid_metadata != (camera_id == 0 ? 1U : 0U) ||
            status.encode_errors != 0U || status.exif_errors != 0U ||
            status.write_errors != 0U) {
            passed = false;
            continue;
        }

        std::vector<std::uint8_t> jpeg;
        const std::string marker =
            "camera_id=" + std::to_string(camera_id) + ";frame_id=" +
            std::to_string(100 + camera_id) + ";trigger_id=" +
            std::to_string(42 + camera_id);
        if (!read_binary(status.last_photo, &jpeg) || jpeg.size() < 8U ||
            jpeg[0] != 0xff || jpeg[1] != 0xd8 || jpeg[2] != 0xff ||
            jpeg[3] != 0xe1 || jpeg[jpeg.size() - 2U] != 0xff ||
            jpeg.back() != 0xd9 || !contains(jpeg, "Exif") ||
            !contains(jpeg, marker)) {
            passed = false;
        }

        std::string csv;
        if (!read_text(status.metadata_csv, &csv) ||
            csv.find("camera_id,frame_id,trigger_id") != 0U ||
            csv.find(std::to_string(camera_id) + "," +
                     std::to_string(100 + camera_id) + "," +
                     std::to_string(42 + camera_id) + ",BOARD_TEST") ==
                std::string::npos) {
            passed = false;
        }
    }

    camera_photo_destroy(photo);
    if (!passed) {
        std::fprintf(stderr,
                     "XDAS_SAVE_TEST_FAILED stage=artifact_validation root=%s\n",
                     output_root);
        return 1;
    }
    std::printf(
        "XDAS_SAVE_TEST_OK root=%s commands=save_on,idempotent,save_off "
        "failure_ack=sticky cameras=%u size=%ux%u input=%s "
        "jpeg=structure_verified exif=verified csv=verified drain=verified\n",
        output_root, camera_count, photo_config.width, photo_config.height,
        argc >= 5 ? "file" : "synthetic");
    return 0;
}
