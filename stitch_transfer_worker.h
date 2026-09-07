#ifndef STITCH_TRANSFER_WORKER_H
#define STITCH_TRANSFER_WORKER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace camera_stitch_transfer {

enum result {
    ok = 0,
    err_argument = -800,
    err_already_running = -801,
    err_not_running = -802,
    err_queue_full = -803,
    err_stop_timeout = -804,
};

struct item {
    std::string path;
    std::uint64_t pair_id = 0;
    /* Transfer identity and wire-level file name (receiver names files
       session_<sid>_cam<id>_frame_<frame_id>).  Filled with a unix-ns
       timestamp at enqueue so that a re-stitched version of the same pair
       never collides with its predecessor at the receiver (the historic
       same-name-different-bytes head-of-line deadlock). */
    std::uint64_t transfer_id = 0;
    std::uint64_t exposure_start_utc_ns = 0;
    std::uint64_t exposure_duration_ns = 0;
    std::uint64_t payload_size = 0;
};

struct status {
    bool enabled = false;
    bool processing = false;
    std::string host;
    std::uint16_t port = 0;
    std::uint64_t session_id = 0;
    std::uint64_t enqueued = 0;
    std::uint64_t delivered = 0;
    std::uint64_t retries = 0;
    std::uint64_t failed = 0;
    std::uint64_t queue_rejected = 0;
    std::uint64_t delete_errors = 0;
    std::uint64_t queue_pending = 0;
    std::uint64_t backlog = 0;
    std::uint64_t backlog_peak = 0;
    std::uint64_t deferred = 0;
    std::uint64_t recovered = 0;
    std::uint64_t spool_recovered = 0;
    std::uint64_t orphan_recovered = 0;
    std::uint64_t orphan_unrecoverable = 0;
    std::uint64_t spool_recovery_errors = 0;
    std::uint64_t bytes = 0;
    int last_result = 0;
    int last_errno = 0;
    std::string last_error;
    std::string last_path;
};

class worker {
public:
    explicit worker(std::size_t queue_depth = 32, int timeout_ms = 10000,
                    unsigned int maximum_retries = 3,
                    int stop_wait_ms = 2000);
    ~worker();

    worker(const worker &) = delete;
    worker &operator=(const worker &) = delete;

    int start(const std::string &host, std::uint16_t port,
              const std::string &spool_directory = {});
    int enqueue(item work);
    int stop();
    status get_status() const;

private:
    class implementation;
    std::unique_ptr<implementation> implementation_;
};

const char *strerror(int value);

}  // namespace camera_stitch_transfer

#endif
