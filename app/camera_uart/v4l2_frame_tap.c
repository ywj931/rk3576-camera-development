#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BUFFER_COUNT 4
#define PLANE_COUNT 2

struct mapped_plane {
    void *address;
    size_t length;
};

struct mapped_buffer {
    struct mapped_plane planes[PLANE_COUNT];
};

static volatile sig_atomic_t snapshot_requested;
static volatile sig_atomic_t stop_requested;

static void handle_signal(int signal_number)
{
    if (signal_number == SIGUSR1)
        snapshot_requested = 1;
    else
        stop_requested = 1;
}

static int xioctl(int fd, unsigned long request, void *argument)
{
    int result;

    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);

    return result;
}

static double monotonic_seconds(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static int write_all(int fd, const void *data, size_t length)
{
    const uint8_t *cursor = data;

    while (length > 0) {
        ssize_t written = write(fd, cursor, length);

        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        cursor += written;
        length -= (size_t)written;
    }

    return 0;
}

static int save_snapshot(const char *output_directory, unsigned int snapshot_index,
                         const struct mapped_buffer *mapped,
                         const struct v4l2_buffer *buffer)
{
    char temporary_path[512];
    char output_path[512];
    int output_fd;
    unsigned int plane;

    snprintf(temporary_path, sizeof(temporary_path), "%s/snapshot_%03u.nv12.tmp",
             output_directory, snapshot_index);
    snprintf(output_path, sizeof(output_path), "%s/snapshot_%03u.nv12",
             output_directory, snapshot_index);

    output_fd = open(temporary_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (output_fd < 0) {
        fprintf(stderr, "ERROR open snapshot %s: %s\n", temporary_path,
                strerror(errno));
        return -1;
    }

    for (plane = 0; plane < PLANE_COUNT; ++plane) {
        size_t bytes_used = buffer->m.planes[plane].bytesused;

        if (bytes_used == 0 || bytes_used > mapped->planes[plane].length)
            bytes_used = mapped->planes[plane].length;
        if (write_all(output_fd, mapped->planes[plane].address, bytes_used) < 0) {
            fprintf(stderr, "ERROR write snapshot %s: %s\n", temporary_path,
                    strerror(errno));
            close(output_fd);
            unlink(temporary_path);
            return -1;
        }
    }

    if (fsync(output_fd) < 0)
        fprintf(stderr, "WARN fsync snapshot %s: %s\n", temporary_path,
                strerror(errno));
    close(output_fd);

    if (rename(temporary_path, output_path) < 0) {
        fprintf(stderr, "ERROR rename snapshot %s: %s\n", output_path,
                strerror(errno));
        unlink(temporary_path);
        return -1;
    }

    printf("SNAPSHOT path=%s sequence=%u timestamp=%ld.%06ld bytes=%u\n",
           output_path, buffer->sequence, (long)buffer->timestamp.tv_sec,
           (long)buffer->timestamp.tv_usec,
           buffer->m.planes[0].bytesused + buffer->m.planes[1].bytesused);
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    const char *device;
    const char *output_directory;
    unsigned int width;
    unsigned int height;
    struct mapped_buffer mapped[BUFFER_COUNT] = {0};
    enum v4l2_buf_type buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    struct v4l2_capability capabilities = {0};
    struct v4l2_format format = {0};
    struct v4l2_requestbuffers request = {0};
    struct sigaction action = {0};
    unsigned int frame_count = 0;
    unsigned int interval_frames = 0;
    unsigned int snapshot_index = 0;
    double interval_start;
    int video_fd = -1;
    unsigned int index;
    int exit_code = EXIT_FAILURE;

    if (argc != 5) {
        fprintf(stderr, "Usage: %s DEVICE OUTPUT_DIR WIDTH HEIGHT\n", argv[0]);
        return EXIT_FAILURE;
    }

    device = argv[1];
    output_directory = argv[2];
    width = (unsigned int)strtoul(argv[3], NULL, 10);
    height = (unsigned int)strtoul(argv[4], NULL, 10);

    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR1, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    if (mkdir(output_directory, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "ERROR mkdir %s: %s\n", output_directory, strerror(errno));
        goto cleanup;
    }

    video_fd = open(device, O_RDWR | O_NONBLOCK);
    if (video_fd < 0) {
        fprintf(stderr, "ERROR open %s: %s\n", device, strerror(errno));
        goto cleanup;
    }

    if (xioctl(video_fd, VIDIOC_QUERYCAP, &capabilities) < 0) {
        fprintf(stderr, "ERROR VIDIOC_QUERYCAP: %s\n", strerror(errno));
        goto cleanup;
    }
    if (!(capabilities.device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) ||
        !(capabilities.device_caps & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "ERROR %s is not a streaming multiplanar capture node\n",
                device);
        goto cleanup;
    }

    format.type = buffer_type;
    format.fmt.pix_mp.width = width;
    format.fmt.pix_mp.height = height;
    format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;
    format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    format.fmt.pix_mp.num_planes = PLANE_COUNT;
    if (xioctl(video_fd, VIDIOC_S_FMT, &format) < 0) {
        fprintf(stderr, "ERROR VIDIOC_S_FMT: %s\n", strerror(errno));
        goto cleanup;
    }
    if (format.fmt.pix_mp.width != width || format.fmt.pix_mp.height != height ||
        format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12M ||
        format.fmt.pix_mp.num_planes != PLANE_COUNT) {
        fprintf(stderr,
                "ERROR unexpected format width=%u height=%u fourcc=%c%c%c%c planes=%u\n",
                format.fmt.pix_mp.width, format.fmt.pix_mp.height,
                format.fmt.pix_mp.pixelformat & 0xff,
                (format.fmt.pix_mp.pixelformat >> 8) & 0xff,
                (format.fmt.pix_mp.pixelformat >> 16) & 0xff,
                (format.fmt.pix_mp.pixelformat >> 24) & 0xff,
                format.fmt.pix_mp.num_planes);
        goto cleanup;
    }

    request.count = BUFFER_COUNT;
    request.type = buffer_type;
    request.memory = V4L2_MEMORY_MMAP;
    if (xioctl(video_fd, VIDIOC_REQBUFS, &request) < 0 || request.count < 2) {
        fprintf(stderr, "ERROR VIDIOC_REQBUFS: %s count=%u\n", strerror(errno),
                request.count);
        goto cleanup;
    }

    for (index = 0; index < request.count; ++index) {
        struct v4l2_buffer buffer = {0};
        struct v4l2_plane planes[PLANE_COUNT] = {0};
        unsigned int plane;

        buffer.type = buffer_type;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        buffer.m.planes = planes;
        buffer.length = PLANE_COUNT;
        if (xioctl(video_fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            fprintf(stderr, "ERROR VIDIOC_QUERYBUF index=%u: %s\n", index,
                    strerror(errno));
            goto cleanup;
        }

        for (plane = 0; plane < PLANE_COUNT; ++plane) {
            mapped[index].planes[plane].length = planes[plane].length;
            mapped[index].planes[plane].address =
                mmap(NULL, planes[plane].length, PROT_READ | PROT_WRITE, MAP_SHARED,
                     video_fd, planes[plane].m.mem_offset);
            if (mapped[index].planes[plane].address == MAP_FAILED) {
                mapped[index].planes[plane].address = NULL;
                fprintf(stderr, "ERROR mmap index=%u plane=%u: %s\n", index,
                        plane, strerror(errno));
                goto cleanup;
            }
        }

        if (xioctl(video_fd, VIDIOC_QBUF, &buffer) < 0) {
            fprintf(stderr, "ERROR VIDIOC_QBUF index=%u: %s\n", index,
                    strerror(errno));
            goto cleanup;
        }
    }

    if (xioctl(video_fd, VIDIOC_STREAMON, &buffer_type) < 0) {
        fprintf(stderr, "ERROR VIDIOC_STREAMON: %s\n", strerror(errno));
        goto cleanup;
    }

    printf("STREAM_READY device=%s width=%u height=%u buffers=%u pid=%ld\n",
           device, width, height, request.count, (long)getpid());
    fflush(stdout);
    interval_start = monotonic_seconds();

    while (!stop_requested) {
        struct pollfd descriptor = {.fd = video_fd, .events = POLLIN};
        struct v4l2_buffer buffer = {0};
        struct v4l2_plane planes[PLANE_COUNT] = {0};
        int poll_result = poll(&descriptor, 1, 2000);

        if (poll_result < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "ERROR poll: %s\n", strerror(errno));
            goto streamoff;
        }
        if (poll_result == 0) {
            fprintf(stderr, "WARN frame timeout total_frames=%u\n", frame_count);
            fflush(stderr);
            continue;
        }

        buffer.type = buffer_type;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.m.planes = planes;
        buffer.length = PLANE_COUNT;
        if (xioctl(video_fd, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN)
                continue;
            fprintf(stderr, "ERROR VIDIOC_DQBUF: %s\n", strerror(errno));
            goto streamoff;
        }

        ++frame_count;
        ++interval_frames;
        if (snapshot_requested) {
            snapshot_requested = 0;
            ++snapshot_index;
            save_snapshot(output_directory, snapshot_index, &mapped[buffer.index],
                          &buffer);
        }

        if (xioctl(video_fd, VIDIOC_QBUF, &buffer) < 0) {
            fprintf(stderr, "ERROR VIDIOC_QBUF: %s\n", strerror(errno));
            goto streamoff;
        }

        if (monotonic_seconds() - interval_start >= 2.0) {
            double now = monotonic_seconds();
            double elapsed = now - interval_start;

            printf("FPS device=%s fps=%.3f interval_frames=%u total_frames=%u\n",
                   device, interval_frames / elapsed, interval_frames, frame_count);
            fflush(stdout);
            interval_frames = 0;
            interval_start = now;
        }
    }

    exit_code = EXIT_SUCCESS;

streamoff:
    xioctl(video_fd, VIDIOC_STREAMOFF, &buffer_type);

cleanup:
    if (video_fd >= 0) {
        for (index = 0; index < BUFFER_COUNT; ++index) {
            unsigned int plane;

            for (plane = 0; plane < PLANE_COUNT; ++plane) {
                if (mapped[index].planes[plane].address)
                    munmap(mapped[index].planes[plane].address,
                           mapped[index].planes[plane].length);
            }
        }
        close(video_fd);
    }

    printf("STREAM_STOP device=%s total_frames=%u snapshots=%u\n",
           argc > 1 ? argv[1] : "unknown", frame_count, snapshot_index);
    fflush(stdout);
    return exit_code;
}
