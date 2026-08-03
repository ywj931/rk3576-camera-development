#include "time_sync_service.h"
#include "trigger_frame_binder.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

std::string make_nmea(const std::string &payload)
{
    unsigned char checksum = 0;
    for (unsigned char byte : payload)
        checksum ^= byte;
    char suffix[8];
    std::snprintf(suffix, sizeof(suffix), "*%02X", checksum);
    return "$" + payload + suffix;
}

bool expect(const char *step, bool condition)
{
    if (!condition)
        std::fprintf(stderr, "STAGE7_TIME_PIPELINE_TEST_FAILED step=%s\n",
                     step);
    return condition;
}

}  // namespace

int main()
{
    time_sync_config_t sync_config = {};
    time_sync_default_config(&sync_config);
    sync_config.timer_frequency_hz = 1000000;

    time_sync_service_t *sync = nullptr;
    trigger_frame_binder_t *binder = nullptr;
    trigger_frame_binder_config_t binder_config = {};
    trigger_frame_binder_default_config(&binder_config);

    bool passed = expect("create-time-sync",
                         time_sync_create(&sync_config, &sync) == TIME_SYNC_OK);
    passed = passed && expect(
        "create-binder",
        trigger_frame_binder_create(&binder_config, &binder) ==
            TRIGGER_FRAME_BINDER_OK);

    const std::string rmc = make_nmea(
        "GNRMC,123519.00,A,4807.038,N,01131.000,E,0.0,0.0,230394,,,A");
    int64_t rmc_utc_sec = 0;
    int rmc_valid = 0;
    passed = passed && expect(
        "parse-rmc",
        time_sync_parse_nmea_rmc(rmc.c_str(), &rmc_utc_sec, &rmc_valid) ==
                TIME_SYNC_OK &&
            rmc_valid == 1);
    passed = passed && expect(
        "pps-700", time_sync_on_pps(sync, 700, 1000000) == TIME_SYNC_OK);
    passed = passed && expect(
        "nmea-after-pps-700",
        time_sync_on_nmea_rmc(sync, 700, rmc.c_str()) == TIME_SYNC_OK);
    passed = passed && expect(
        "pps-701-lock", time_sync_on_pps(sync, 701, 2000000) == TIME_SYNC_OK);

    time_sync_resolution_t resolution = {};
    const uint64_t trigger_tick = 2250000;
    const uint64_t expected_utc_ns =
        static_cast<uint64_t>(rmc_utc_sec + 1) * 1000000000ULL +
        250000000ULL;
    passed = passed && expect(
        "resolve-xvs",
        time_sync_resolve_tick(sync, 701, trigger_tick, &resolution) ==
                TIME_SYNC_OK &&
            resolution.valid && resolution.utc_ns == expected_utc_ns &&
            resolution.state == TIME_SYNC_STATE_UTC_LOCKED);

    trigger_frame_trigger_t trigger = {};
    trigger.trigger_id = 9001;
    trigger.monotonic_ns = 9000000000ULL;
    trigger.realtime_ns = resolution.utc_ns;
    trigger.pps_id = resolution.pps_id;
    trigger.timer_tick = resolution.timer_tick;
    trigger.utc_valid = resolution.valid;
    trigger.monotonic_is_uart_arrival = 1;
    trigger.source = "MCU_PPS_LOCKED";
    passed = passed && expect(
        "queue-trigger",
        trigger_frame_binder_on_trigger_ex(binder, &trigger) ==
            TRIGGER_FRAME_BINDER_OK);

    const trigger_frame_event_t cam0 = {
        0, 1701, 0x2001, 9000012000ULL, 19000012000ULL};
    const trigger_frame_event_t cam1 = {
        1, 2701, 0x2001, 9000015000ULL, 19000015000ULL};
    passed = passed && expect(
        "bind-cam0", trigger_frame_binder_on_frame(binder, &cam0) ==
                         TRIGGER_FRAME_BINDER_OK);
    passed = passed && expect(
        "bind-cam1", trigger_frame_binder_on_frame(binder, &cam1) ==
                         TRIGGER_FRAME_BINDER_OK);

    trigger_frame_binding_t binding = {};
    passed = passed && expect(
        "read-binding",
        trigger_frame_binder_get_last_binding(binder, &binding) ==
                TRIGGER_FRAME_BINDER_OK &&
            binding.valid && binding.trigger_id == 9001 &&
            binding.pps_id == 701 &&
            binding.trigger_timer_tick == trigger_tick && binding.utc_valid &&
            binding.monotonic_is_uart_arrival &&
            binding.trigger_realtime_ns == expected_utc_ns &&
            binding.cam0_sequence == 1701 && binding.cam1_sequence == 2701 &&
            binding.frame_delta_ns == 3000);

    trigger_frame_match_t match = {};
    passed = passed && expect(
        "photo-frame-lookup",
        trigger_frame_binder_find_frame(binder, 1, 2701, &match) ==
                TRIGGER_FRAME_BINDER_OK &&
            match.valid && match.pair_complete && match.utc_valid &&
            match.trigger_realtime_ns == expected_utc_ns &&
            match.pps_id == 701 && match.trigger_timer_tick == trigger_tick);

    trigger_frame_binder_destroy(binder);
    time_sync_destroy(sync);
    if (!passed)
        return EXIT_FAILURE;

    std::printf("STAGE7_TIME_PIPELINE_TEST_OK pps_to_utc=verified "
                "trigger_id=9001 cam0_frame_id=1701 cam1_frame_id=2701 "
                "utc_ns=%llu\n",
                static_cast<unsigned long long>(expected_utc_ns));
    return EXIT_SUCCESS;
}
