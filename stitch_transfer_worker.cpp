#include "stitch_transfer_worker.h"

#include "camera_transfer_client.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <ctime>
#include <deque>
#include <dirent.h>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <sys/random.h>
#include <sys/stat.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace camera_stitch_transfer {
namespace {

struct file_identity {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t size = 0;
    std::int64_t mtime_seconds = 0;
    std::int64_t mtime_nanoseconds = 0;
};

std::uint64_t generate_session_id()
{
    std::uint64_t value = 0;
    if (getrandom(&value, sizeof(value), 0) ==
            static_cast<ssize_t>(sizeof(value)) &&
        value) {
        return value;
    }
    struct timespec now = {};
    clock_gettime(CLOCK_REALTIME, &now);
    value = static_cast<std::uint64_t>(now.tv_sec) << 32U;
    value ^= static_cast<std::uint64_t>(now.tv_nsec);
    value ^= static_cast<std::uint64_t>(getpid()) << 16U;
    return value ? value : 1U;
}

bool same_identity(const file_identity &left, const file_identity &right)
{
    return left.device == right.device && left.inode == right.inode &&
           left.size == right.size &&
           left.mtime_seconds == right.mtime_seconds &&
           left.mtime_nanoseconds == right.mtime_nanoseconds;
}

int snapshot_file(const std::string &input, std::string *canonical_path,
                  file_identity *identity)
{
    if (!canonical_path || !identity || input.empty())
        return EINVAL;
    struct stat original = {};
    if (lstat(input.c_str(), &original) != 0)
        return errno ? errno : EIO;
    if (!S_ISREG(original.st_mode) || S_ISLNK(original.st_mode))
        return EINVAL;

    char *resolved = realpath(input.c_str(), nullptr);
    if (!resolved)
        return errno ? errno : EIO;
    *canonical_path = resolved;
    std::free(resolved);

    struct stat current = {};
    if (lstat(canonical_path->c_str(), &current) != 0)
        return errno ? errno : EIO;
    if (!S_ISREG(current.st_mode) || S_ISLNK(current.st_mode) ||
        current.st_size <= 0) {
        return EINVAL;
    }
    identity->device = static_cast<std::uint64_t>(current.st_dev);
    identity->inode = static_cast<std::uint64_t>(current.st_ino);
    identity->size = static_cast<std::uint64_t>(current.st_size);
    identity->mtime_seconds = current.st_mtim.tv_sec;
    identity->mtime_nanoseconds = current.st_mtim.tv_nsec;
    return 0;
}

std::string canonical_directory(const std::string &input)
{
    if (input.empty())
        return {};
    char *resolved = realpath(input.c_str(), nullptr);
    if (!resolved)
        return {};
    std::string output = resolved;
    std::free(resolved);
    struct stat info = {};
    if (stat(output.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
        return {};
    return output;
}

std::string path_directory(const std::string &path)
{
    const std::size_t separator = path.rfind('/');
    if (separator == std::string::npos)
        return ".";
    if (separator == 0)
        return "/";
    return path.substr(0, separator);
}

std::string path_basename(const std::string &path)
{
    const std::size_t separator = path.rfind('/');
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

bool is_stitch_jpeg_name(const std::string &name)
{
    static const std::string prefix = "stitch_pair_";
    static const std::string suffix = ".jpg";
    if (name.size() <= suffix.size() ||
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
        return false;
    if (name.size() > prefix.size() + suffix.size() &&
        name.compare(0, prefix.size(), prefix) == 0)
        return true;
    /* New naming: pure unix-ns timestamp "<digits>.jpg". */
    for (std::size_t i = 0; i + suffix.size() < name.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(name[i])))
            return false;
    return true;
}

bool split_csv_line(const std::string &line, std::vector<std::string> *fields)
{
    if (!fields)
        return false;
    fields->clear();
    std::string field;
    bool quoted = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if (quoted) {
            if (character == '"') {
                if (index + 1 < line.size() && line[index + 1] == '"') {
                    field.push_back('"');
                    ++index;
                } else {
                    quoted = false;
                }
            } else {
                field.push_back(character);
            }
        } else if (character == ',') {
            fields->push_back(std::move(field));
            field.clear();
        } else if (character == '"' && field.empty()) {
            quoted = true;
        } else if (character != '\r') {
            field.push_back(character);
        }
    }
    if (quoted)
        return false;
    fields->push_back(std::move(field));
    return true;
}

bool parse_u64(const std::string &text, std::uint64_t *value)
{
    if (!value || text.empty() || text.front() == '-')
        return false;
    errno = 0;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (errno || !end || *end != '\0')
        return false;
    *value = static_cast<std::uint64_t>(parsed);
    return true;
}

bool parse_orphan_name(const std::string &name, std::uint64_t *pair_id,
                       std::uint64_t *timestamp_ns)
{
    static const std::string prefix = "stitch_pair_";
    static const std::string top_marker = "_top_";
    static const std::string suffix = ".jpg";
    if (!pair_id || !timestamp_ns || !is_stitch_jpeg_name(name))
        return false;
    if (name.compare(0, prefix.size(), prefix) != 0) {
        /* New pure-timestamp name: <unix-ns>.jpg */
        const std::uint64_t ts = std::strtoull(name.c_str(), nullptr, 10);
        if (!ts)
            return false;
        *pair_id = ts;
        *timestamp_ns = ts;
        return true;
    }
    const std::size_t top = name.find(top_marker, prefix.size());
    const std::size_t final_separator = name.rfind('_');
    if (top == std::string::npos || final_separator == std::string::npos ||
        final_separator <= top || final_separator + 1 >= name.size() - 4) {
        return false;
    }
    return parse_u64(name.substr(prefix.size(), top - prefix.size()), pair_id) &&
           *pair_id &&
           parse_u64(name.substr(final_separator + 1,
                                 name.size() - suffix.size() -
                                     final_separator - 1),
                     timestamp_ns) &&
           *timestamp_ns;
}

bool extract_top_exif_metadata(const std::string &path,
                               std::uint64_t *exposure_start_ns,
                               std::uint64_t *exposure_us)
{
    if (!exposure_start_ns || !exposure_us)
        return false;
    std::ifstream jpeg(path, std::ios::binary);
    if (!jpeg)
        return false;
    std::string prefix(128U * 1024U, '\0');
    jpeg.read(&prefix[0], static_cast<std::streamsize>(prefix.size()));
    prefix.resize(static_cast<std::size_t>(jpeg.gcount()));
    const std::size_t top = prefix.find(";top={");
    const std::size_t bottom = prefix.find("};bottom={", top);
    if (top == std::string::npos || bottom == std::string::npos)
        return false;

    const auto extract = [&](const char *key, std::uint64_t *output) {
        const std::size_t key_position = prefix.find(key, top);
        if (key_position == std::string::npos || key_position >= bottom)
            return false;
        const std::size_t value_start = key_position + std::strlen(key);
        const std::size_t value_end = prefix.find(';', value_start);
        if (value_end == std::string::npos || value_end > bottom)
            return false;
        return parse_u64(prefix.substr(value_start, value_end - value_start),
                         output);
    };
    *exposure_start_ns = 0;
    extract("exposure_start_realtime_ns=", exposure_start_ns);
    return extract("exposure_us=", exposure_us) && *exposure_us;
}

int unlink_if_same_file(const std::string &path,
                        const file_identity &expected)
{
    std::string canonical_path;
    file_identity current;
    const int snapshot_error = snapshot_file(path, &canonical_path, &current);
    if (snapshot_error == ENOENT)
        return 0;
    if (snapshot_error)
        return snapshot_error;
    if (canonical_path != path || !same_identity(current, expected))
        return ESTALE;
    if (unlink(path.c_str()) != 0)
        return errno ? errno : EIO;
    return 0;
}

bool permanent_transfer_error(int value)
{
    return value == camera_transfer::err_argument ||
           value == camera_transfer::err_protocol ||
           value == camera_transfer::err_conflict;
}

/* Append one line to transfer_dropped.csv next to the retained JPEG for
   every photo that will never be delivered (permanent rejection or a
   local file that went missing/changed).  Best effort: logging problems
   must never affect the queue. */
void log_dropped_item(const std::string &path, std::uint64_t transfer_id,
                      std::uint64_t pair_id, unsigned int attempts,
                      int result, const std::string &detail)
{
    const std::string directory = path_directory(path);
    if (directory.empty())
        return;
    const std::string log_path = directory + "/transfer_dropped.csv";
    struct stat info = {};
    const bool need_header =
        stat(log_path.c_str(), &info) != 0 || info.st_size <= 0;
    std::ofstream log(log_path, std::ios::out | std::ios::app);
    if (!log)
        return;
    if (need_header)
        log << "drop_realtime_ns,path,transfer_id,pair_id,attempts,result,"
               "error\n";
    struct timespec now = {};
    clock_gettime(CLOCK_REALTIME, &now);
    const std::uint64_t now_ns =
        static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL +
        static_cast<std::uint64_t>(now.tv_nsec);
    std::string escaped = detail;
    for (char &ch : escaped) {
        if (ch == '"' || ch == ',' || ch == '\n' || ch == '\r')
            ch = ';';
    }
    log << now_ns << ',' << path << ',' << transfer_id << ',' << pair_id << ','
        << attempts << ',' << result << ',' << escaped << '\n';
}

std::string identity_key(const file_identity &identity)
{
    return std::to_string(identity.device) + ':' +
           std::to_string(identity.inode);
}

}  // namespace

class worker::implementation {
public:
    implementation(std::size_t queue_depth, int timeout_ms,
                   unsigned int maximum_retries, int stop_wait_ms)
        : queue_depth_(queue_depth), timeout_ms_(timeout_ms),
          maximum_retries_(maximum_retries), stop_wait_ms_(stop_wait_ms)
    {
        thread_ = std::thread(&implementation::run, this);
    }

    ~implementation()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            accepting_ = false;
            draining_ = false;
            stop_worker_ = true;
            condition_.notify_all();
        }
        if (thread_.joinable())
            thread_.join();
    }

    int start(const std::string &host, std::uint16_t port,
              const std::string &spool_directory)
    {
        if (host.empty() || !port || !queue_depth_ || timeout_ms_ <= 0 ||
            stop_wait_ms_ <= 0) {
            return err_argument;
        }
        std::string canonical_spool;
        if (!spool_directory.empty()) {
            canonical_spool = canonical_directory(spool_directory);
            if (canonical_spool.empty())
                return err_argument;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (accepting_ || draining_ || processing_)
            return err_already_running;
        if (client_)
            client_->disconnect();
        host_ = host;
        port_ = port;
        session_id_ = generate_session_id();
        client_.reset(new camera_transfer::client(host, port, timeout_ms_));
        spool_directory_ = canonical_spool;
        spool_scan_attempted_ = false;
        enqueued_ = queue_.size();
        delivered_ = 0;
        retries_ = 0;
        failed_ = 0;
        queue_rejected_ = 0;
        delete_errors_ = 0;
        backlog_peak_ = queue_.size();
        deferred_ = 0;
        recovered_ = 0;
        spool_recovered_ = 0;
        orphan_recovered_ = 0;
        orphan_unrecoverable_ = 0;
        spool_recovery_errors_ = 0;
        bytes_ = 0;
        consecutive_failed_cycles_ = 0;
        retry_not_before_ = std::chrono::steady_clock::time_point{};
        last_result_ = camera_transfer::ok;
        last_errno_ = 0;
        last_error_.clear();
        last_path_.clear();
        for (queued_item &queued : queue_) {
            queued.restored = true;
            queued.deferred = false;
            queued.attempts = 0;
        }
        rebuild_queue_index_locked();
        if (!spool_directory_.empty()) {
            recover_spool_locked(spool_directory_, {});
            spool_scan_attempted_ = true;
        }
        accepting_ = true;
        condition_.notify_all();
        return ok;
    }

    int enqueue(item work)
    {
        if (work.path.empty() || !work.pair_id ||
            !work.exposure_start_utc_ns || !work.exposure_duration_ns ||
            !work.payload_size) {
            return err_argument;
        }

        queued_item queued;
        const int snapshot_error =
            snapshot_file(work.path, &queued.work.path, &queued.identity);
        if (snapshot_error || queued.identity.size != work.payload_size)
            return err_argument;
        work.path = queued.work.path;
        if (!work.transfer_id) {
            /* Name by unix timestamp: exposure start when available,
               otherwise the enqueue-time realtime clock. */
            if (work.exposure_start_utc_ns) {
                work.transfer_id = work.exposure_start_utc_ns;
            } else {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                work.transfer_id =
                    static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
                    static_cast<std::uint64_t>(ts.tv_nsec);
            }
        }
        queued.work = std::move(work);

        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_)
            return err_not_running;
        if (!spool_scan_attempted_) {
            spool_directory_ =
                canonical_directory(path_directory(queued.work.path));
            if (!spool_directory_.empty())
                recover_spool_locked(spool_directory_, queued.work.path);
            spool_scan_attempted_ = true;
        }
        if (contains_locked(queued.work.path, queued.identity))
            return ok;
        push_back_locked(std::move(queued));
        enqueued_++;
        backlog_peak_ = std::max<std::uint64_t>(backlog_peak_, queue_.size());
        condition_.notify_all();
        return ok;
    }

    int stop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!accepting_ && !draining_)
            return err_not_running;

        accepting_ = false;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(stop_wait_ms_);
        stop_deadline_ = deadline;
        draining_ = !queue_.empty() || processing_;
        condition_.notify_all();
        if (!draining_ && !processing_) {
            if (client_)
                client_->disconnect();
            client_.reset();
            return ok;
        }

        if (!idle_.wait_until(lock, deadline, [this] {
                return !draining_ && !processing_;
            })) {
            draining_ = false;
            last_result_ = err_stop_timeout;
            last_errno_ = ETIMEDOUT;
            last_error_ = "transfer stop timed out; unsent JPEGs retained";
            condition_.notify_all();
            return err_stop_timeout;
        }
        if (client_)
            client_->disconnect();
        client_.reset();
        return ok;
    }

    status get_status() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status output;
        output.enabled = accepting_;
        output.processing = processing_;
        output.host = host_;
        output.port = port_;
        output.session_id = session_id_;
        output.enqueued = enqueued_;
        output.delivered = delivered_;
        output.retries = retries_;
        output.failed = failed_;
        output.queue_rejected = queue_rejected_;
        output.delete_errors = delete_errors_;
        output.queue_pending = queue_.size();
        output.backlog = queue_.size();
        output.backlog_peak = backlog_peak_;
        output.deferred = deferred_;
        output.recovered = recovered_;
        output.spool_recovered = spool_recovered_;
        output.orphan_recovered = orphan_recovered_;
        output.orphan_unrecoverable = orphan_unrecoverable_;
        output.spool_recovery_errors = spool_recovery_errors_;
        output.bytes = bytes_;
        output.last_result = last_result_;
        output.last_errno = last_errno_;
        output.last_error = last_error_;
        output.last_path = last_path_;
        return output;
    }

private:
    struct queued_item {
        item work;
        file_identity identity;
        std::uint64_t attempts = 0;
        bool deferred = false;
        bool restored = false;
    };

    bool contains_locked(const std::string &path,
                         const file_identity &identity) const
    {
        return queued_paths_.find(path) != queued_paths_.end() ||
               queued_identities_.find(identity_key(identity)) !=
                   queued_identities_.end();
    }

    void rebuild_queue_index_locked()
    {
        queued_paths_.clear();
        queued_identities_.clear();
        for (const queued_item &queued : queue_) {
            queued_paths_.insert(queued.work.path);
            queued_identities_.insert(identity_key(queued.identity));
        }
    }

    void push_back_locked(queued_item queued)
    {
        queued_paths_.insert(queued.work.path);
        queued_identities_.insert(identity_key(queued.identity));
        queue_.push_back(std::move(queued));
    }

    void pop_front_locked()
    {
        queued_paths_.erase(queue_.front().work.path);
        queued_identities_.erase(identity_key(queue_.front().identity));
        queue_.pop_front();
    }

    void record_recovery_error_locked(const std::string &path,
                                      const std::string &detail,
                                      int error_number)
    {
        spool_recovery_errors_++;
        last_result_ = camera_transfer::err_io;
        last_errno_ = error_number;
        last_error_ = detail;
        last_path_ = path;
    }

    void append_recovered_locked(queued_item queued, bool orphan)
    {
        queued.restored = true;
        push_back_locked(std::move(queued));
        enqueued_++;
        spool_recovered_++;
        if (orphan)
            orphan_recovered_++;
        backlog_peak_ = std::max<std::uint64_t>(backlog_peak_, queue_.size());
    }

    void recover_csv_locked(const std::string &directory,
                            const std::string &csv_path,
                            const std::string &excluded_path)
    {
        std::ifstream csv(csv_path);
        if (!csv) {
            if (access(csv_path.c_str(), F_OK) == 0) {
                record_recovery_error_locked(
                    csv_path, "cannot open stitch spool metadata", EACCES);
            }
            return;
        }

        std::string line;
        std::vector<std::string> fields;
        if (!std::getline(csv, line) || !split_csv_line(line, &fields)) {
            record_recovery_error_locked(
                csv_path, "invalid stitch spool metadata header", EINVAL);
            return;
        }
        std::unordered_map<std::string, std::size_t> columns;
        for (std::size_t index = 0; index < fields.size(); ++index)
            columns.emplace(fields[index], index);
        const char *required[] = {"pair_id", "top_exposure_start_realtime_ns",
                                  "top_exposure_us", "jpeg_path"};
        for (const char *name : required) {
            if (columns.find(name) == columns.end()) {
                record_recovery_error_locked(
                    csv_path, std::string("stitch spool metadata lacks ") + name,
                    EINVAL);
                return;
            }
        }

        const std::size_t pair_column = columns["pair_id"];
        const std::size_t start_column =
            columns["top_exposure_start_realtime_ns"];
        const std::size_t duration_column = columns["top_exposure_us"];
        const std::size_t path_column = columns["jpeg_path"];
        const std::size_t largest_column =
            std::max({pair_column, start_column, duration_column, path_column});

        while (std::getline(csv, line)) {
            if (line.empty() || !split_csv_line(line, &fields) ||
                fields.size() <= largest_column) {
                continue;
            }
            std::uint64_t pair_id = 0;
            std::uint64_t exposure_start = 0;
            std::uint64_t exposure_us = 0;
            if (!parse_u64(fields[pair_column], &pair_id) || !pair_id ||
                !parse_u64(fields[start_column], &exposure_start) ||
                !exposure_start ||
                !parse_u64(fields[duration_column], &exposure_us) ||
                !exposure_us ||
                exposure_us >
                    std::numeric_limits<std::uint64_t>::max() / 1000ULL) {
                continue;
            }

            queued_item queued;
            const int file_error =
                snapshot_file(fields[path_column], &queued.work.path,
                              &queued.identity);
            if (file_error == ENOENT)
                continue;
            if (file_error) {
                record_recovery_error_locked(fields[path_column],
                                             "invalid retained stitch JPEG",
                                             file_error);
                continue;
            }
            if (queued.work.path == excluded_path ||
                path_directory(queued.work.path) != directory ||
                !is_stitch_jpeg_name(path_basename(queued.work.path)) ||
                contains_locked(queued.work.path, queued.identity)) {
                continue;
            }
            queued.work.pair_id = pair_id;
            queued.work.exposure_start_utc_ns = exposure_start;
            queued.work.exposure_duration_ns = exposure_us * 1000ULL;
            queued.work.payload_size = queued.identity.size;
            append_recovered_locked(std::move(queued), false);
        }
    }

    void recover_orphans_locked(const std::string &directory,
                                const std::string &excluded_path)
    {
        DIR *entries = opendir(directory.c_str());
        if (!entries) {
            record_recovery_error_locked(directory,
                                         "cannot scan stitch spool directory",
                                         errno ? errno : EIO);
            return;
        }
        for (;;) {
            errno = 0;
            struct dirent *entry = readdir(entries);
            if (!entry)
                break;
            const std::string name = entry->d_name;
            if (!is_stitch_jpeg_name(name))
                continue;
            const std::string path = directory + "/" + name;
            queued_item queued;
            const int file_error =
                snapshot_file(path, &queued.work.path, &queued.identity);
            if (file_error || queued.work.path == excluded_path ||
                contains_locked(queued.work.path, queued.identity)) {
                continue;
            }

            std::uint64_t filename_timestamp = 0;
            std::uint64_t exposure_start = 0;
            std::uint64_t exposure_us = 0;
            if (!parse_orphan_name(name, &queued.work.pair_id,
                                   &filename_timestamp) ||
                !extract_top_exif_metadata(path, &exposure_start,
                                           &exposure_us) ||
                exposure_us >
                    std::numeric_limits<std::uint64_t>::max() / 1000ULL) {
                orphan_unrecoverable_++;
                last_result_ = camera_transfer::err_io;
                last_errno_ = ENODATA;
                last_error_ =
                    "orphan stitch JPEG lacks recoverable transfer metadata";
                last_path_ = path;
                continue;
            }
            queued.work.exposure_start_utc_ns =
                exposure_start ? exposure_start : filename_timestamp;
            queued.work.transfer_id = filename_timestamp;
            queued.work.exposure_duration_ns = exposure_us * 1000ULL;
            queued.work.payload_size = queued.identity.size;
            append_recovered_locked(std::move(queued), true);
        }
        const int scan_error = errno;
        closedir(entries);
        if (scan_error) {
            record_recovery_error_locked(directory,
                                         "stitch spool directory scan failed",
                                         scan_error);
        }
    }

    void recover_spool_locked(const std::string &directory,
                              const std::string &excluded_path)
    {
        recover_csv_locked(directory, directory + "/stitch_metadata.csv",
                           excluded_path);
        recover_csv_locked(directory, directory + "/stitch_metadata_v2.csv",
                           excluded_path);
        recover_orphans_locked(directory, excluded_path);
    }

    bool wait_before_retry()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return !condition_.wait_for(
            lock, std::chrono::milliseconds(100),
            [this] {
                return stop_worker_ || (!accepting_ && !draining_) ||
                       (draining_ && std::chrono::steady_clock::now() >=
                                         stop_deadline_);
            });
    }

    void run()
    {
        for (;;) {
            queued_item current;
            camera_transfer::client *client = nullptr;
            std::uint64_t session_id = 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                for (;;) {
                    condition_.wait(lock, [this] {
                        return stop_worker_ ||
                               ((accepting_ || draining_) && !queue_.empty());
                    });
                    if (stop_worker_)
                        return;
                    const auto now = std::chrono::steady_clock::now();
                    if (draining_ && now >= stop_deadline_) {
                        draining_ = false;
                        idle_.notify_all();
                        continue;
                    }
                    if (!accepting_ && !draining_)
                        continue;
                    if (accepting_ && !draining_ &&
                        now < retry_not_before_) {
                        condition_.wait_until(lock, retry_not_before_, [this] {
                            return stop_worker_ || draining_ || !accepting_;
                        });
                        continue;
                    }
                    current = queue_.front();
                    processing_ = true;
                    client = client_.get();
                    session_id = session_id_;
                    break;
                }
            }

            bool local_failure = false;
            int operation_errno = 0;
            std::string canonical_path;
            file_identity before_send;
            operation_errno = snapshot_file(current.work.path, &canonical_path,
                                             &before_send);
            if (operation_errno || canonical_path != current.work.path ||
                !same_identity(before_send, current.identity)) {
                local_failure = true;
            }

            int send_result = camera_transfer::err_io;
            std::string detail = local_failure
                                     ? "retained JPEG is missing or changed"
                                     : "transfer client unavailable";
            camera_transfer::ack response;
            if (!local_failure && client) {
                camera_transfer::send_request request;
                request.path = current.work.path;
                request.format = camera_transfer::photo_format::jpeg;
                request.camera_id = 0;
                request.session_id = session_id;
                request.frame_id = current.work.transfer_id
                                       ? current.work.transfer_id
                                       : current.work.pair_id;
                request.exposure_start_utc_ns =
                    current.work.exposure_start_utc_ns;
                request.exposure_duration_ns =
                    current.work.exposure_duration_ns;
                for (unsigned int attempt = 0;
                     attempt <= maximum_retries_; ++attempt) {
                    if (attempt && !wait_before_retry())
                        break;
                    if (current.attempts) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        retries_++;
                    }
                    current.attempts++;
                    detail.clear();
                    errno = 0;
                    send_result = client->send_photo(request, &response, &detail);
                    operation_errno = errno;
                    if (send_result == camera_transfer::ok ||
                        permanent_transfer_error(send_result)) {
                        break;
                    }
                    client->disconnect();
                }
            }

            int delete_error = 0;
            if (send_result == camera_transfer::ok)
                delete_error =
                    unlink_if_same_file(current.work.path, current.identity);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                last_path_ = current.work.path;
                last_result_ = send_result;
                last_errno_ = 0;
                if (send_result == camera_transfer::ok) {
                    pop_front_locked();
                    delivered_++;
                    bytes_ += current.work.payload_size;
                    if (current.deferred || current.restored)
                        recovered_++;
                    consecutive_failed_cycles_ = 0;
                    retry_not_before_ =
                        std::chrono::steady_clock::time_point{};
                    if (delete_error) {
                        delete_errors_++;
                        last_errno_ = delete_error;
                        last_error_ =
                            std::string("ACK OK but JPEG was retained: ") +
                            std::strerror(delete_error);
                    } else {
                        last_error_.clear();
                    }
                } else if (local_failure ||
                           permanent_transfer_error(send_result)) {
                    pop_front_locked();
                    failed_++;
                    last_errno_ = operation_errno ? operation_errno : EINVAL;
                    last_error_ = detail.empty()
                                      ? camera_transfer::strerror(send_result)
                                      : detail;
                    log_dropped_item(current.work.path,
                                     current.work.transfer_id,
                                     current.work.pair_id, current.attempts,
                                     send_result, last_error_);
                } else {
                    queued_item &queued = queue_.front();
                    queued.attempts = current.attempts;
                    if (!queued.deferred) {
                        queued.deferred = true;
                        deferred_++;
                    }
                    if (accepting_ && !draining_) {
                        queued_item postponed = std::move(queued);
                        queue_.pop_front();
                        queue_.push_back(std::move(postponed));
                    }
                    consecutive_failed_cycles_++;
                    const unsigned int shift =
                        std::min(consecutive_failed_cycles_ - 1U, 4U);
                    const unsigned int delay_ms =
                        std::min(5000U, 250U << shift);
                    retry_not_before_ = std::chrono::steady_clock::now() +
                                        std::chrono::milliseconds(delay_ms);
                    last_errno_ = operation_errno;
                    last_error_ = detail.empty()
                                      ? camera_transfer::strerror(send_result)
                                      : detail;
                    if (draining_ || !accepting_)
                        draining_ = false;
                }
                processing_ = false;
                if (draining_ &&
                    (queue_.empty() ||
                     std::chrono::steady_clock::now() >= stop_deadline_)) {
                    draining_ = false;
                }
                idle_.notify_all();
                condition_.notify_all();
            }
        }
    }

    const std::size_t queue_depth_;
    const int timeout_ms_;
    const unsigned int maximum_retries_;
    const int stop_wait_ms_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::condition_variable idle_;
    bool stop_worker_ = false;
    bool accepting_ = false;
    bool draining_ = false;
    bool processing_ = false;
    bool spool_scan_attempted_ = false;
    std::deque<queued_item> queue_;
    std::unordered_set<std::string> queued_paths_;
    std::unordered_set<std::string> queued_identities_;
    std::unique_ptr<camera_transfer::client> client_;
    std::string host_;
    std::string spool_directory_;
    std::uint16_t port_ = 0;
    std::uint64_t session_id_ = 0;
    std::uint64_t enqueued_ = 0;
    std::uint64_t delivered_ = 0;
    std::uint64_t retries_ = 0;
    std::uint64_t failed_ = 0;
    std::uint64_t queue_rejected_ = 0;
    std::uint64_t delete_errors_ = 0;
    std::uint64_t backlog_peak_ = 0;
    std::uint64_t deferred_ = 0;
    std::uint64_t recovered_ = 0;
    std::uint64_t spool_recovered_ = 0;
    std::uint64_t orphan_recovered_ = 0;
    std::uint64_t orphan_unrecoverable_ = 0;
    std::uint64_t spool_recovery_errors_ = 0;
    std::uint64_t bytes_ = 0;
    unsigned int consecutive_failed_cycles_ = 0;
    int last_result_ = camera_transfer::ok;
    int last_errno_ = 0;
    std::string last_error_;
    std::string last_path_;
    std::chrono::steady_clock::time_point retry_not_before_{};
    std::chrono::steady_clock::time_point stop_deadline_{};
    std::thread thread_;
};

worker::worker(std::size_t queue_depth, int timeout_ms,
               unsigned int maximum_retries, int stop_wait_ms)
    : implementation_(new implementation(queue_depth, timeout_ms,
                                         maximum_retries, stop_wait_ms))
{
}

worker::~worker() = default;

int worker::start(const std::string &host, std::uint16_t port,
                  const std::string &spool_directory)
{
    return implementation_->start(host, port, spool_directory);
}

int worker::enqueue(item work)
{
    return implementation_->enqueue(std::move(work));
}

int worker::stop()
{
    return implementation_->stop();
}

status worker::get_status() const
{
    return implementation_->get_status();
}

const char *strerror(int value)
{
    switch (value) {
    case ok:
        return "ok";
    case err_argument:
        return "invalid argument";
    case err_already_running:
        return "transfer already running";
    case err_not_running:
        return "transfer is not running";
    case err_queue_full:
        return "transfer queue is full";
    case err_stop_timeout:
        return "transfer stop timed out; unsent JPEGs retained";
    default:
        return "unknown stitch transfer error";
    }
}

}  // namespace camera_stitch_transfer
