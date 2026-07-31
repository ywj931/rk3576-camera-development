#include "camera_backend.h"
#include "camera_net_backend.h"
#include "camera_photo_backend.h"
#include "camera_uvc_backend.h"
#include "capture_backend.h"
#include "photo_exif.h"
#include "trigger_frame_binder.h"
#include "trigger_simulator.h"
#include "xvs_uart_controller.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

volatile sig_atomic_t g_stop = 0;

void signal_handler(int)
{
    g_stop = 1;
}

bool parse_u32(const std::string &text, uint32_t *value)
{
    if (value == nullptr || text.empty() || text[0] == '-')
        return false;

    errno = 0;
    char *end = nullptr;
    unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' ||
        parsed > UINT32_MAX) {
        return false;
    }
    *value = static_cast<uint32_t>(parsed);
    return true;
}

bool parse_i64(const std::string &text, int64_t *value)
{
    if (!value || text.empty())
        return false;
    errno = 0;
    char *end = nullptr;
    const long long parsed = std::strtoll(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0')
        return false;
    *value = static_cast<int64_t>(parsed);
    return true;
}

uint64_t add_signed_ns(uint64_t base, int64_t offset)
{
    if (offset >= 0)
        return base + static_cast<uint64_t>(offset);
    const uint64_t magnitude = static_cast<uint64_t>(-(offset + 1)) + 1;
    return base > magnitude ? base - magnitude : 0;
}

bool parse_camera_id(const std::string &text, int *camera_id)
{
    uint32_t parsed = 0;
    if (!parse_u32(text, &parsed) ||
        parsed >= CAMERA_BACKEND_CAMERA_COUNT) {
        return false;
    }
    *camera_id = static_cast<int>(parsed);
    return true;
}

void print_usage(const char *program)
{
    std::cout
        << "Usage: " << program << " [options]\n"
        << "  --width PIXELS      sensor width (default: 4000)\n"
        << "  --height PIXELS     sensor height (default: 3000)\n"
        << "  --iq0 DIRECTORY     camera 0 IQ directory\n"
        << "  --iq1 DIRECTORY     camera 1 IQ directory\n"
        << "  --params0 DEVICE    camera 0 rkisp-input-params node\n"
        << "  --params1 DEVICE    camera 1 rkisp-input-params node\n"
        << "  --video0 DEVICE     camera 0 V4L2 capture node\n"
        << "  --video1 DEVICE     camera 1 V4L2 capture node\n"
        << "  --sensor0 TEXT      expected camera 0 sensor text\n"
        << "  --sensor1 TEXT      expected camera 1 sensor text\n"
        << "  --autostart         start capture and HTTP output for both cameras\n"
        << "  --daemon            autostart both cameras and run without console input\n"
        << "  --uvc-daemon        start both cameras and two UVC outputs without console input\n"
        << "  --sync-uart DEVICE  MCU XVS control UART (115200 8N1)\n"
        << "  --sync-protocol-self-test  test XVS protocol without camera hardware\n"
        << "  --sync-bind-self-test  test simulated trigger/frame binding without camera hardware\n"
        << "  --photo-exif-self-test  test JPEG EXIF generation without camera hardware\n"
        << "  UVC mode is fixed at 4000x3000 MJPEG 10 fps per camera\n"
        << "  network mode is fixed at 4000x3000 MJPEG 10 fps per camera, HTTP port 8080\n"
        << "  --help              show this help\n";
}

void print_commands()
{
    std::cout
        << "Commands:\n"
        << "  status [all|0|1]\n"
        << "  auto CAMERA_ID\n"
        << "  exposure CAMERA_ID EXPOSURE_US\n"
        << "  gain CAMERA_ID GAIN_X1000\n"
        << "  fps CAMERA_ID FPS\n"
        << "  stream-start CAMERA_ID|all\n"
        << "  stream-stop CAMERA_ID|all\n"
        << "  save-start CAMERA_ID OUTPUT_DIR\n"
        << "  save-stop CAMERA_ID\n"
        << "  photo-start CAMERA_ID OUTPUT_DIR\n"
        << "  photo-stop CAMERA_ID\n"
        << "  photo-status [all|0|1]\n"
        << "  photo-offset CAMERA_ID SENSOR_RESPONSE_OFFSET_NS\n"
        << "  capture-status [all|0|1]\n"
        << "  uvc-start CAMERA_ID|all\n"
        << "  uvc-stop\n"
        << "  uvc-status [CAMERA_ID|all]\n"
        << "  net-start CAMERA_ID\n"
        << "  net-stop [CAMERA_ID|all]\n"
        << "  net-status [CAMERA_ID|all]\n"
        << "  sync-idle\n"
        << "  sync-start 2|4 [LOW_PULSE_US]\n"
        << "  sync-count 2|4 PULSE_COUNT [LOW_PULSE_US]\n"
        << "  sync-stop\n"
        << "  sync-controller-status\n"
        << "  sync-bind-reset [PRE_SHUTTER_TRIGGERS]\n"
        << "  sync-bind-log CSV_PATH|off\n"
        << "  sync-bind-status\n"
        << "  sync-bind-last\n"
        << "  sync-sim-start 2|4 [PULSE_COUNT]\n"
        << "  sync-sim-stop\n"
        << "  sync-sim-status\n"
        << "  sync-status\n"
        << "  wait MILLISECONDS\n"
        << "  help\n"
        << "  quit\n";
}

void print_status(camera_backend_t *backend, int camera_id)
{
    camera_backend_status_t status = {};
    int result = camera_backend_get_status(backend, camera_id, &status);
    if (result != CAMERA_BACKEND_OK) {
        std::cout << "ERROR command=status camera_id=" << camera_id
                  << " code=" << result
                  << " reason=\"" << camera_backend_strerror(result) << "\"\n";
        return;
    }

    std::cout << "STATUS camera_id=" << status.camera_id
              << " online=" << status.online
              << " started=" << status.started
              << " mode=" << (status.manual_mode ? "MANUAL" : "AUTO")
              << " query_valid=" << status.query_valid
              << " exposure_us=" << status.exposure_us
              << " gain_x1000=" << status.gain_x1000
              << " iso=" << status.iso
              << " fps_x1000=" << status.fps_x1000
              << " mean_luma=" << status.mean_luma
              << " converged=" << status.converged
              << " last_aiq_error=" << status.last_aiq_error
              << " sensor=\"" << status.sensor_name << "\""
              << " iq=\"" << status.iq_dir << "\"\n";
}

void print_capture_status(capture_backend_t *capture, int camera_id)
{
    capture_backend_status_t status = {};
    int result = capture_backend_get_status(capture, camera_id, &status);
    if (result != CAPTURE_BACKEND_OK) {
        std::cout << "ERROR command=capture-status camera_id=" << camera_id
                  << " code=" << result
                  << " reason=\"" << capture_backend_strerror(result)
                  << "\"\n";
        return;
    }

    std::cout << "CAPTURE_STATUS camera_id=" << status.camera_id
              << " running=" << status.running
              << " saving=" << status.saving
              << " size=" << status.width << 'x' << status.height
              << " fps_x1000=" << status.fps_x1000
              << " frames=" << status.frames_captured
              << " sequence_drops=" << status.frames_dropped
              << " saved=" << status.frames_saved
              << " save_queue_pending=" << status.save_queue_pending
              << " save_queue_drops=" << status.save_queue_dropped
              << " save_failures=" << status.save_failures
              << " bytes_saved=" << status.bytes_saved
              << " timestamp_valid=" << status.timestamp_valid
              << " last_sequence=" << status.last_sequence
              << " buffer_flags=0x" << std::hex << status.last_buffer_flags
              << std::dec
              << " v4l2_timestamp_ns=" << status.last_v4l2_timestamp_ns
              << " realtime_dequeue_ns=" << status.last_realtime_dequeue_ns
              << " last_errno=" << status.last_errno
              << " device=\"" << status.video_device << "\""
              << " output_dir=\"" << status.output_dir << "\""
              << " metadata=\"" << status.metadata_path << "\""
              << " last_file=\"" << status.last_saved_path << "\"\n";
}

void print_sync_status(capture_backend_t *capture)
{
    capture_backend_sync_status_t status = {};
    int result = capture_backend_get_sync_status(capture, &status);
    if (result != CAPTURE_BACKEND_OK) {
        std::cout << "ERROR command=sync-status code=" << result
                  << " reason=\"" << capture_backend_strerror(result)
                  << "\"\n";
        return;
    }
    std::cout << "SYNC_STATUS valid=" << status.valid
              << " method=nearest_v4l2_monotonic_timestamp"
              << " diagnostic_only=1"
              << " cam0_sequence=" << status.cam0_sequence
              << " cam1_sequence=" << status.cam1_sequence
              << " cam0_flags=0x" << std::hex << status.cam0_buffer_flags
              << " cam1_flags=0x" << status.cam1_buffer_flags << std::dec
              << " cam0_timestamp_ns=" << status.cam0_timestamp_ns
              << " cam1_timestamp_ns=" << status.cam1_timestamp_ns
              << " delta_ns=" << status.delta_ns << '\n';
}

void print_xvs_controller_status(xvs_uart_controller_t *controller)
{
    if (!controller) {
        std::cout << "XVS_CONTROLLER_STATUS configured=0 connected=0\n";
        return;
    }

    xvs_uart_status_t status = {};
    const int result = xvs_uart_get_status(controller, &status);
    if (result != XVS_UART_OK) {
        std::cout << "ERROR command=sync-controller-status code=" << result
                  << " reason=\"" << xvs_uart_strerror(result) << "\"\n";
        return;
    }
    std::cout << "XVS_CONTROLLER_STATUS configured=1"
              << " connected=" << status.connected
              << " valid=" << status.valid
              << " state=" << status.state
              << " frequency_millihz=" << status.frequency_millihz
              << " low_pulse_us=" << status.low_pulse_us
              << " pulse_count=" << status.pulse_count
              << " last_trigger_id=" << status.last_trigger_id
              << " device=\"" << status.device << "\"\n";
}

void print_trigger_binding_status(trigger_frame_binder_t *binder)
{
    trigger_frame_binder_status_t status = {};
    const int result = trigger_frame_binder_get_status(binder, &status);
    if (result != TRIGGER_FRAME_BINDER_OK) {
        std::cout << "ERROR command=sync-bind-status code=" << result
                  << " reason=\"" << trigger_frame_binder_strerror(result)
                  << "\"\n";
        return;
    }
    std::cout << "TRIGGER_BIND_STATUS triggers=" << status.triggers_received
              << " ignored=" << status.triggers_ignored
              << " trigger_id_gaps=" << status.trigger_id_gaps
              << " duplicate_triggers=" << status.duplicate_triggers
              << " pending=" << status.pending_triggers
              << " pending_overflows="
              << status.pending_trigger_overflows
              << " cam0_frames=" << status.frames_received[0]
              << " cam0_bound=" << status.frames_bound[0]
              << " cam0_without_trigger="
              << status.frames_without_trigger[0]
              << " cam1_frames=" << status.frames_received[1]
              << " cam1_bound=" << status.frames_bound[1]
              << " cam1_without_trigger="
              << status.frames_without_trigger[1]
              << " complete_pairs=" << status.complete_pairs
              << " last_trigger_id=" << status.last_trigger_id
              << " last_completed_trigger_id="
              << status.last_completed_trigger_id
              << " last_frame_delta_ns=" << status.last_frame_delta_ns
              << " csv=\"" << status.csv_path << "\"\n";
}

void print_trigger_binding_last(trigger_frame_binder_t *binder)
{
    trigger_frame_binding_t binding = {};
    const int result = trigger_frame_binder_get_last_binding(binder, &binding);
    if (result != TRIGGER_FRAME_BINDER_OK) {
        std::cout << "ERROR command=sync-bind-last code=" << result
                  << " reason=\"" << trigger_frame_binder_strerror(result)
                  << "\"\n";
        return;
    }
    if (!binding.valid) {
        std::cout << "TRIGGER_BIND_LAST valid=0\n";
        return;
    }
    std::cout << "TRIGGER_BIND_LAST valid=1 trigger_id="
              << binding.trigger_id << " source=" << binding.source
              << " trigger_monotonic_ns=" << binding.trigger_monotonic_ns
              << " trigger_realtime_ns=" << binding.trigger_realtime_ns
              << " cam0_sequence=" << binding.cam0_sequence
              << " cam0_timestamp_ns=" << binding.cam0_timestamp_ns
              << " cam0_delay_ns=" << binding.cam0_delay_ns
              << " cam1_sequence=" << binding.cam1_sequence
              << " cam1_timestamp_ns=" << binding.cam1_timestamp_ns
              << " cam1_delay_ns=" << binding.cam1_delay_ns
              << " frame_delta_ns=" << binding.frame_delta_ns << '\n';
}

void print_trigger_simulator_status(trigger_simulator_t *simulator)
{
    trigger_simulator_status_t status = {};
    const int result = trigger_simulator_get_status(simulator, &status);
    if (result != TRIGGER_SIMULATOR_OK) {
        std::cout << "ERROR command=sync-sim-status code=" << result
                  << " reason=\"" << trigger_simulator_strerror(result)
                  << "\"\n";
        return;
    }
    std::cout << "SYNC_SIMULATOR_STATUS running=" << status.running
              << " physical_xvs=0 frequency_hz=" << status.frequency_hz
              << " requested_pulses=" << status.requested_count
              << " emitted_triggers=" << status.emitted_count
              << " last_trigger_id=" << status.last_trigger_id << '\n';
}

void capture_to_trigger_binder(int camera_id, uint32_t sequence,
                               uint32_t buffer_flags,
                               uint64_t v4l2_timestamp_ns,
                               uint64_t realtime_dequeue_ns,
                               void *user_data)
{
    auto *binder = static_cast<trigger_frame_binder_t *>(user_data);
    const trigger_frame_event_t frame = {
        camera_id, sequence, buffer_flags, v4l2_timestamp_ns,
        realtime_dequeue_ns,
    };
    const int result = trigger_frame_binder_on_frame(binder, &frame);
    if (result != TRIGGER_FRAME_BINDER_OK) {
        std::cerr << "TRIGGER_BIND_FRAME_ERROR camera_id=" << camera_id
                  << " code=" << result << " reason=\""
                  << trigger_frame_binder_strerror(result) << "\"\n";
    }
}

void simulator_to_trigger_binder(uint64_t trigger_id, uint64_t monotonic_ns,
                                 uint64_t realtime_ns, void *user_data)
{
    auto *binder = static_cast<trigger_frame_binder_t *>(user_data);
    const int result = trigger_frame_binder_on_trigger(
        binder, trigger_id, monotonic_ns, realtime_ns, "SIM");
    if (result != TRIGGER_FRAME_BINDER_OK) {
        std::cerr << "TRIGGER_BIND_SIM_ERROR trigger_id=" << trigger_id
                  << " code=" << result << " reason=\""
                  << trigger_frame_binder_strerror(result) << "\"\n";
    }
}

struct trigger_binding_self_test_context {
    trigger_frame_binder_t *binder = nullptr;
    std::atomic<int> callback_error{0};
};

void simulated_trigger_self_test(uint64_t trigger_id, uint64_t monotonic_ns,
                                 uint64_t realtime_ns, void *user_data)
{
    auto *context = static_cast<trigger_binding_self_test_context *>(user_data);
    int result = trigger_frame_binder_on_trigger(
        context->binder, trigger_id, monotonic_ns, realtime_ns, "SIM");
    if (result == TRIGGER_FRAME_BINDER_OK) {
        const trigger_frame_event_t cam1 = {
            1, static_cast<uint32_t>(100 + trigger_id), 0x2001,
            monotonic_ns + 1200000ULL, realtime_ns + 1200000ULL,
        };
        const trigger_frame_event_t cam0 = {
            0, static_cast<uint32_t>(200 + trigger_id), 0x2001,
            monotonic_ns + 1000000ULL, realtime_ns + 1000000ULL,
        };
        result = trigger_frame_binder_on_frame(context->binder, &cam1);
        if (result == TRIGGER_FRAME_BINDER_OK)
            result = trigger_frame_binder_on_frame(context->binder, &cam0);
    }
    if (result != TRIGGER_FRAME_BINDER_OK)
        context->callback_error.store(result);
}

bool run_trigger_binding_self_test()
{
    trigger_frame_binder_t *binder = nullptr;
    trigger_simulator_t *simulator = nullptr;
    trigger_frame_binder_config_t config = {};
    trigger_frame_binder_default_config(&config);
    config.max_pending_triggers = 8;
    config.completed_history_depth = 8;
    int result = trigger_frame_binder_create(&config, &binder);
    if (result != TRIGGER_FRAME_BINDER_OK) {
        std::cerr << "SYNC_BIND_SELF_TEST_FAILED stage=binder-create code="
                  << result << '\n';
        return false;
    }
    trigger_binding_self_test_context context = {binder};
    result = trigger_simulator_create(simulated_trigger_self_test, &context,
                                      &simulator);
    if (result != TRIGGER_SIMULATOR_OK) {
        std::cerr << "SYNC_BIND_SELF_TEST_FAILED stage=simulator-create code="
                  << result << '\n';
        trigger_frame_binder_destroy(binder);
        return false;
    }
    result = trigger_simulator_start(simulator, 2, 2);
    if (result == TRIGGER_SIMULATOR_OK) {
        for (int attempt = 0; attempt < 60; ++attempt) {
            trigger_simulator_status_t status = {};
            trigger_simulator_get_status(simulator, &status);
            if (!status.running && status.emitted_count == 2)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    trigger_simulator_stop(simulator);

    trigger_simulator_status_t simulator_status = {};
    trigger_frame_binder_status_t binder_status = {};
    trigger_frame_binding_t last = {};
    const bool passed =
        result == TRIGGER_SIMULATOR_OK && context.callback_error.load() == 0 &&
        trigger_simulator_get_status(simulator, &simulator_status) ==
            TRIGGER_SIMULATOR_OK &&
        trigger_frame_binder_get_status(binder, &binder_status) ==
            TRIGGER_FRAME_BINDER_OK &&
        trigger_frame_binder_get_last_binding(binder, &last) ==
            TRIGGER_FRAME_BINDER_OK &&
        simulator_status.emitted_count == 2 && binder_status.triggers_received == 2 &&
        binder_status.complete_pairs == 2 && binder_status.frames_bound[0] == 2 &&
        binder_status.frames_bound[1] == 2 && last.valid && last.trigger_id == 2 &&
        last.cam0_sequence == 202 && last.cam1_sequence == 102 &&
        last.frame_delta_ns == 200000;
    trigger_simulator_destroy(simulator);
    trigger_frame_binder_destroy(binder);
    if (!passed) {
        std::cerr << "SYNC_BIND_SELF_TEST_FAILED emitted="
                  << simulator_status.emitted_count << " triggers="
                  << binder_status.triggers_received << " pairs="
                  << binder_status.complete_pairs << " callback_error="
                  << context.callback_error.load() << '\n';
        return false;
    }
    std::cout << "SYNC_BIND_SELF_TEST_OK source=SIM triggers=2 pairs=2 "
                 "frame_delta_ns=200000\n";
    return true;
}

bool both_capture_streams_running(capture_backend_t *capture)
{
    for (int camera_id = 0; camera_id < CAPTURE_BACKEND_CAMERA_COUNT;
         ++camera_id) {
        capture_backend_status_t status = {};
        if (capture_backend_get_status(capture, camera_id, &status) !=
                CAPTURE_BACKEND_OK ||
            !status.running) {
            return false;
        }
    }
    return true;
}

void print_xvs_result(const char *command, int result)
{
    if (result == XVS_UART_OK) {
        std::cout << "OK command=" << command << '\n';
    } else {
        std::cout << "ERROR command=" << command << " code=" << result
                  << " reason=\"" << xvs_uart_strerror(result) << "\"\n";
    }
}

bool require_xvs_controller(const char *command,
                            xvs_uart_controller_t *controller)
{
    if (controller)
        return true;
    std::cout << "ERROR command=" << command
              << " reason=\"XVS MCU UART is not configured; restart with "
                 "--sync-uart DEVICE\"\n";
    return false;
}

void print_uvc_status(camera_uvc_backend_t *uvc, int camera_id)
{
    camera_uvc_status_t status = {};
    int result = camera_uvc_get_status(uvc, camera_id, &status);
    if (result != CAMERA_UVC_OK) {
        std::cout << "ERROR command=uvc-status camera_id=" << camera_id
                  << " code=" << result
                  << " reason=\"" << camera_uvc_strerror(result) << "\"\n";
        return;
    }

    char fcc[5] = {
        static_cast<char>(status.negotiated_fcc & 0xff),
        static_cast<char>((status.negotiated_fcc >> 8) & 0xff),
        static_cast<char>((status.negotiated_fcc >> 16) & 0xff),
        static_cast<char>((status.negotiated_fcc >> 24) & 0xff),
        '\0'};
    std::cout << "UVC_STATUS camera_id=" << camera_id
              << " enabled=" << status.enabled
              << " host_streaming=" << status.host_streaming
              << " source_camera_id=" << status.source_camera_id
              << " configured=" << status.width << 'x' << status.height
              << '@' << status.fps << "fps/MJPEG"
              << " negotiated=" << status.negotiated_width << 'x'
              << status.negotiated_height << '@' << status.negotiated_fps
              << "fps/" << (status.negotiated_fcc ? fcc : "none")
              << " submitted=" << status.frames_submitted
              << " encoded=" << status.frames_encoded
              << " sent=" << status.frames_sent
              << " skipped_no_host=" << status.frames_skipped_no_host
              << " rate_limited=" << status.frames_rate_limited
              << " queue_pending=" << status.queue_pending
              << " queue_drops=" << status.queue_drops
              << " encode_errors=" << status.encode_errors
              << " jpeg_bytes=" << status.jpeg_bytes
              << " last_sequence=" << status.last_sequence
              << " last_error=" << status.last_error
              << " last_mpp_error=" << status.last_mpp_error << '\n';
}

void print_net_status(camera_net_backend_t *net, int camera_id)
{
    camera_net_status_t status = {};
    int result = camera_net_get_camera_status(net, camera_id, &status);
    if (result != CAMERA_NET_OK) {
        std::cout << "ERROR command=net-status camera_id=" << camera_id
                  << " code=" << result
                  << " reason=\"" << camera_net_strerror(result) << "\"\n";
        return;
    }
    std::cout << "NET_STATUS camera_id=" << status.camera_id
              << " enabled=" << status.enabled
              << " server_running=" << status.server_running
              << " source_camera_id=" << status.source_camera_id
              << " configured=" << status.width << 'x' << status.height
              << '@' << status.fps << "fps/MJPEG"
              << " jpeg_quality=" << status.jpeg_quality
              << " port=" << status.port
              << " path=" << status.path
              << " clients=" << status.connected_clients
              << " submitted=" << status.frames_submitted
              << " encoded=" << status.frames_encoded
              << " sent=" << status.frames_sent
              << " queue_pending=" << status.queue_pending
              << " queue_drops=" << status.queue_drops
              << " encode_errors=" << status.encode_errors
              << " client_disconnects=" << status.client_disconnects
              << " http_errors=" << status.http_errors
              << " jpeg_bytes=" << status.jpeg_bytes
              << " last_sequence=" << status.last_sequence
              << " last_error=" << status.last_error
              << " last_mpp_error=" << status.last_mpp_error
              << " last_socket_errno=" << status.last_socket_errno << '\n';
}

void print_photo_status(camera_photo_backend_t *photo, int camera_id)
{
    camera_photo_status_t status = {};
    const int result = camera_photo_get_status(photo, camera_id, &status);
    if (result != CAMERA_PHOTO_OK) {
        std::cout << "ERROR command=photo-status camera_id=" << camera_id
                  << " code=" << result << " reason=\""
                  << camera_photo_strerror(result) << "\"\n";
        return;
    }
    std::cout
        << "PHOTO_STATUS camera_id=" << status.camera_id
        << " enabled=" << status.enabled
        << " processing=" << status.processing
        << " size=" << status.width << 'x' << status.height
        << " quality=" << status.jpeg_quality
        << " response_offset_ns=" << status.sensor_response_offset_ns
        << " submitted=" << status.frames_submitted
        << " saved=" << status.photos_saved
        << " queue_pending=" << status.queue_pending
        << " queue_drops=" << status.queue_drops
        << " unbound_frames=" << status.frames_without_trigger
        << " invalid_metadata=" << status.invalid_metadata
        << " encode_errors=" << status.encode_errors
        << " exif_errors=" << status.exif_errors
        << " write_errors=" << status.write_errors
        << " jpeg_bytes=" << status.jpeg_bytes
        << " last_frame_id=" << status.last_frame_id
        << " last_trigger_id=" << status.last_trigger_id
        << " last_error=" << status.last_error
        << " last_mpp_error=" << status.last_mpp_error
        << " last_errno=" << status.last_errno
        << " output_dir=\"" << status.output_dir << "\""
        << " metadata_csv=\"" << status.metadata_csv << "\""
        << " last_photo=\"" << status.last_photo << "\"\n";
}

struct output_backends {
    camera_uvc_backend_t *uvc = nullptr;
    camera_net_backend_t *net = nullptr;
    camera_backend_t *camera = nullptr;
    trigger_frame_binder_t *binder = nullptr;
    camera_photo_backend_t *photo = nullptr;
};

void capture_to_outputs(int camera_id, const void *plane0, size_t plane0_size,
                        const void *plane1, size_t plane1_size,
                        uint32_t sequence, void *user_data)
{
    output_backends *outputs = static_cast<output_backends *>(user_data);
    if (outputs->uvc) {
        camera_uvc_submit_nv12(outputs->uvc, camera_id, plane0, plane0_size,
                               plane1, plane1_size, sequence);
    }
    if (outputs->net) {
        camera_net_submit_nv12(outputs->net, camera_id, plane0, plane0_size,
                               plane1, plane1_size, sequence);
    }
    if (!outputs->photo || !outputs->binder || !outputs->camera ||
        !camera_photo_is_enabled(outputs->photo, camera_id))
        return;

    trigger_frame_match_t match = {};
    const int match_result = trigger_frame_binder_find_frame(
        outputs->binder, camera_id, sequence, &match);
    if (match_result != TRIGGER_FRAME_BINDER_OK || !match.valid) {
        camera_photo_note_unbound_frame(outputs->photo, camera_id);
        return;
    }

    camera_backend_status_t camera_status = {};
    const int status_result =
        camera_backend_get_status(outputs->camera, camera_id, &camera_status);
    camera_photo_metadata_t metadata = {};
    metadata.camera_id = camera_id;
    metadata.frame_id = sequence;
    metadata.trigger_id = match.trigger_id;
    metadata.trigger_monotonic_ns = match.trigger_monotonic_ns;
    metadata.trigger_realtime_ns = match.trigger_realtime_ns;
    metadata.frame_monotonic_ns = match.v4l2_timestamp_ns;
    metadata.frame_realtime_ns = match.realtime_dequeue_ns;
    metadata.trigger_to_frame_ns = match.trigger_to_frame_ns;
    camera_photo_get_response_offset(outputs->photo, camera_id,
                                     &metadata.sensor_response_offset_ns);
    metadata.exposure_start_realtime_ns = add_signed_ns(
        metadata.trigger_realtime_ns, metadata.sensor_response_offset_ns);
    if (status_result == CAMERA_BACKEND_OK) {
        metadata.exposure_us = camera_status.exposure_us;
        metadata.gain_x1000 = camera_status.gain_x1000;
        if (camera_status.iso > 0) {
            metadata.iso = static_cast<uint32_t>(camera_status.iso);
        } else {
            metadata.iso =
                std::max<uint32_t>(1, (camera_status.gain_x1000 + 5) / 10);
            metadata.iso_estimated = 1;
        }
    }
    metadata.exposure_center_realtime_ns =
        metadata.exposure_start_realtime_ns +
        static_cast<uint64_t>(metadata.exposure_us) * 500ULL;
    metadata.utc_valid =
        std::strcmp(match.source, "MCU_PPS") == 0 ? 1 : 0;
    std::snprintf(metadata.trigger_source,
                  sizeof(metadata.trigger_source), "%s", match.source);
    std::snprintf(metadata.exposure_source,
                  sizeof(metadata.exposure_source), "%s",
                  status_result == CAMERA_BACKEND_OK
                      ? "RKAIQ_LATEST_NOT_FRAME_BOUND"
                      : "UNAVAILABLE");
    camera_photo_submit_nv12(outputs->photo, camera_id, plane0, plane0_size,
                             plane1, plane1_size, &metadata);
}

void print_result(const char *command, int camera_id, int result)
{
    if (result == CAMERA_BACKEND_OK) {
        std::cout << "OK command=" << command << " camera_id=" << camera_id
                  << '\n';
    } else {
        std::cout << "ERROR command=" << command << " camera_id=" << camera_id
                  << " code=" << result
                  << " reason=\"" << camera_backend_strerror(result) << "\"\n";
    }
}

void print_capture_result(const char *command, int camera_id, int result)
{
    if (result == CAPTURE_BACKEND_OK) {
        std::cout << "OK command=" << command << " camera_id=" << camera_id
                  << '\n';
    } else {
        std::cout << "ERROR command=" << command
                  << " camera_id=" << camera_id << " code=" << result
                  << " reason=\"" << capture_backend_strerror(result)
                  << "\"\n";
    }
}

void invalid_command(const std::string &command, const char *usage)
{
    std::cout << "ERROR command=" << command << " reason=\"invalid syntax\""
              << " usage=\"" << usage << "\"\n";
}

bool execute_command(camera_backend_t *backend, capture_backend_t *capture,
                     camera_uvc_backend_t *uvc, camera_net_backend_t *net,
                     camera_photo_backend_t *photo,
                     xvs_uart_controller_t *xvs,
                     trigger_frame_binder_t *binder,
                     trigger_simulator_t *simulator,
                     const std::string &line)
{
    std::istringstream stream(line);
    std::string command;
    stream >> command;
    if (command.empty())
        return true;

    if (command == "quit" || command == "exit")
        return false;
    if (command == "help") {
        print_commands();
        return true;
    }

    if (command == "status") {
        std::string target = "all";
        std::string extra;
        stream >> target;
        if (stream >> extra) {
            invalid_command(command, "status [all|0|1]");
            return true;
        }
        if (target == "all") {
            for (int camera_id = 0; camera_id < CAMERA_BACKEND_CAMERA_COUNT;
                 ++camera_id) {
                print_status(backend, camera_id);
                print_capture_status(capture, camera_id);
            }
            for (int camera_id = 0; camera_id < CAMERA_UVC_CAMERA_COUNT;
                 ++camera_id)
                print_uvc_status(uvc, camera_id);
            for (int camera_id = 0;
                 camera_id < CAMERA_NET_CAMERA_COUNT; ++camera_id)
                print_net_status(net, camera_id);
            for (int camera_id = 0;
                 camera_id < CAMERA_PHOTO_CAMERA_COUNT; ++camera_id)
                print_photo_status(photo, camera_id);
        } else {
            int camera_id = -1;
            if (!parse_camera_id(target, &camera_id)) {
                invalid_command(command, "status [all|0|1]");
                return true;
            }
            print_status(backend, camera_id);
            print_capture_status(capture, camera_id);
            print_uvc_status(uvc, camera_id);
            print_net_status(net, camera_id);
            print_photo_status(photo, camera_id);
        }
        return true;
    }

    if (command == "capture-status") {
        std::string target = "all";
        std::string extra;
        stream >> target;
        if (stream >> extra) {
            invalid_command(command, "capture-status [all|0|1]");
            return true;
        }
        if (target == "all") {
            for (int camera_id = 0;
                 camera_id < CAPTURE_BACKEND_CAMERA_COUNT; ++camera_id) {
                print_capture_status(capture, camera_id);
            }
        } else {
            int camera_id = -1;
            if (!parse_camera_id(target, &camera_id)) {
                invalid_command(command, "capture-status [all|0|1]");
                return true;
            }
            print_capture_status(capture, camera_id);
        }
        return true;
    }

    if (command == "photo-status") {
        std::string target = "all";
        std::string extra;
        stream >> target;
        if (stream >> extra) {
            invalid_command(command, "photo-status [all|0|1]");
            return true;
        }
        if (target == "all") {
            for (int camera_id = 0;
                 camera_id < CAMERA_PHOTO_CAMERA_COUNT; ++camera_id)
                print_photo_status(photo, camera_id);
        } else {
            int camera_id = -1;
            if (!parse_camera_id(target, &camera_id)) {
                invalid_command(command, "photo-status [all|0|1]");
                return true;
            }
            print_photo_status(photo, camera_id);
        }
        return true;
    }

    if (command == "photo-start") {
        std::string camera_text;
        std::string output_dir;
        std::string extra;
        int camera_id = -1;
        if (!(stream >> camera_text >> output_dir) || (stream >> extra) ||
            !parse_camera_id(camera_text, &camera_id)) {
            invalid_command(command, "photo-start CAMERA_ID OUTPUT_DIR");
            return true;
        }
        capture_backend_status_t capture_status = {};
        const int capture_result = capture_backend_get_status(
            capture, camera_id, &capture_status);
        if (capture_result != CAPTURE_BACKEND_OK || !capture_status.running) {
            std::cout << "ERROR command=photo-start camera_id=" << camera_id
                      << " reason=\"camera capture is not running; use "
                         "stream-start "
                      << camera_id << " first\"\n";
            return true;
        }
        const int result =
            camera_photo_start(photo, camera_id, output_dir.c_str());
        if (result == CAMERA_PHOTO_OK) {
            std::cout << "OK command=photo-start camera_id=" << camera_id
                      << " output_dir=\"" << output_dir << "\""
                      << " trigger_required=1"
                      << " exposure_source=RKAIQ_LATEST_NOT_FRAME_BOUND\n";
        } else {
            std::cout << "ERROR command=photo-start camera_id=" << camera_id
                      << " code=" << result << " reason=\""
                      << camera_photo_strerror(result) << "\"\n";
        }
        return true;
    }

    if (command == "photo-stop") {
        std::string camera_text;
        std::string extra;
        int camera_id = -1;
        if (!(stream >> camera_text) || (stream >> extra) ||
            !parse_camera_id(camera_text, &camera_id)) {
            invalid_command(command, "photo-stop CAMERA_ID");
            return true;
        }
        const int result = camera_photo_stop(photo, camera_id);
        if (result == CAMERA_PHOTO_OK) {
            std::cout << "OK command=photo-stop camera_id=" << camera_id
                      << '\n';
        } else {
            std::cout << "ERROR command=photo-stop camera_id=" << camera_id
                      << " code=" << result << " reason=\""
                      << camera_photo_strerror(result) << "\"\n";
        }
        return true;
    }

    if (command == "photo-offset") {
        std::string camera_text;
        std::string offset_text;
        std::string extra;
        int camera_id = -1;
        int64_t offset_ns = 0;
        if (!(stream >> camera_text >> offset_text) || (stream >> extra) ||
            !parse_camera_id(camera_text, &camera_id) ||
            !parse_i64(offset_text, &offset_ns)) {
            invalid_command(command,
                            "photo-offset CAMERA_ID SENSOR_RESPONSE_OFFSET_NS");
            return true;
        }
        const int result =
            camera_photo_set_response_offset(photo, camera_id, offset_ns);
        if (result == CAMERA_PHOTO_OK) {
            std::cout << "OK command=photo-offset camera_id=" << camera_id
                      << " sensor_response_offset_ns=" << offset_ns << '\n';
        } else {
            std::cout << "ERROR command=photo-offset camera_id=" << camera_id
                      << " code=" << result << " reason=\""
                      << camera_photo_strerror(result) << "\"\n";
        }
        return true;
    }

    if (command == "uvc-status") {
        std::string target = "all";
        std::string extra;
        stream >> target;
        if (stream >> extra) {
            invalid_command(command, "uvc-status [CAMERA_ID|all]");
            return true;
        }
        if (target == "all") {
            for (int camera_id = 0; camera_id < CAMERA_UVC_CAMERA_COUNT;
                 ++camera_id)
                print_uvc_status(uvc, camera_id);
        } else {
            int camera_id = -1;
            if (!parse_camera_id(target, &camera_id)) {
                invalid_command(command, "uvc-status [CAMERA_ID|all]");
                return true;
            }
            print_uvc_status(uvc, camera_id);
        }
        return true;
    }

    if (command == "uvc-start") {
        std::string target;
        std::string extra;
        if (!(stream >> target) || (stream >> extra)) {
            invalid_command(command, "uvc-start CAMERA_ID|all");
            return true;
        }

        int first_camera = 0;
        int camera_count = CAMERA_UVC_CAMERA_COUNT;
        int camera_id = -1;
        if (target != "all") {
            if (!parse_camera_id(target, &camera_id)) {
                invalid_command(command, "uvc-start CAMERA_ID|all");
                return true;
            }
            first_camera = camera_id;
            camera_count = 1;
        }

        for (int offset = 0; offset < camera_count; ++offset) {
            const int id = first_camera + offset;
            capture_backend_status_t capture_status = {};
            const int capture_result = capture_backend_get_status(
                capture, id, &capture_status);
            if (capture_result != CAPTURE_BACKEND_OK ||
                !capture_status.running) {
                std::cout << "ERROR command=uvc-start camera_id=" << id
                          << " reason=\"camera capture is not running; use "
                             "stream-start " << id << " first\"\n";
                return true;
            }
        }

        const int result = target == "all"
                               ? camera_uvc_start_all(uvc)
                               : camera_uvc_start(uvc, camera_id);
        if (result == CAMERA_UVC_OK) {
            std::cout << "OK command=uvc-start target=" << target
                      << " mode=4000x3000@10fps/MJPEG\n";
        } else {
            std::cout << "ERROR command=uvc-start target=" << target
                      << " code="
                      << result << " reason=\""
                      << camera_uvc_strerror(result) << "\"\n";
        }
        return true;
    }

    if (command == "uvc-stop") {
        std::string extra;
        if (stream >> extra) {
            invalid_command(command, "uvc-stop");
            return true;
        }
        const int result = camera_uvc_stop(uvc);
        if (result == CAMERA_UVC_OK) {
            std::cout << "OK command=uvc-stop\n";
        } else {
            std::cout << "ERROR command=uvc-stop code=" << result
                      << " reason=\"" << camera_uvc_strerror(result)
                      << "\"\n";
        }
        return true;
    }

    if (command == "net-status") {
        std::string target = "all";
        std::string extra;
        stream >> target;
        if (stream >> extra) {
            invalid_command(command, "net-status [CAMERA_ID|all]");
            return true;
        }
        if (target == "all") {
            for (int camera_id = 0;
                 camera_id < CAMERA_NET_CAMERA_COUNT; ++camera_id)
                print_net_status(net, camera_id);
        } else {
            int camera_id = -1;
            if (!parse_camera_id(target, &camera_id)) {
                invalid_command(command, "net-status [CAMERA_ID|all]");
                return true;
            }
            print_net_status(net, camera_id);
        }
        return true;
    }

    if (command == "net-start") {
        std::string camera_text;
        std::string extra;
        int camera_id = -1;
        if (!(stream >> camera_text) || (stream >> extra) ||
            !parse_camera_id(camera_text, &camera_id)) {
            invalid_command(command, "net-start CAMERA_ID");
            return true;
        }
        capture_backend_status_t capture_status = {};
        int result = capture_backend_get_status(capture, camera_id,
                                                &capture_status);
        if (result != CAPTURE_BACKEND_OK || !capture_status.running) {
            std::cout << "ERROR command=net-start camera_id=" << camera_id
                      << " reason=\"camera " << camera_id
                      << " capture is not running; use stream-start "
                      << camera_id << " first\"\n";
            return true;
        }
        result = camera_net_start(net, camera_id);
        if (result == CAMERA_NET_OK) {
            std::cout << "OK command=net-start camera_id=" << camera_id
                      << " mode=4000x3000@10fps/MJPEG"
                      << " url=http://<board-ip>:8080/cam" << camera_id
                      << '\n';
        } else {
            std::cout << "ERROR command=net-start camera_id=" << camera_id
                      << " code="
                      << result << " reason=\""
                      << camera_net_strerror(result) << "\"\n";
        }
        return true;
    }

    if (command == "net-stop") {
        std::string target = "all";
        std::string extra;
        stream >> target;
        if (stream >> extra) {
            invalid_command(command, "net-stop [CAMERA_ID|all]");
            return true;
        }
        if (target == "all") {
            const int result = camera_net_stop(net);
            if (result == CAMERA_NET_OK) {
                std::cout << "OK command=net-stop target=all\n";
            } else {
                std::cout << "ERROR command=net-stop target=all code="
                          << result << " reason=\""
                          << camera_net_strerror(result) << "\"\n";
            }
        } else {
            int camera_id = -1;
            if (!parse_camera_id(target, &camera_id)) {
                invalid_command(command, "net-stop [CAMERA_ID|all]");
                return true;
            }
            const int result = camera_net_stop_camera(net, camera_id);
            if (result == CAMERA_NET_OK) {
                std::cout << "OK command=net-stop camera_id=" << camera_id
                          << '\n';
            } else {
                std::cout << "ERROR command=net-stop camera_id=" << camera_id
                          << " code=" << result << " reason=\""
                          << camera_net_strerror(result) << "\"\n";
            }
        }
        return true;
    }

    if (command == "sync-idle" || command == "sync-stop") {
        std::string extra;
        if (stream >> extra) {
            invalid_command(command,
                            command == "sync-idle" ? "sync-idle"
                                                   : "sync-stop");
            return true;
        }
        if (!require_xvs_controller(command.c_str(), xvs))
            return true;
        print_xvs_result(command.c_str(),
                         command == "sync-idle" ? xvs_uart_idle(xvs)
                                                : xvs_uart_stop(xvs));
        return true;
    }

    if (command == "sync-start") {
        std::string frequency_text;
        std::string low_pulse_text;
        std::string extra;
        uint32_t frequency_hz = 0;
        uint32_t low_pulse_us = 10;
        if (!(stream >> frequency_text) ||
            !parse_u32(frequency_text, &frequency_hz) ||
            ((stream >> low_pulse_text) &&
             !parse_u32(low_pulse_text, &low_pulse_us)) ||
            (stream >> extra)) {
            invalid_command(command, "sync-start 2|4 [LOW_PULSE_US]");
            return true;
        }
        if (!require_xvs_controller(command.c_str(), xvs))
            return true;
        if (!both_capture_streams_running(capture)) {
            std::cout << "ERROR command=sync-start reason=\"both capture "
                         "streams must be running; use stream-start all\"\n";
            return true;
        }
        const int result = xvs_uart_start(xvs, frequency_hz, low_pulse_us);
        print_xvs_result(command.c_str(), result);
        if (result == XVS_UART_OK) {
            std::cout << "XVS_OUTPUT state=RUNNING frequency_hz="
                      << frequency_hz << " low_pulse_us=" << low_pulse_us
                      << '\n';
        }
        return true;
    }

    if (command == "sync-count") {
        std::string frequency_text;
        std::string pulse_count_text;
        std::string low_pulse_text;
        std::string extra;
        uint32_t frequency_hz = 0;
        uint32_t pulse_count = 0;
        uint32_t low_pulse_us = 10;
        if (!(stream >> frequency_text >> pulse_count_text) ||
            !parse_u32(frequency_text, &frequency_hz) ||
            !parse_u32(pulse_count_text, &pulse_count) ||
            ((stream >> low_pulse_text) &&
             !parse_u32(low_pulse_text, &low_pulse_us)) ||
            (stream >> extra)) {
            invalid_command(
                command, "sync-count 2|4 PULSE_COUNT [LOW_PULSE_US]");
            return true;
        }
        if (!require_xvs_controller(command.c_str(), xvs))
            return true;
        if (!both_capture_streams_running(capture)) {
            std::cout << "ERROR command=sync-count reason=\"both capture "
                         "streams must be running; use stream-start all\"\n";
            return true;
        }
        const int result = xvs_uart_count(xvs, frequency_hz, low_pulse_us,
                                          pulse_count);
        print_xvs_result(command.c_str(), result);
        if (result == XVS_UART_OK) {
            std::cout << "XVS_OUTPUT state=COUNTING frequency_hz="
                      << frequency_hz << " low_pulse_us=" << low_pulse_us
                      << " requested_pulses=" << pulse_count << '\n';
        }
        return true;
    }

    if (command == "sync-controller-status") {
        std::string extra;
        if (stream >> extra) {
            invalid_command(command, "sync-controller-status");
            return true;
        }
        print_xvs_controller_status(xvs);
        return true;
    }

    if (command == "sync-bind-reset") {
        std::string ignored_text;
        std::string extra;
        uint32_t ignored_triggers = 0;
        if ((stream >> ignored_text) &&
            !parse_u32(ignored_text, &ignored_triggers)) {
            invalid_command(command, "sync-bind-reset [PRE_SHUTTER_TRIGGERS]");
            return true;
        }
        if (stream >> extra) {
            invalid_command(command, "sync-bind-reset [PRE_SHUTTER_TRIGGERS]");
            return true;
        }
        const int result = trigger_frame_binder_reset(binder, ignored_triggers);
        if (result == TRIGGER_FRAME_BINDER_OK) {
            std::cout << "OK command=sync-bind-reset ignored_triggers="
                      << ignored_triggers << '\n';
        } else {
            std::cout << "ERROR command=sync-bind-reset code=" << result
                      << " reason=\"" << trigger_frame_binder_strerror(result)
                      << "\"\n";
        }
        return true;
    }

    if (command == "sync-bind-log") {
        std::string path;
        std::string extra;
        if (!(stream >> path) || (stream >> extra)) {
            invalid_command(command, "sync-bind-log CSV_PATH|off");
            return true;
        }
        const char *selected_path = path == "off" ? nullptr : path.c_str();
        const int result = trigger_frame_binder_set_csv_path(binder,
                                                              selected_path);
        if (result == TRIGGER_FRAME_BINDER_OK) {
            std::cout << "OK command=sync-bind-log path=\""
                      << (selected_path ? selected_path : "") << "\"\n";
        } else {
            std::cout << "ERROR command=sync-bind-log code=" << result
                      << " reason=\"" << trigger_frame_binder_strerror(result)
                      << "\"\n";
        }
        return true;
    }

    if (command == "sync-bind-status") {
        std::string extra;
        if (stream >> extra) {
            invalid_command(command, "sync-bind-status");
            return true;
        }
        print_trigger_binding_status(binder);
        return true;
    }

    if (command == "sync-bind-last") {
        std::string extra;
        if (stream >> extra) {
            invalid_command(command, "sync-bind-last");
            return true;
        }
        print_trigger_binding_last(binder);
        return true;
    }

    if (command == "sync-sim-start") {
        std::string frequency_text;
        std::string count_text;
        std::string extra;
        uint32_t frequency_hz = 0;
        uint32_t pulse_count = 0;
        if (!(stream >> frequency_text) ||
            !parse_u32(frequency_text, &frequency_hz) ||
            ((stream >> count_text) && !parse_u32(count_text, &pulse_count)) ||
            (stream >> extra) || (frequency_hz != 2 && frequency_hz != 4)) {
            invalid_command(command, "sync-sim-start 2|4 [PULSE_COUNT]");
            return true;
        }
        if (!both_capture_streams_running(capture)) {
            std::cout << "ERROR command=sync-sim-start reason=\"both capture "
                         "streams must be running; use stream-start all\"\n";
            return true;
        }
        const int result = trigger_simulator_start(simulator, frequency_hz,
                                                   pulse_count);
        if (result == TRIGGER_SIMULATOR_OK) {
            std::cout << "OK command=sync-sim-start frequency_hz="
                      << frequency_hz << " requested_pulses=" << pulse_count
                      << " physical_xvs=0 source=SIM\n";
        } else {
            std::cout << "ERROR command=sync-sim-start code=" << result
                      << " reason=\"" << trigger_simulator_strerror(result)
                      << "\"\n";
        }
        return true;
    }

    if (command == "sync-sim-stop") {
        std::string extra;
        if (stream >> extra) {
            invalid_command(command, "sync-sim-stop");
            return true;
        }
        const int result = trigger_simulator_stop(simulator);
        if (result == TRIGGER_SIMULATOR_OK) {
            std::cout << "OK command=sync-sim-stop physical_xvs=0\n";
        } else {
            std::cout << "ERROR command=sync-sim-stop code=" << result
                      << " reason=\"" << trigger_simulator_strerror(result)
                      << "\"\n";
        }
        return true;
    }

    if (command == "sync-sim-status") {
        std::string extra;
        if (stream >> extra) {
            invalid_command(command, "sync-sim-status");
            return true;
        }
        print_trigger_simulator_status(simulator);
        return true;
    }

    if (command == "sync-status") {
        std::string extra;
        if (stream >> extra) {
            invalid_command(command, "sync-status");
            return true;
        }
        print_xvs_controller_status(xvs);
        print_sync_status(capture);
        print_trigger_binding_status(binder);
        print_trigger_simulator_status(simulator);
        return true;
    }

    if (command == "stream-start" || command == "stream-stop") {
        std::string target;
        std::string extra;
        if (!(stream >> target) || (stream >> extra)) {
            invalid_command(command,
                            command == "stream-start"
                                ? "stream-start CAMERA_ID|all"
                                : "stream-stop CAMERA_ID|all");
            return true;
        }
        if (command == "stream-stop") {
            trigger_simulator_status_t simulator_status = {};
            if (trigger_simulator_get_status(simulator, &simulator_status) ==
                    TRIGGER_SIMULATOR_OK && simulator_status.running) {
                trigger_simulator_stop(simulator);
                std::cout << "SYNC_SIMULATOR_STOPPED reason=capture-stop\n";
            }
        }
        if (target == "all") {
            for (int camera_id = 0;
                 camera_id < CAPTURE_BACKEND_CAMERA_COUNT; ++camera_id) {
                if (command == "stream-stop" &&
                    camera_photo_is_enabled(photo, camera_id)) {
                    const int photo_result = camera_photo_stop(photo, camera_id);
                    if (photo_result == CAMERA_PHOTO_OK) {
                        std::cout << "PHOTO_STOPPED camera_id=" << camera_id
                                  << " reason=capture-stop\n";
                    }
                }
                int result = command == "stream-start"
                                 ? capture_backend_start_stream(capture,
                                                                camera_id)
                                 : capture_backend_stop_stream(capture,
                                                               camera_id);
                print_capture_result(command.c_str(), camera_id, result);
            }
        } else {
            int camera_id = -1;
            if (!parse_camera_id(target, &camera_id)) {
                invalid_command(command,
                                command == "stream-start"
                                    ? "stream-start CAMERA_ID|all"
                                    : "stream-stop CAMERA_ID|all");
                return true;
            }
            if (command == "stream-stop" &&
                camera_photo_is_enabled(photo, camera_id)) {
                const int photo_result = camera_photo_stop(photo, camera_id);
                if (photo_result == CAMERA_PHOTO_OK) {
                    std::cout << "PHOTO_STOPPED camera_id=" << camera_id
                              << " reason=capture-stop\n";
                }
            }
            int result = command == "stream-start"
                             ? capture_backend_start_stream(capture, camera_id)
                             : capture_backend_stop_stream(capture, camera_id);
            print_capture_result(command.c_str(), camera_id, result);
        }
        return true;
    }

    if (command == "save-start") {
        std::string camera_text;
        std::string output_dir;
        std::string extra;
        int camera_id = -1;
        if (!(stream >> camera_text >> output_dir) || (stream >> extra) ||
            !parse_camera_id(camera_text, &camera_id)) {
            invalid_command(command, "save-start CAMERA_ID OUTPUT_DIR");
            return true;
        }
        print_capture_result(
            command.c_str(), camera_id,
            capture_backend_start_save(capture, camera_id,
                                       output_dir.c_str()));
        return true;
    }

    if (command == "save-stop") {
        std::string camera_text;
        std::string extra;
        int camera_id = -1;
        if (!(stream >> camera_text) || (stream >> extra) ||
            !parse_camera_id(camera_text, &camera_id)) {
            invalid_command(command, "save-stop CAMERA_ID");
            return true;
        }
        print_capture_result(command.c_str(), camera_id,
                             capture_backend_stop_save(capture, camera_id));
        return true;
    }

    if (command == "auto") {
        std::string camera_text;
        std::string extra;
        int camera_id = -1;
        if (!(stream >> camera_text) || (stream >> extra) ||
            !parse_camera_id(camera_text, &camera_id)) {
            invalid_command(command, "auto CAMERA_ID");
            return true;
        }
        print_result(command.c_str(), camera_id,
                     camera_backend_set_auto(backend, camera_id));
        return true;
    }

    if (command == "exposure") {
        std::string camera_text;
        std::string exposure_text;
        std::string extra;
        int camera_id = -1;
        uint32_t exposure_us = 0;
        if (!(stream >> camera_text >> exposure_text) || (stream >> extra) ||
            !parse_camera_id(camera_text, &camera_id) ||
            !parse_u32(exposure_text, &exposure_us)) {
            invalid_command(command, "exposure CAMERA_ID EXPOSURE_US");
            return true;
        }
        print_result(command.c_str(), camera_id,
                     camera_backend_set_exposure(backend, camera_id,
                                                 exposure_us));
        return true;
    }

    if (command == "gain") {
        std::string camera_text;
        std::string gain_text;
        std::string extra;
        int camera_id = -1;
        uint32_t gain_x1000 = 0;
        if (!(stream >> camera_text >> gain_text) || (stream >> extra) ||
            !parse_camera_id(camera_text, &camera_id) ||
            !parse_u32(gain_text, &gain_x1000)) {
            invalid_command(command, "gain CAMERA_ID GAIN_X1000");
            return true;
        }
        print_result(command.c_str(), camera_id,
                     camera_backend_set_gain(backend, camera_id,
                                             gain_x1000));
        return true;
    }

    if (command == "fps") {
        std::string camera_text;
        std::string fps_text;
        std::string extra;
        int camera_id = -1;
        uint32_t fps = 0;
        if (!(stream >> camera_text >> fps_text) || (stream >> extra) ||
            !parse_camera_id(camera_text, &camera_id) ||
            !parse_u32(fps_text, &fps)) {
            invalid_command(command, "fps CAMERA_ID FPS");
            return true;
        }
        print_result(command.c_str(), camera_id,
                     camera_backend_set_fps(backend, camera_id, fps));
        return true;
    }

    if (command == "wait") {
        std::string milliseconds_text;
        std::string extra;
        uint32_t milliseconds = 0;
        if (!(stream >> milliseconds_text) || (stream >> extra) ||
            !parse_u32(milliseconds_text, &milliseconds)) {
            invalid_command(command, "wait MILLISECONDS");
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        std::cout << "OK command=wait milliseconds=" << milliseconds << '\n';
        return true;
    }

    std::cout << "ERROR command=" << command << " reason=\"unknown command\"\n";
    return true;
}

bool autostart_http_outputs(capture_backend_t *capture,
                            camera_net_backend_t *net)
{
    int captures_started = 0;
    for (int camera_id = 0; camera_id < CAPTURE_BACKEND_CAMERA_COUNT;
         ++camera_id) {
        const int result = capture_backend_start_stream(capture, camera_id);
        if (result != CAPTURE_BACKEND_OK) {
            std::cerr << "HTTP_AUTOSTART_FAILED stage=capture camera_id="
                      << camera_id << " code=" << result << " reason=\""
                      << capture_backend_strerror(result) << "\"\n";
            for (int started = 0; started < captures_started; ++started)
                capture_backend_stop_stream(capture, started);
            return false;
        }
        captures_started++;
    }

    int networks_started = 0;
    for (int camera_id = 0; camera_id < CAMERA_NET_CAMERA_COUNT;
         ++camera_id) {
        const int result = camera_net_start(net, camera_id);
        if (result != CAMERA_NET_OK) {
            std::cerr << "HTTP_AUTOSTART_FAILED stage=network camera_id="
                      << camera_id << " code=" << result << " reason=\""
                      << camera_net_strerror(result) << "\"\n";
            for (int started = 0; started < networks_started; ++started)
                camera_net_stop_camera(net, started);
            for (int started = 0; started < captures_started; ++started)
                capture_backend_stop_stream(capture, started);
            return false;
        }
        networks_started++;
    }
    std::cout << "HTTP_AUTOSTART_READY cam0=http://<board-ip>:8080/cam0"
              << " cam1=http://<board-ip>:8080/cam1"
              << " index=http://<board-ip>:8080/\n";
    return true;
}

bool autostart_uvc_output(capture_backend_t *capture,
                          camera_uvc_backend_t *uvc)
{
    int started = 0;
    for (; started < CAMERA_UVC_CAMERA_COUNT; ++started) {
        const int result = capture_backend_start_stream(capture, started);
        if (result != CAPTURE_BACKEND_OK) {
            std::cerr << "UVC_AUTOSTART_FAILED stage=capture camera_id="
                      << started << " code=" << result << " reason=\""
                      << capture_backend_strerror(result) << "\"\n";
            while (started > 0)
                capture_backend_stop_stream(capture, --started);
            return false;
        }
    }

    const int result = camera_uvc_start_all(uvc);
    if (result != CAMERA_UVC_OK) {
        std::cerr << "UVC_AUTOSTART_FAILED stage=uvc target=all code="
                  << result << " reason=\""
                  << camera_uvc_strerror(result) << "\"\n";
        for (int camera_id = 0; camera_id < CAMERA_UVC_CAMERA_COUNT;
             ++camera_id)
            capture_backend_stop_stream(capture, camera_id);
        return false;
    }

    std::cout << "UVC_AUTOSTART_READY cameras=0,1"
              << " outputs=2 mode=4000x3000@10fps/MJPEG\n";
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    camera_backend_config_t config = {};
    camera_backend_default_config(&config);
    capture_backend_config_t capture_config = {};
    capture_backend_default_config(&capture_config);
    std::string iq_dirs[CAMERA_BACKEND_CAMERA_COUNT];
    std::string expected_sensors[CAMERA_BACKEND_CAMERA_COUNT];
    std::string params_devices[CAMERA_BACKEND_CAMERA_COUNT];
    std::string video_devices[CAPTURE_BACKEND_CAMERA_COUNT];
    bool autostart = false;
    bool uvc_autostart = false;
    bool daemon_mode = false;
    bool sync_protocol_self_test = false;
    bool sync_bind_self_test = false;
    bool photo_exif_self_test = false;
    std::string sync_uart_device;

    for (int index = 1; index < argc; ++index) {
        std::string option = argv[index];
        if (option == "--help") {
            print_usage(argv[0]);
            print_commands();
            return EXIT_SUCCESS;
        }
        if (option == "--autostart") {
            autostart = true;
            continue;
        }
        if (option == "--daemon") {
            autostart = true;
            daemon_mode = true;
            continue;
        }
        if (option == "--uvc-daemon") {
            uvc_autostart = true;
            daemon_mode = true;
            continue;
        }
        if (option == "--sync-protocol-self-test") {
            sync_protocol_self_test = true;
            continue;
        }
        if (option == "--sync-bind-self-test") {
            sync_bind_self_test = true;
            continue;
        }
        if (option == "--photo-exif-self-test") {
            photo_exif_self_test = true;
            continue;
        }
        if (index + 1 >= argc) {
            std::cerr << "Missing value for " << option << '\n';
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }

        std::string value = argv[++index];
        if (option == "--width") {
            if (!parse_u32(value, &config.width) || config.width == 0) {
                std::cerr << "Invalid width: " << value << '\n';
                return EXIT_FAILURE;
            }
        } else if (option == "--height") {
            if (!parse_u32(value, &config.height) || config.height == 0) {
                std::cerr << "Invalid height: " << value << '\n';
                return EXIT_FAILURE;
            }
        } else if (option == "--iq0" || option == "--iq1") {
            int camera_id = option == "--iq0" ? 0 : 1;
            iq_dirs[camera_id] = value;
            config.iq_dir[camera_id] = iq_dirs[camera_id].c_str();
        } else if (option == "--params0" || option == "--params1") {
            int camera_id = option == "--params0" ? 0 : 1;
            params_devices[camera_id] = value;
            config.params_device[camera_id] = params_devices[camera_id].c_str();
        } else if (option == "--video0" || option == "--video1") {
            int camera_id = option == "--video0" ? 0 : 1;
            video_devices[camera_id] = value;
            capture_config.video_device[camera_id] =
                video_devices[camera_id].c_str();
        } else if (option == "--sensor0" || option == "--sensor1") {
            int camera_id = option == "--sensor0" ? 0 : 1;
            expected_sensors[camera_id] = value;
            config.expected_sensor[camera_id] =
                expected_sensors[camera_id].c_str();
        } else if (option == "--sync-uart") {
            sync_uart_device = value;
        } else {
            std::cerr << "Unknown option: " << option << '\n';
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (sync_protocol_self_test) {
        const int protocol_result = xvs_uart_protocol_self_test();
        if (protocol_result != XVS_UART_OK) {
            std::cerr << "XVS_PROTOCOL_SELF_TEST_FAILED code="
                      << protocol_result << " reason=\""
                      << xvs_uart_strerror(protocol_result) << "\"\n";
            return EXIT_FAILURE;
        }
        std::cout << "XVS_PROTOCOL_SELF_TEST_OK\n";
        return EXIT_SUCCESS;
    }
    if (sync_bind_self_test) {
        return run_trigger_binding_self_test() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (photo_exif_self_test) {
        std::string report;
        const int photo_result = camera_photo::self_test(&report);
        if (photo_result != camera_photo::EXIF_OK) {
            std::cerr << "PHOTO_EXIF_SELF_TEST_FAILED code=" << photo_result
                      << " reason=\"" << report << "\"\n";
            return EXIT_FAILURE;
        }
        std::cout << "PHOTO_EXIF_SELF_TEST_OK detail=\"" << report
                  << "\"\n";
        return EXIT_SUCCESS;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);

    capture_config.width = config.width;
    capture_config.height = config.height;

    camera_backend_t *backend = nullptr;
    int result = camera_backend_create(&config, &backend);
    if (result != CAMERA_BACKEND_OK) {
        std::cerr << "CAMERA_BACKEND_START_FAILED code=" << result
                  << " reason=\"" << camera_backend_strerror(result) << "\"\n";
        return EXIT_FAILURE;
    }

    capture_backend_t *capture = nullptr;
    result = capture_backend_create(&capture_config, &capture);
    if (result != CAPTURE_BACKEND_OK) {
        std::cerr << "CAPTURE_BACKEND_START_FAILED code=" << result
                  << " reason=\"" << capture_backend_strerror(result)
                  << "\"\n";
        camera_backend_destroy(backend);
        return EXIT_FAILURE;
    }

    camera_uvc_config_t uvc_config = {};
    camera_uvc_default_config(&uvc_config);
    camera_uvc_backend_t *uvc = nullptr;
    result = camera_uvc_create(&uvc_config, &uvc);
    if (result != CAMERA_UVC_OK) {
        std::cerr << "UVC_BACKEND_START_FAILED code=" << result
                  << " reason=\"" << camera_uvc_strerror(result) << "\"\n";
        capture_backend_destroy(capture);
        camera_backend_destroy(backend);
        return EXIT_FAILURE;
    }

    camera_net_config_t net_config = {};
    camera_net_default_config(&net_config);
    camera_net_backend_t *net = nullptr;
    result = camera_net_create(&net_config, &net);
    if (result != CAMERA_NET_OK) {
        std::cerr << "NET_BACKEND_START_FAILED code=" << result
                  << " reason=\"" << camera_net_strerror(result) << "\"\n";
        camera_uvc_destroy(uvc);
        capture_backend_destroy(capture);
        camera_backend_destroy(backend);
        return EXIT_FAILURE;
    }

    camera_photo_config_t photo_config = {};
    camera_photo_default_config(&photo_config);
    photo_config.width = config.width;
    photo_config.height = config.height;
    camera_photo_backend_t *photo = nullptr;
    result = camera_photo_create(&photo_config, &photo);
    if (result != CAMERA_PHOTO_OK) {
        std::cerr << "PHOTO_BACKEND_START_FAILED code=" << result
                  << " reason=\"" << camera_photo_strerror(result) << "\"\n";
        camera_net_destroy(net);
        camera_uvc_destroy(uvc);
        capture_backend_destroy(capture);
        camera_backend_destroy(backend);
        return EXIT_FAILURE;
    }

    output_backends outputs = {uvc, net, backend, nullptr, photo};
    int callback_camera_id = 0;
    for (; callback_camera_id < CAPTURE_BACKEND_CAMERA_COUNT;
         ++callback_camera_id) {
        result = capture_backend_set_frame_callback(
            capture, callback_camera_id, capture_to_outputs, &outputs);
        if (result != CAPTURE_BACKEND_OK)
            break;
    }
    if (callback_camera_id != CAPTURE_BACKEND_CAMERA_COUNT) {
        std::cerr << "OUTPUT_CAPTURE_CALLBACK_FAILED camera_id="
                  << callback_camera_id << " code=" << result
                  << " reason=\"" << capture_backend_strerror(result)
                  << "\"\n";
        camera_photo_destroy(photo);
        camera_net_destroy(net);
        camera_uvc_destroy(uvc);
        capture_backend_destroy(capture);
        camera_backend_destroy(backend);
        return EXIT_FAILURE;
    }

    trigger_frame_binder_config_t binder_config = {};
    trigger_frame_binder_default_config(&binder_config);
    trigger_frame_binder_t *binder = nullptr;
    result = trigger_frame_binder_create(&binder_config, &binder);
    if (result != TRIGGER_FRAME_BINDER_OK) {
        std::cerr << "TRIGGER_BINDER_START_FAILED code=" << result
                  << " reason=\"" << trigger_frame_binder_strerror(result)
                  << "\"\n";
        camera_photo_destroy(photo);
        camera_net_destroy(net);
        camera_uvc_destroy(uvc);
        capture_backend_destroy(capture);
        camera_backend_destroy(backend);
        return EXIT_FAILURE;
    }

    trigger_simulator_t *simulator = nullptr;
    result = trigger_simulator_create(simulator_to_trigger_binder, binder,
                                      &simulator);
    if (result != TRIGGER_SIMULATOR_OK) {
        std::cerr << "TRIGGER_SIMULATOR_START_FAILED code=" << result
                  << " reason=\"" << trigger_simulator_strerror(result)
                  << "\"\n";
        trigger_frame_binder_destroy(binder);
        camera_photo_destroy(photo);
        camera_net_destroy(net);
        camera_uvc_destroy(uvc);
        capture_backend_destroy(capture);
        camera_backend_destroy(backend);
        return EXIT_FAILURE;
    }
    outputs.binder = binder;

    int event_callback_camera_id = 0;
    for (; event_callback_camera_id < CAPTURE_BACKEND_CAMERA_COUNT;
         ++event_callback_camera_id) {
        result = capture_backend_set_frame_event_callback(
            capture, event_callback_camera_id, capture_to_trigger_binder,
            binder);
        if (result != CAPTURE_BACKEND_OK)
            break;
    }
    if (event_callback_camera_id != CAPTURE_BACKEND_CAMERA_COUNT) {
        std::cerr << "TRIGGER_CAPTURE_CALLBACK_FAILED camera_id="
                  << event_callback_camera_id << " code=" << result
                  << " reason=\"" << capture_backend_strerror(result)
                  << "\"\n";
        trigger_simulator_destroy(simulator);
        trigger_frame_binder_destroy(binder);
        camera_photo_destroy(photo);
        camera_net_destroy(net);
        camera_uvc_destroy(uvc);
        capture_backend_destroy(capture);
        camera_backend_destroy(backend);
        return EXIT_FAILURE;
    }

    xvs_uart_controller_t *xvs = nullptr;
    if (!sync_uart_device.empty()) {
        result = xvs_uart_create(sync_uart_device.c_str(), &xvs);
        if (result == XVS_UART_OK)
            result = xvs_uart_ping(xvs);
        if (result == XVS_UART_OK)
            result = xvs_uart_idle(xvs);
        if (result != XVS_UART_OK) {
            std::cerr << "XVS_UART_START_FAILED device=\""
                      << sync_uart_device << "\" code=" << result
                      << " reason=\"" << xvs_uart_strerror(result)
                      << "\"\n";
            xvs_uart_destroy(xvs);
            trigger_simulator_destroy(simulator);
            trigger_frame_binder_destroy(binder);
            camera_photo_destroy(photo);
            camera_net_destroy(net);
            camera_uvc_destroy(uvc);
            capture_backend_destroy(capture);
            camera_backend_destroy(backend);
            return EXIT_FAILURE;
        }
        std::cout << "XVS_UART_READY device=\"" << sync_uart_device
                  << "\" baud=115200 format=8N1 output_state=IDLE_HIGH\n";
    }

    std::cout << "CAMERA_BACKEND_READY cameras=" << CAMERA_BACKEND_CAMERA_COUNT
              << " uvc=2x4000x3000@10fps/MJPEG sources=camera0,camera1"
              << " net=4000x3000@10fps/MJPEG"
              << " HTTP=:8080/{cam0,cam1} sources=camera0,camera1\n";
    if (!daemon_mode)
        print_commands();

    if (autostart && !autostart_http_outputs(capture, net)) {
        xvs_uart_destroy(xvs);
        trigger_simulator_destroy(simulator);
        capture_backend_destroy(capture);
        trigger_frame_binder_destroy(binder);
        camera_photo_destroy(photo);
        camera_net_destroy(net);
        camera_uvc_destroy(uvc);
        camera_backend_destroy(backend);
        return EXIT_FAILURE;
    }

    if (uvc_autostart && !autostart_uvc_output(capture, uvc)) {
        xvs_uart_destroy(xvs);
        trigger_simulator_destroy(simulator);
        capture_backend_destroy(capture);
        trigger_frame_binder_destroy(binder);
        camera_photo_destroy(photo);
        camera_net_destroy(net);
        camera_uvc_destroy(uvc);
        camera_backend_destroy(backend);
        return EXIT_FAILURE;
    }

    if (daemon_mode) {
        while (!g_stop)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
    } else {
        const bool interactive = isatty(STDIN_FILENO);
        std::string line;
        while (!g_stop) {
            if (interactive)
                std::cout << "camera-aiq> " << std::flush;
            if (!std::getline(std::cin, line))
                break;
            if (!execute_command(backend, capture, uvc, net, photo, xvs, binder,
                                 simulator, line))
                break;
        }
    }

    trigger_simulator_destroy(simulator);
    if (xvs) {
        const int stop_result = xvs_uart_stop(xvs);
        if (stop_result != XVS_UART_OK) {
            std::cerr << "XVS_UART_STOP_WARNING code=" << stop_result
                      << " reason=\"" << xvs_uart_strerror(stop_result)
                      << "\"\n";
        }
        xvs_uart_destroy(xvs);
    }
    capture_backend_destroy(capture);
    trigger_frame_binder_destroy(binder);
    camera_photo_destroy(photo);
    camera_net_destroy(net);
    camera_uvc_destroy(uvc);
    camera_backend_destroy(backend);
    std::cout << "CAMERA_BACKEND_STOPPED\n";
    return EXIT_SUCCESS;
}
