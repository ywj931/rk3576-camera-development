#include "trigger_simulator.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <time.h>

namespace {

uint64_t clock_ns(clockid_t clock_id)
{
    struct timespec now = {};
    clock_gettime(clock_id, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(now.tv_nsec);
}

}  // namespace

struct trigger_simulator {
    std::mutex mutex;
    std::condition_variable condition;
    std::thread worker;
    trigger_simulator_callback_t callback = nullptr;
    void *user_data = nullptr;
    bool stop_requested = false;
    bool running = false;
    uint32_t frequency_hz = 0;
    uint32_t requested_count = 0;
    uint64_t emitted_count = 0;
    uint64_t last_trigger_id = 0;
};

namespace {

void worker_main(trigger_simulator_t *simulator)
{
    std::chrono::microseconds interval(1000000 / simulator->frequency_hz);
    auto next_deadline = std::chrono::steady_clock::now() + interval;
    for (;;) {
        trigger_simulator_callback_t callback = nullptr;
        void *user_data = nullptr;
        uint64_t trigger_id = 0;
        {
            std::unique_lock<std::mutex> lock(simulator->mutex);
            if (simulator->condition.wait_until(
                    lock, next_deadline, [simulator] {
                        return simulator->stop_requested;
                    })) {
                simulator->running = false;
                return;
            }
            if (simulator->stop_requested) {
                simulator->running = false;
                return;
            }
            trigger_id = ++simulator->last_trigger_id;
            ++simulator->emitted_count;
            callback = simulator->callback;
            user_data = simulator->user_data;
        }
        callback(trigger_id, clock_ns(CLOCK_MONOTONIC),
                 clock_ns(CLOCK_REALTIME), user_data);
        {
            std::lock_guard<std::mutex> lock(simulator->mutex);
            if (simulator->requested_count &&
                simulator->emitted_count >= simulator->requested_count) {
                simulator->running = false;
                return;
            }
        }
        next_deadline += interval;
    }
}

}  // namespace

extern "C" int trigger_simulator_create(
    trigger_simulator_callback_t callback, void *user_data,
    trigger_simulator_t **simulator_out)
{
    if (!callback || !simulator_out)
        return TRIGGER_SIMULATOR_ERR_ARGUMENT;
    *simulator_out = nullptr;
    trigger_simulator_t *simulator = new (std::nothrow) trigger_simulator;
    if (!simulator)
        return TRIGGER_SIMULATOR_ERR_IO;
    simulator->callback = callback;
    simulator->user_data = user_data;
    *simulator_out = simulator;
    return TRIGGER_SIMULATOR_OK;
}

extern "C" void trigger_simulator_destroy(trigger_simulator_t *simulator)
{
    if (!simulator)
        return;
    trigger_simulator_stop(simulator);
    delete simulator;
}

extern "C" int trigger_simulator_start(trigger_simulator_t *simulator,
                                          uint32_t frequency_hz,
                                          uint32_t count)
{
    if (!simulator || (frequency_hz != 2 && frequency_hz != 4))
        return TRIGGER_SIMULATOR_ERR_ARGUMENT;
    trigger_simulator_stop(simulator);
    {
        std::lock_guard<std::mutex> lock(simulator->mutex);
        simulator->stop_requested = false;
        simulator->running = true;
        simulator->frequency_hz = frequency_hz;
        simulator->requested_count = count;
        simulator->emitted_count = 0;
    }
    try {
        simulator->worker = std::thread(worker_main, simulator);
    } catch (...) {
        std::lock_guard<std::mutex> lock(simulator->mutex);
        simulator->running = false;
        return TRIGGER_SIMULATOR_ERR_IO;
    }
    return TRIGGER_SIMULATOR_OK;
}

extern "C" int trigger_simulator_stop(trigger_simulator_t *simulator)
{
    if (!simulator)
        return TRIGGER_SIMULATOR_ERR_ARGUMENT;
    {
        std::lock_guard<std::mutex> lock(simulator->mutex);
        simulator->stop_requested = true;
    }
    simulator->condition.notify_all();
    if (simulator->worker.joinable())
        simulator->worker.join();
    std::lock_guard<std::mutex> lock(simulator->mutex);
    simulator->running = false;
    return TRIGGER_SIMULATOR_OK;
}

extern "C" int trigger_simulator_get_status(
    trigger_simulator_t *simulator, trigger_simulator_status_t *status)
{
    if (!simulator || !status)
        return TRIGGER_SIMULATOR_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(simulator->mutex);
    std::memset(status, 0, sizeof(*status));
    status->running = simulator->running ? 1 : 0;
    status->frequency_hz = simulator->frequency_hz;
    status->requested_count = simulator->requested_count;
    status->emitted_count = simulator->emitted_count;
    status->last_trigger_id = simulator->last_trigger_id;
    return TRIGGER_SIMULATOR_OK;
}

extern "C" const char *trigger_simulator_strerror(int result)
{
    switch (result) {
    case TRIGGER_SIMULATOR_OK:
        return "success";
    case TRIGGER_SIMULATOR_ERR_ARGUMENT:
        return "frequency must be 2 or 4 Hz";
    case TRIGGER_SIMULATOR_ERR_RUNNING:
        return "simulator is already running";
    case TRIGGER_SIMULATOR_ERR_IO:
        return "unable to start simulator thread";
    default:
        return "unknown trigger simulator error";
    }
}
