#include "trigger_frame_binder.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits.h>
#include <mutex>
#include <new>
#include <string>

namespace {

constexpr uint32_t kDefaultMaxPendingTriggers = 4096;
constexpr uint32_t kDefaultCompletedHistoryDepth = 128;

struct StoredFrame {
    uint32_t sequence = 0;
    uint32_t buffer_flags = 0;
    uint64_t timestamp_ns = 0;
    uint64_t realtime_dequeue_ns = 0;
};

struct PendingBinding {
    uint64_t trigger_id = 0;
    uint64_t trigger_monotonic_ns = 0;
    uint64_t trigger_realtime_ns = 0;
    uint64_t pps_id = 0;
    uint64_t trigger_timer_tick = 0;
    bool utc_valid = false;
    bool monotonic_is_uart_arrival = false;
    std::string source;
    bool has_frame[2] = {false, false};
    StoredFrame frame[2];
};

int64_t signed_delta_ns(uint64_t value, uint64_t reference)
{
    if (value >= reference) {
        const uint64_t difference = value - reference;
        return difference > static_cast<uint64_t>(INT64_MAX)
                   ? INT64_MAX
                   : static_cast<int64_t>(difference);
    }
    const uint64_t difference = reference - value;
    return difference > static_cast<uint64_t>(INT64_MAX)
               ? INT64_MIN
               : -static_cast<int64_t>(difference);
}

uint64_t absolute_delta_ns(uint64_t left, uint64_t right)
{
    return left >= right ? left - right : right - left;
}

}  // namespace

struct trigger_frame_binder {
    std::mutex mutex;
    uint32_t max_pending_triggers = kDefaultMaxPendingTriggers;
    uint32_t completed_history_depth = kDefaultCompletedHistoryDepth;
    uint32_t triggers_to_ignore = 0;
    bool have_last_trigger_id = false;
    uint64_t last_trigger_id = 0;
    std::deque<PendingBinding> pending;
    std::deque<trigger_frame_binding_t> completed;
    trigger_frame_binder_status_t status = {};
    FILE *csv = nullptr;
    std::string csv_path;
};

namespace {

void close_csv_locked(trigger_frame_binder_t *binder)
{
    if (binder->csv) {
        std::fclose(binder->csv);
        binder->csv = nullptr;
    }
}

int open_csv_locked(trigger_frame_binder_t *binder, const char *path)
{
    close_csv_locked(binder);
    binder->csv_path.clear();
    if (!path || !*path) {
        std::memset(binder->status.csv_path, 0,
                    sizeof(binder->status.csv_path));
        return TRIGGER_FRAME_BINDER_OK;
    }
    if (std::strlen(path) >= sizeof(binder->status.csv_path))
        return TRIGGER_FRAME_BINDER_ERR_ARGUMENT;

    FILE *csv = std::fopen(path, "w");
    if (!csv)
        return TRIGGER_FRAME_BINDER_ERR_IO;
    const char *header =
        "trigger_id,source,trigger_monotonic_ns,trigger_realtime_ns,"
        "pps_id,trigger_timer_tick,utc_valid,monotonic_is_uart_arrival,"
        "cam0_sequence,cam0_flags,cam0_timestamp_ns,cam0_realtime_dequeue_ns,cam0_delay_ns,"
        "cam1_sequence,cam1_flags,cam1_timestamp_ns,cam1_realtime_dequeue_ns,cam1_delay_ns,"
        "frame_delta_ns\n";
    if (std::fputs(header, csv) == EOF || std::fflush(csv) != 0) {
        std::fclose(csv);
        return TRIGGER_FRAME_BINDER_ERR_IO;
    }
    binder->csv = csv;
    binder->csv_path = path;
    std::snprintf(binder->status.csv_path, sizeof(binder->status.csv_path),
                  "%s", path);
    return TRIGGER_FRAME_BINDER_OK;
}

void write_binding_locked(trigger_frame_binder_t *binder,
                          const trigger_frame_binding_t &binding)
{
    if (!binder->csv)
        return;
    const int result = std::fprintf(
        binder->csv,
        "%llu,%s,%llu,%llu,%llu,%llu,%d,%d,%u,0x%08x,%llu,%llu,%lld,"
        "%u,0x%08x,%llu,%llu,%lld,%llu\n",
        static_cast<unsigned long long>(binding.trigger_id), binding.source,
        static_cast<unsigned long long>(binding.trigger_monotonic_ns),
        static_cast<unsigned long long>(binding.trigger_realtime_ns),
        static_cast<unsigned long long>(binding.pps_id),
        static_cast<unsigned long long>(binding.trigger_timer_tick),
        binding.utc_valid, binding.monotonic_is_uart_arrival,
        binding.cam0_sequence, binding.cam0_buffer_flags,
        static_cast<unsigned long long>(binding.cam0_timestamp_ns),
        static_cast<unsigned long long>(binding.cam0_realtime_dequeue_ns),
        static_cast<long long>(binding.cam0_delay_ns), binding.cam1_sequence,
        binding.cam1_buffer_flags,
        static_cast<unsigned long long>(binding.cam1_timestamp_ns),
        static_cast<unsigned long long>(binding.cam1_realtime_dequeue_ns),
        static_cast<long long>(binding.cam1_delay_ns),
        static_cast<unsigned long long>(binding.frame_delta_ns));
    if (result < 0 || std::fflush(binder->csv) != 0) {
        close_csv_locked(binder);
    }
}

void complete_binding_locked(trigger_frame_binder_t *binder,
                             const PendingBinding &pending)
{
    trigger_frame_binding_t binding = {};
    binding.valid = 1;
    binding.trigger_id = pending.trigger_id;
    binding.trigger_monotonic_ns = pending.trigger_monotonic_ns;
    binding.trigger_realtime_ns = pending.trigger_realtime_ns;
    binding.pps_id = pending.pps_id;
    binding.trigger_timer_tick = pending.trigger_timer_tick;
    binding.utc_valid = pending.utc_valid ? 1 : 0;
    binding.monotonic_is_uart_arrival =
        pending.monotonic_is_uart_arrival ? 1 : 0;
    std::snprintf(binding.source, sizeof(binding.source), "%s",
                  pending.source.c_str());
    binding.cam0_sequence = pending.frame[0].sequence;
    binding.cam0_buffer_flags = pending.frame[0].buffer_flags;
    binding.cam0_timestamp_ns = pending.frame[0].timestamp_ns;
    binding.cam0_realtime_dequeue_ns = pending.frame[0].realtime_dequeue_ns;
    binding.cam1_sequence = pending.frame[1].sequence;
    binding.cam1_buffer_flags = pending.frame[1].buffer_flags;
    binding.cam1_timestamp_ns = pending.frame[1].timestamp_ns;
    binding.cam1_realtime_dequeue_ns = pending.frame[1].realtime_dequeue_ns;
    binding.cam0_delay_ns = signed_delta_ns(binding.cam0_timestamp_ns,
                                             binding.trigger_monotonic_ns);
    binding.cam1_delay_ns = signed_delta_ns(binding.cam1_timestamp_ns,
                                             binding.trigger_monotonic_ns);
    binding.frame_delta_ns = absolute_delta_ns(binding.cam0_timestamp_ns,
                                               binding.cam1_timestamp_ns);
    binder->completed.push_back(binding);
    while (binder->completed.size() > binder->completed_history_depth)
        binder->completed.pop_front();
    ++binder->status.complete_pairs;
    binder->status.last_completed_trigger_id = binding.trigger_id;
    binder->status.last_frame_delta_ns = binding.frame_delta_ns;
    write_binding_locked(binder, binding);
}

}  // namespace

extern "C" void trigger_frame_binder_default_config(
    trigger_frame_binder_config_t *config)
{
    if (!config)
        return;
    std::memset(config, 0, sizeof(*config));
    config->max_pending_triggers = kDefaultMaxPendingTriggers;
    config->completed_history_depth = kDefaultCompletedHistoryDepth;
}

extern "C" int trigger_frame_binder_create(
    const trigger_frame_binder_config_t *config,
    trigger_frame_binder_t **binder_out)
{
    if (!binder_out)
        return TRIGGER_FRAME_BINDER_ERR_ARGUMENT;
    *binder_out = nullptr;
    trigger_frame_binder_config_t selected = {};
    trigger_frame_binder_default_config(&selected);
    if (config) {
        if (config->max_pending_triggers)
            selected.max_pending_triggers = config->max_pending_triggers;
        if (config->completed_history_depth)
            selected.completed_history_depth = config->completed_history_depth;
    }
    if (!selected.max_pending_triggers || !selected.completed_history_depth)
        return TRIGGER_FRAME_BINDER_ERR_ARGUMENT;

    trigger_frame_binder_t *binder = new (std::nothrow) trigger_frame_binder;
    if (!binder)
        return TRIGGER_FRAME_BINDER_ERR_IO;
    binder->max_pending_triggers = selected.max_pending_triggers;
    binder->completed_history_depth = selected.completed_history_depth;
    *binder_out = binder;
    return TRIGGER_FRAME_BINDER_OK;
}

extern "C" void trigger_frame_binder_destroy(trigger_frame_binder_t *binder)
{
    if (!binder)
        return;
    {
        std::lock_guard<std::mutex> lock(binder->mutex);
        close_csv_locked(binder);
    }
    delete binder;
}

extern "C" int trigger_frame_binder_reset(trigger_frame_binder_t *binder,
                                             uint32_t triggers_to_ignore)
{
    if (!binder)
        return TRIGGER_FRAME_BINDER_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(binder->mutex);
    binder->triggers_to_ignore = triggers_to_ignore;
    binder->have_last_trigger_id = false;
    binder->last_trigger_id = 0;
    binder->pending.clear();
    binder->completed.clear();
    std::memset(&binder->status, 0, sizeof(binder->status));
    const std::string csv_path = binder->csv_path;
    if (csv_path.empty())
        return TRIGGER_FRAME_BINDER_OK;
    return open_csv_locked(binder, csv_path.c_str());
}

extern "C" int trigger_frame_binder_set_csv_path(
    trigger_frame_binder_t *binder, const char *path)
{
    if (!binder)
        return TRIGGER_FRAME_BINDER_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(binder->mutex);
    return open_csv_locked(binder, path);
}

extern "C" int trigger_frame_binder_on_trigger(
    trigger_frame_binder_t *binder, uint64_t trigger_id,
    uint64_t monotonic_ns, uint64_t realtime_ns, const char *source)
{
    trigger_frame_trigger_t trigger = {};
    trigger.trigger_id = trigger_id;
    trigger.monotonic_ns = monotonic_ns;
    trigger.realtime_ns = realtime_ns;
    trigger.source = source;
    return trigger_frame_binder_on_trigger_ex(binder, &trigger);
}

extern "C" int trigger_frame_binder_on_trigger_ex(
    trigger_frame_binder_t *binder, const trigger_frame_trigger_t *trigger)
{
    if (!binder || !trigger || !trigger->trigger_id ||
        !trigger->monotonic_ns || !trigger->realtime_ns || !trigger->source ||
        !*trigger->source ||
        std::strlen(trigger->source) >= TRIGGER_FRAME_BINDER_SOURCE_MAX) {
        return TRIGGER_FRAME_BINDER_ERR_ARGUMENT;
    }

    std::lock_guard<std::mutex> lock(binder->mutex);
    ++binder->status.triggers_received;
    if (binder->have_last_trigger_id) {
        if (trigger->trigger_id <= binder->last_trigger_id) {
            ++binder->status.duplicate_triggers;
            return TRIGGER_FRAME_BINDER_OK;
        }
        binder->status.trigger_id_gaps +=
            trigger->trigger_id - binder->last_trigger_id - 1;
    }
    binder->have_last_trigger_id = true;
    binder->last_trigger_id = trigger->trigger_id;
    binder->status.last_trigger_id = trigger->trigger_id;

    if (binder->triggers_to_ignore) {
        --binder->triggers_to_ignore;
        ++binder->status.triggers_ignored;
        return TRIGGER_FRAME_BINDER_OK;
    }
    if (binder->pending.size() >= binder->max_pending_triggers) {
        binder->pending.pop_front();
        ++binder->status.pending_trigger_overflows;
    }
    PendingBinding pending;
    pending.trigger_id = trigger->trigger_id;
    pending.trigger_monotonic_ns = trigger->monotonic_ns;
    pending.trigger_realtime_ns = trigger->realtime_ns;
    pending.pps_id = trigger->pps_id;
    pending.trigger_timer_tick = trigger->timer_tick;
    pending.utc_valid = trigger->utc_valid != 0;
    pending.monotonic_is_uart_arrival =
        trigger->monotonic_is_uart_arrival != 0;
    pending.source = trigger->source;
    binder->pending.push_back(std::move(pending));
    binder->status.pending_triggers = binder->pending.size();
    return TRIGGER_FRAME_BINDER_OK;
}

extern "C" int trigger_frame_binder_on_frame(
    trigger_frame_binder_t *binder, const trigger_frame_event_t *frame)
{
    if (!binder || !frame || frame->camera_id < 0 || frame->camera_id > 1)
        return TRIGGER_FRAME_BINDER_ERR_ARGUMENT;

    std::lock_guard<std::mutex> lock(binder->mutex);
    const int camera_id = frame->camera_id;
    ++binder->status.frames_received[camera_id];
    PendingBinding *target = nullptr;
    for (PendingBinding &pending : binder->pending) {
        if (!pending.has_frame[camera_id]) {
            target = &pending;
            break;
        }
    }
    if (!target) {
        ++binder->status.frames_without_trigger[camera_id];
        return TRIGGER_FRAME_BINDER_OK;
    }
    target->has_frame[camera_id] = true;
    target->frame[camera_id].sequence = frame->sequence;
    target->frame[camera_id].buffer_flags = frame->buffer_flags;
    target->frame[camera_id].timestamp_ns = frame->v4l2_timestamp_ns;
    target->frame[camera_id].realtime_dequeue_ns = frame->realtime_dequeue_ns;
    ++binder->status.frames_bound[camera_id];

    for (auto it = binder->pending.begin(); it != binder->pending.end();) {
        if (it->has_frame[0] && it->has_frame[1]) {
            complete_binding_locked(binder, *it);
            it = binder->pending.erase(it);
        } else {
            ++it;
        }
    }
    binder->status.pending_triggers = binder->pending.size();
    return TRIGGER_FRAME_BINDER_OK;
}

extern "C" int trigger_frame_binder_get_status(
    trigger_frame_binder_t *binder, trigger_frame_binder_status_t *status)
{
    if (!binder || !status)
        return TRIGGER_FRAME_BINDER_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(binder->mutex);
    *status = binder->status;
    status->pending_triggers = binder->pending.size();
    std::snprintf(status->csv_path, sizeof(status->csv_path), "%s",
                  binder->csv_path.c_str());
    return TRIGGER_FRAME_BINDER_OK;
}

extern "C" int trigger_frame_binder_get_last_binding(
    trigger_frame_binder_t *binder, trigger_frame_binding_t *binding)
{
    if (!binder || !binding)
        return TRIGGER_FRAME_BINDER_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(binder->mutex);
    std::memset(binding, 0, sizeof(*binding));
    if (!binder->completed.empty())
        *binding = binder->completed.back();
    return TRIGGER_FRAME_BINDER_OK;
}

extern "C" int trigger_frame_binder_find_frame(
    trigger_frame_binder_t *binder, int camera_id, uint32_t sequence,
    trigger_frame_match_t *match)
{
    if (!binder || !match || camera_id < 0 || camera_id > 1)
        return TRIGGER_FRAME_BINDER_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(binder->mutex);
    std::memset(match, 0, sizeof(*match));

    for (auto it = binder->completed.rbegin(); it != binder->completed.rend();
         ++it) {
        const uint32_t frame_sequence = camera_id == 0 ? it->cam0_sequence
                                                       : it->cam1_sequence;
        if (frame_sequence != sequence)
            continue;
        match->valid = 1;
        match->pair_complete = 1;
        match->camera_id = camera_id;
        match->trigger_id = it->trigger_id;
        match->trigger_monotonic_ns = it->trigger_monotonic_ns;
        match->trigger_realtime_ns = it->trigger_realtime_ns;
        match->pps_id = it->pps_id;
        match->trigger_timer_tick = it->trigger_timer_tick;
        match->utc_valid = it->utc_valid;
        match->monotonic_is_uart_arrival = it->monotonic_is_uart_arrival;
        std::snprintf(match->source, sizeof(match->source), "%s", it->source);
        match->sequence = frame_sequence;
        match->buffer_flags = camera_id == 0 ? it->cam0_buffer_flags
                                             : it->cam1_buffer_flags;
        match->v4l2_timestamp_ns = camera_id == 0 ? it->cam0_timestamp_ns
                                                  : it->cam1_timestamp_ns;
        match->realtime_dequeue_ns =
            camera_id == 0 ? it->cam0_realtime_dequeue_ns
                           : it->cam1_realtime_dequeue_ns;
        match->trigger_to_frame_ns = signed_delta_ns(
            match->v4l2_timestamp_ns, match->trigger_monotonic_ns);
        return TRIGGER_FRAME_BINDER_OK;
    }

    for (auto it = binder->pending.rbegin(); it != binder->pending.rend(); ++it) {
        if (!it->has_frame[camera_id] ||
            it->frame[camera_id].sequence != sequence)
            continue;
        match->valid = 1;
        match->pair_complete = it->has_frame[0] && it->has_frame[1];
        match->camera_id = camera_id;
        match->trigger_id = it->trigger_id;
        match->trigger_monotonic_ns = it->trigger_monotonic_ns;
        match->trigger_realtime_ns = it->trigger_realtime_ns;
        match->pps_id = it->pps_id;
        match->trigger_timer_tick = it->trigger_timer_tick;
        match->utc_valid = it->utc_valid ? 1 : 0;
        match->monotonic_is_uart_arrival =
            it->monotonic_is_uart_arrival ? 1 : 0;
        std::snprintf(match->source, sizeof(match->source), "%s",
                      it->source.c_str());
        match->sequence = sequence;
        match->buffer_flags = it->frame[camera_id].buffer_flags;
        match->v4l2_timestamp_ns = it->frame[camera_id].timestamp_ns;
        match->realtime_dequeue_ns =
            it->frame[camera_id].realtime_dequeue_ns;
        match->trigger_to_frame_ns = signed_delta_ns(
            match->v4l2_timestamp_ns, match->trigger_monotonic_ns);
        return TRIGGER_FRAME_BINDER_OK;
    }
    return TRIGGER_FRAME_BINDER_OK;
}

extern "C" const char *trigger_frame_binder_strerror(int result)
{
    switch (result) {
    case TRIGGER_FRAME_BINDER_OK:
        return "success";
    case TRIGGER_FRAME_BINDER_ERR_ARGUMENT:
        return "invalid trigger or frame metadata";
    case TRIGGER_FRAME_BINDER_ERR_IO:
        return "unable to allocate or write binding data";
    default:
        return "unknown trigger binding error";
    }
}
