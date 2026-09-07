#include "time_sync_service.h"

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kNanosecondsPerSecond = 1000000000ULL;
constexpr uint64_t kDefaultTimerFrequencyHz = 1000000ULL;
constexpr uint32_t kDefaultMaxHoldoverPps = 5;

std::vector<std::string> split(const std::string &text)
{
    std::vector<std::string> fields;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        fields.push_back(text.substr(
            start, comma == std::string::npos ? std::string::npos
                                               : comma - start));
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return fields;
}

bool parse_two_digits(const std::string &text, size_t offset, int *value)
{
    if (!value || offset + 2 > text.size() ||
        !std::isdigit(static_cast<unsigned char>(text[offset])) ||
        !std::isdigit(static_cast<unsigned char>(text[offset + 1]))) {
        return false;
    }
    *value = (text[offset] - '0') * 10 + text[offset + 1] - '0';
    return true;
}

int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned adjusted_month = month > 2 ? month - 3 : month + 9;
    const unsigned day_of_year =
        (153 * adjusted_month + 2) / 5 + day - 1;
    const unsigned day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 +
        day_of_year;
    return era * 146097LL + static_cast<int64_t>(day_of_era) - 719468LL;
}

bool valid_date(int year, int month, int day)
{
    static const int days_per_month[] = {31, 28, 31, 30, 31, 30,
                                         31, 31, 30, 31, 30, 31};
    if (year < 1970 || year > 2199 || month < 1 || month > 12 || day < 1)
        return false;
    int maximum = days_per_month[month - 1];
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (month == 2 && leap)
        ++maximum;
    return day <= maximum;
}

bool ticks_to_ns(uint64_t ticks, uint64_t frequency_hz, uint64_t *ns)
{
    if (!frequency_hz || !ns)
        return false;
    const uint64_t seconds = ticks / frequency_hz;
    const uint64_t remainder = ticks % frequency_hz;
    if (seconds > UINT64_MAX / kNanosecondsPerSecond ||
        remainder > UINT64_MAX / kNanosecondsPerSecond) {
        return false;
    }
    const uint64_t whole = seconds * kNanosecondsPerSecond;
    const uint64_t fraction =
        (remainder * kNanosecondsPerSecond) / frequency_hz;
    if (whole > UINT64_MAX - fraction)
        return false;
    *ns = whole + fraction;
    return true;
}

}  // namespace

struct time_sync_service {
    std::mutex mutex;
    time_sync_config_t config = {};
    time_sync_status_t status = {};
    bool have_last_pps = false;
    bool have_pending_rmc = false;
    bool have_reference = false;
    uint64_t pending_rmc_pps_id = 0;
    int64_t pending_rmc_utc_sec = 0;
};

namespace {

void reset_locked(time_sync_service_t *service)
{
    const uint64_t frequency = service->config.timer_frequency_hz;
    const uint32_t holdover = service->config.max_holdover_pps;
    service->status = {};
    service->status.state = TIME_SYNC_STATE_UNLOCKED;
    service->status.timer_frequency_hz = frequency;
    service->status.max_holdover_pps = holdover;
    service->have_last_pps = false;
    service->have_pending_rmc = false;
    service->have_reference = false;
    service->pending_rmc_pps_id = 0;
    service->pending_rmc_utc_sec = 0;
}

bool utc_sec_to_ns(int64_t utc_sec, uint64_t *utc_ns)
{
    if (!utc_ns || utc_sec < 0 ||
        static_cast<uint64_t>(utc_sec) > UINT64_MAX / kNanosecondsPerSecond) {
        return false;
    }
    *utc_ns = static_cast<uint64_t>(utc_sec) * kNanosecondsPerSecond;
    return true;
}

}  // namespace

extern "C" void time_sync_default_config(time_sync_config_t *config)
{
    if (!config)
        return;
    std::memset(config, 0, sizeof(*config));
    config->timer_frequency_hz = kDefaultTimerFrequencyHz;
    config->max_holdover_pps = kDefaultMaxHoldoverPps;
}

extern "C" int time_sync_create(const time_sync_config_t *config,
                                time_sync_service_t **service_out)
{
    if (!service_out)
        return TIME_SYNC_ERR_ARGUMENT;
    *service_out = nullptr;
    time_sync_config_t selected = {};
    time_sync_default_config(&selected);
    if (config)
        selected = *config;
    if (!selected.timer_frequency_hz ||
        selected.timer_frequency_hz > kNanosecondsPerSecond ||
        !selected.max_holdover_pps) {
        return TIME_SYNC_ERR_ARGUMENT;
    }
    time_sync_service_t *service = new (std::nothrow) time_sync_service;
    if (!service)
        return TIME_SYNC_ERR_ALLOCATE;
    service->config = selected;
    reset_locked(service);
    *service_out = service;
    return TIME_SYNC_OK;
}

extern "C" void time_sync_destroy(time_sync_service_t *service)
{
    delete service;
}

extern "C" int time_sync_reset(time_sync_service_t *service)
{
    if (!service)
        return TIME_SYNC_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(service->mutex);
    reset_locked(service);
    return TIME_SYNC_OK;
}

extern "C" int time_sync_on_pps(time_sync_service_t *service,
                                uint64_t pps_id, uint64_t timer_tick)
{
    if (!service || !pps_id)
        return TIME_SYNC_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(service->mutex);
    if (service->have_last_pps &&
        (pps_id <= service->status.last_pps_id ||
         timer_tick <= service->status.last_pps_tick)) {
        return TIME_SYNC_ERR_SEQUENCE;
    }

    ++service->status.pps_events;
    service->status.last_pps_id = pps_id;
    service->status.last_pps_tick = timer_tick;
    service->have_last_pps = true;

    if (service->have_pending_rmc &&
        service->pending_rmc_pps_id != UINT64_MAX &&
        service->pending_rmc_pps_id + 1 == pps_id) {
        uint64_t reference_utc_ns = 0;
        if (service->pending_rmc_utc_sec == INT64_MAX ||
            !utc_sec_to_ns(service->pending_rmc_utc_sec + 1,
                           &reference_utc_ns)) {
            return TIME_SYNC_ERR_RANGE;
        }
        service->status.reference_pps_id = pps_id;
        service->status.reference_tick = timer_tick;
        service->status.reference_utc_ns = reference_utc_ns;
        service->status.holdover_age_pps = 0;
        service->status.state = TIME_SYNC_STATE_UTC_LOCKED;
        service->status.utc_valid = 1;
        service->have_reference = true;
        service->have_pending_rmc = false;
        return TIME_SYNC_OK;
    }

    if (service->have_reference) {
        const uint64_t pps_delta =
            pps_id - service->status.reference_pps_id;
        if (pps_delta > UINT64_MAX / kNanosecondsPerSecond ||
            service->status.reference_utc_ns >
                UINT64_MAX - pps_delta * kNanosecondsPerSecond) {
            return TIME_SYNC_ERR_RANGE;
        }
        service->status.reference_utc_ns +=
            pps_delta * kNanosecondsPerSecond;
        service->status.reference_pps_id = pps_id;
        service->status.reference_tick = timer_tick;
        if (service->status.holdover_age_pps < UINT32_MAX)
            ++service->status.holdover_age_pps;
        service->status.state = TIME_SYNC_STATE_HOLDOVER;
        service->status.utc_valid =
            service->status.holdover_age_pps <=
                    service->config.max_holdover_pps
                ? 1
                : 0;
    } else {
        service->status.state = TIME_SYNC_STATE_PPS_ONLY;
        service->status.utc_valid = 0;
    }
    return TIME_SYNC_OK;
}

extern "C" int time_sync_on_rmc_utc(time_sync_service_t *service,
                                    uint64_t pps_id, int64_t utc_sec,
                                    int valid)
{
    if (!service || !pps_id)
        return TIME_SYNC_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(service->mutex);
    ++service->status.rmc_events;
    service->status.last_rmc_pps_id = pps_id;
    service->status.last_rmc_utc_sec = utc_sec;
    if (!valid || utc_sec < 0) {
        ++service->status.invalid_rmc_events;
        return TIME_SYNC_OK;
    }
    if (service->have_last_pps && pps_id > service->status.last_pps_id)
        return TIME_SYNC_ERR_SEQUENCE;
    service->pending_rmc_pps_id = pps_id;
    service->pending_rmc_utc_sec = utc_sec;
    service->have_pending_rmc = true;
    return TIME_SYNC_OK;
}

extern "C" int time_sync_parse_nmea_rmc(const char *sentence,
                                        int64_t *utc_sec, int *valid)
{
    if (!sentence || !utc_sec || !valid)
        return TIME_SYNC_ERR_ARGUMENT;
    *utc_sec = 0;
    *valid = 0;
    std::string frame(sentence);
    while (!frame.empty() && (frame.back() == '\r' || frame.back() == '\n'))
        frame.pop_back();
    if (frame.size() < 7 || frame.front() != '$')
        return TIME_SYNC_ERR_NMEA;
    const size_t star = frame.rfind('*');
    if (star == std::string::npos || star + 3 != frame.size())
        return TIME_SYNC_ERR_NMEA;
    char *end = nullptr;
    errno = 0;
    const unsigned long expected =
        std::strtoul(frame.substr(star + 1).c_str(), &end, 16);
    if (errno || !end || *end != '\0' || expected > 0xff)
        return TIME_SYNC_ERR_NMEA;
    unsigned char checksum = 0;
    for (size_t index = 1; index < star; ++index)
        checksum ^= static_cast<unsigned char>(frame[index]);
    if (checksum != expected)
        return TIME_SYNC_ERR_NMEA;

    const std::vector<std::string> fields =
        split(frame.substr(1, star - 1));
    if (fields.size() <= 9 || fields[0].size() < 3 ||
        fields[0].compare(fields[0].size() - 3, 3, "RMC") != 0 ||
        fields[1].size() < 6 || fields[9].size() != 6) {
        return TIME_SYNC_ERR_NMEA;
    }
    if (fields[2] != "A" && fields[2] != "V")
        return TIME_SYNC_ERR_NMEA;

    int hour = 0;
    int minute = 0;
    int second = 0;
    int day = 0;
    int month = 0;
    int short_year = 0;
    if (!parse_two_digits(fields[1], 0, &hour) ||
        !parse_two_digits(fields[1], 2, &minute) ||
        !parse_two_digits(fields[1], 4, &second) ||
        !parse_two_digits(fields[9], 0, &day) ||
        !parse_two_digits(fields[9], 2, &month) ||
        !parse_two_digits(fields[9], 4, &short_year) || hour > 23 ||
        minute > 59 || second > 60) {
        return TIME_SYNC_ERR_NMEA;
    }
    const int year = short_year < 80 ? 2000 + short_year
                                     : 1900 + short_year;
    if (!valid_date(year, month, day))
        return TIME_SYNC_ERR_NMEA;
    const int normalized_second = second == 60 ? 59 : second;
    *utc_sec = days_from_civil(year, static_cast<unsigned>(month),
                               static_cast<unsigned>(day)) *
                   86400LL +
               hour * 3600LL + minute * 60LL + normalized_second;
    *valid = fields[2] == "A" ? 1 : 0;
    return TIME_SYNC_OK;
}

extern "C" int time_sync_on_nmea_rmc(time_sync_service_t *service,
                                     uint64_t pps_id,
                                     const char *sentence)
{
    int64_t utc_sec = 0;
    int valid = 0;
    const int result = time_sync_parse_nmea_rmc(sentence, &utc_sec, &valid);
    if (result != TIME_SYNC_OK) {
        if (service) {
            std::lock_guard<std::mutex> lock(service->mutex);
            ++service->status.rmc_events;
            ++service->status.invalid_rmc_events;
        }
        return result;
    }
    return time_sync_on_rmc_utc(service, pps_id, utc_sec, valid);
}

extern "C" int time_sync_resolve_tick(
    time_sync_service_t *service, uint64_t pps_id, uint64_t timer_tick,
    time_sync_resolution_t *resolution)
{
    if (!service || !pps_id || !resolution)
        return TIME_SYNC_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(service->mutex);
    std::memset(resolution, 0, sizeof(*resolution));
    resolution->state = service->status.state;
    resolution->pps_id = pps_id;
    resolution->timer_tick = timer_tick;
    if (!service->have_reference || !service->status.utc_valid) {
        ++service->status.unresolved_events;
        return TIME_SYNC_ERR_NOT_LOCKED;
    }

    uint64_t delta_ns = 0;
    uint64_t resolved_ns = service->status.reference_utc_ns;
    if (timer_tick >= service->status.reference_tick) {
        if (!ticks_to_ns(timer_tick - service->status.reference_tick,
                         service->config.timer_frequency_hz, &delta_ns) ||
            resolved_ns > UINT64_MAX - delta_ns) {
            ++service->status.unresolved_events;
            return TIME_SYNC_ERR_RANGE;
        }
        resolved_ns += delta_ns;
    } else {
        if (!ticks_to_ns(service->status.reference_tick - timer_tick,
                         service->config.timer_frequency_hz, &delta_ns) ||
            resolved_ns < delta_ns) {
            ++service->status.unresolved_events;
            return TIME_SYNC_ERR_RANGE;
        }
        resolved_ns -= delta_ns;
    }
    resolution->valid = 1;
    resolution->utc_ns = resolved_ns;
    ++service->status.resolved_events;
    return TIME_SYNC_OK;
}

extern "C" int time_sync_get_status(time_sync_service_t *service,
                                    time_sync_status_t *status)
{
    if (!service || !status)
        return TIME_SYNC_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(service->mutex);
    *status = service->status;
    return TIME_SYNC_OK;
}

extern "C" const char *time_sync_state_name(enum time_sync_state state)
{
    switch (state) {
    case TIME_SYNC_STATE_UNLOCKED:
        return "UNLOCKED";
    case TIME_SYNC_STATE_PPS_ONLY:
        return "PPS_ONLY";
    case TIME_SYNC_STATE_UTC_LOCKED:
        return "UTC_LOCKED";
    case TIME_SYNC_STATE_HOLDOVER:
        return "HOLDOVER";
    default:
        return "UNKNOWN";
    }
}

extern "C" const char *time_sync_strerror(int result)
{
    switch (result) {
    case TIME_SYNC_OK:
        return "success";
    case TIME_SYNC_ERR_ARGUMENT:
        return "invalid time synchronization argument";
    case TIME_SYNC_ERR_ALLOCATE:
        return "unable to allocate time synchronization service";
    case TIME_SYNC_ERR_SEQUENCE:
        return "PPS or RMC event sequence moved backwards";
    case TIME_SYNC_ERR_NMEA:
        return "invalid NMEA RMC sentence";
    case TIME_SYNC_ERR_NOT_LOCKED:
        return "UTC is not locked to PPS";
    case TIME_SYNC_ERR_RANGE:
        return "timestamp conversion exceeded its numeric range";
    default:
        return "unknown time synchronization error";
    }
}
