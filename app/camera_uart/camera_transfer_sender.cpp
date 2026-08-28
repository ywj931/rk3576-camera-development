#include "camera_transfer_client.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

namespace {

bool parse_u64(const char *text, std::uint64_t *value)
{
    if (!text || !*text || !value || text[0] == '-')
        return false;
    errno = 0;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 0);
    if (errno || end == text || *end)
        return false;
    *value = parsed;
    return true;
}

bool parse_format(const std::string &text, camera_transfer::photo_format *format)
{
    if (text == "jpeg" || text == "jpg")
        *format = camera_transfer::photo_format::jpeg;
    else if (text == "png")
        *format = camera_transfer::photo_format::png;
    else if (text == "nv12" || text == "nm12")
        *format = camera_transfer::photo_format::nv12;
    else if (text == "dng")
        *format = camera_transfer::photo_format::dng;
    else
        return false;
    return true;
}

std::uint64_t generate_session_id()
{
    std::uint64_t value = 0;
    if (getrandom(&value, sizeof(value), 0) ==
            static_cast<ssize_t>(sizeof(value)) &&
        value)
        return value;
    struct timespec now = {};
    clock_gettime(CLOCK_REALTIME, &now);
    value = static_cast<std::uint64_t>(now.tv_sec) << 32U;
    value ^= static_cast<std::uint64_t>(now.tv_nsec);
    value ^= static_cast<std::uint64_t>(getpid()) << 16U;
    return value ? value : 1U;
}

void usage(const char *program)
{
    std::fprintf(
        stderr,
        "usage: %s HOST PORT FILE CAMERA_ID FRAME_ID EXPOSURE_START_UTC_NS "
        "EXPOSURE_DURATION_NS FORMAT [SESSION_ID]\n",
        program);
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 9 && argc != 10) {
        usage(argv[0]);
        return 2;
    }
    std::uint64_t port = 0;
    std::uint64_t camera_id = 0;
    camera_transfer::send_request request;
    if (!parse_u64(argv[2], &port) || !port || port > UINT16_MAX ||
        !parse_u64(argv[4], &camera_id) || camera_id > 1U ||
        !parse_u64(argv[5], &request.frame_id) ||
        !parse_u64(argv[6], &request.exposure_start_utc_ns) ||
        !request.exposure_start_utc_ns ||
        !parse_u64(argv[7], &request.exposure_duration_ns) ||
        !request.exposure_duration_ns || !parse_format(argv[8], &request.format)) {
        usage(argv[0]);
        return 2;
    }
    request.path = argv[3];
    request.camera_id = static_cast<std::uint8_t>(camera_id);
    request.session_id = generate_session_id();
    if (argc == 10 &&
        (!parse_u64(argv[9], &request.session_id) || !request.session_id)) {
        usage(argv[0]);
        return 2;
    }

    camera_transfer::client client(argv[1], static_cast<std::uint16_t>(port));
    camera_transfer::ack response;
    std::string error;
    const int result = client.send_photo(request, &response, &error);
    if (result != camera_transfer::ok) {
        std::fprintf(
            stderr,
            "PHOTO_TRANSFER_FAILED session=%016llx camera_id=%u frame_id=%llu "
            "code=%d reason=\"%s\" detail=\"%s\"\n",
            static_cast<unsigned long long>(request.session_id),
            request.camera_id,
            static_cast<unsigned long long>(request.frame_id), result,
            camera_transfer::strerror(result), error.c_str());
        return 1;
    }
    std::printf(
        "PHOTO_TRANSFER_OK session=%016llx camera_id=%u frame_id=%llu "
        "ack=%s\n",
        static_cast<unsigned long long>(request.session_id), request.camera_id,
        static_cast<unsigned long long>(request.frame_id),
        camera_transfer::ack_status_string(response.status));
    return 0;
}
