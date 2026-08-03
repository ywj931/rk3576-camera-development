#include "time_sync_service.h"

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

bool expect(const char *name, bool condition)
{
    if (!condition)
        std::fprintf(stderr, "TIME_SYNC_TEST_FAILED step=%s\n", name);
    return condition;
}

}  // namespace

int main()
{
    time_sync_config_t config = {};
    time_sync_default_config(&config);
    config.timer_frequency_hz = 1000000;
    config.max_holdover_pps = 2;

    time_sync_service_t *service = nullptr;
    bool passed = expect("create", time_sync_create(&config, &service) == 0);
    const std::string rmc = make_nmea(
        "GNRMC,123519.00,A,4807.038,N,01131.000,E,0.0,0.0,230394,,,A");
    int64_t rmc_utc_sec = 0;
    int rmc_valid = 0;
    passed = passed && expect(
        "parse-rmc", time_sync_parse_nmea_rmc(
                         rmc.c_str(), &rmc_utc_sec, &rmc_valid) == 0 &&
                         rmc_valid == 1);
    passed = passed && expect(
        "pps-10", time_sync_on_pps(service, 10, 10000000) == 0);
    passed = passed && expect(
        "rmc-after-pps-10",
        time_sync_on_nmea_rmc(service, 10, rmc.c_str()) == 0);
    passed = passed && expect(
        "pps-11-lock", time_sync_on_pps(service, 11, 11000000) == 0);

    time_sync_resolution_t resolution = {};
    const uint64_t expected_ns =
        static_cast<uint64_t>(rmc_utc_sec + 1) * 1000000000ULL + 250000000ULL;
    passed = passed && expect(
        "resolve-quarter-second",
        time_sync_resolve_tick(service, 11, 11250000, &resolution) == 0 &&
            resolution.valid && resolution.utc_ns == expected_ns &&
            resolution.state == TIME_SYNC_STATE_UTC_LOCKED);

    passed = passed && expect(
        "pps-12-holdover", time_sync_on_pps(service, 12, 12000000) == 0);
    passed = passed && expect(
        "resolve-holdover",
        time_sync_resolve_tick(service, 12, 12500000, &resolution) == 0 &&
            resolution.valid &&
            resolution.state == TIME_SYNC_STATE_HOLDOVER &&
            resolution.utc_ns ==
                static_cast<uint64_t>(rmc_utc_sec + 2) * 1000000000ULL +
                    500000000ULL);

    const std::string invalid_rmc = make_nmea(
        "GPRMC,123520.00,V,4807.038,N,01131.000,E,0.0,0.0,230394,,,N");
    passed = passed && expect(
        "invalid-rmc-status",
        time_sync_on_nmea_rmc(service, 12, invalid_rmc.c_str()) == 0);
    passed = passed && expect(
        "pps-13-holdover", time_sync_on_pps(service, 13, 13000000) == 0);
    passed = passed && expect(
        "pps-14-expire", time_sync_on_pps(service, 14, 14000000) == 0);
    passed = passed && expect(
        "expired-holdover",
        time_sync_resolve_tick(service, 14, 14100000, &resolution) ==
            TIME_SYNC_ERR_NOT_LOCKED);

    time_sync_status_t status = {};
    passed = passed && expect(
        "status", time_sync_get_status(service, &status) == 0 &&
                      status.state == TIME_SYNC_STATE_HOLDOVER &&
                      status.utc_valid == 0 && status.pps_events == 5 &&
                      status.rmc_events == 2 &&
                      status.invalid_rmc_events == 1 &&
                      status.resolved_events == 2 &&
                      status.unresolved_events == 1);

    std::string damaged = rmc;
    damaged[8] = damaged[8] == '1' ? '2' : '1';
    passed = passed && expect(
        "checksum-reject",
        time_sync_parse_nmea_rmc(damaged.c_str(), &rmc_utc_sec,
                                 &rmc_valid) == TIME_SYNC_ERR_NMEA);
    time_sync_destroy(service);
    if (!passed)
        return EXIT_FAILURE;
    std::printf("TIME_SYNC_SERVICE_TEST_OK state=HOLDOVER utc_valid=0 "
                "rmc_variants=GNRMC,GPRMC\n");
    return EXIT_SUCCESS;
}
