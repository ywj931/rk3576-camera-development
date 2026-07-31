#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define DEFAULT_DEVICE "/dev/ttyS9"
#define FRAME_MAX 256
#define RESPONSE_MAX 256
#define CAMERA_ID_GLOBAL 255U

static volatile sig_atomic_t stop_requested;

struct frame_reader {
    char data[FRAME_MAX];
    size_t used;
    int discarding;
};

static void handle_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}

static int configure_uart(int fd, struct termios *saved)
{
    struct termios tio;

    if (tcgetattr(fd, saved) < 0) {
        perror("tcgetattr");
        return -1;
    }

    tio = *saved;
    cfmakeraw(&tio);
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    tio.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
    tio.c_cflag |= CS8 | CLOCAL | CREAD;
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        perror("tcsetattr");
        return -1;
    }

    if (tcflush(fd, TCIOFLUSH) < 0) {
        perror("tcflush");
        return -1;
    }

    return 0;
}

static int parse_u32(const char *text, uint32_t *value)
{
    char *end;
    unsigned long parsed;

    if (!text || !*text)
        return -1;

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno || *end != '\0' || parsed > UINT32_MAX)
        return -1;

    *value = (uint32_t)parsed;
    return 0;
}

static int format_nack(char *response, size_t size, uint32_t sequence,
                       uint32_t camera_id, const char *reason)
{
    return snprintf(response, size, "$NACK,%u,%u,%s\r\n",
                    sequence, camera_id, reason);
}

static int dispatch_command(const char *input, char *response, size_t size)
{
    char frame[FRAME_MAX];
    char *tokens[5] = {0};
    char *saveptr = NULL;
    char *token;
    uint32_t sequence = 0;
    uint32_t camera_id = CAMERA_ID_GLOBAL;
    int count = 0;

    if (strlen(input) >= sizeof(frame))
        return format_nack(response, size, 0, CAMERA_ID_GLOBAL,
                           "FRAME_TOO_LONG");

    strcpy(frame, input);
    for (token = strtok_r(frame, ",", &saveptr);
         token && count < (int)(sizeof(tokens) / sizeof(tokens[0]));
         token = strtok_r(NULL, ",", &saveptr))
        tokens[count++] = token;

    if (count != 4 || strcmp(tokens[0], "$CAM") != 0)
        return format_nack(response, size, 0, CAMERA_ID_GLOBAL,
                           "BAD_FORMAT");

    if (parse_u32(tokens[1], &sequence) < 0)
        return format_nack(response, size, 0, CAMERA_ID_GLOBAL,
                           "BAD_SEQUENCE");

    if (parse_u32(tokens[2], &camera_id) < 0 || camera_id > CAMERA_ID_GLOBAL)
        return format_nack(response, size, sequence, CAMERA_ID_GLOBAL,
                           "BAD_CAMERA_ID");

    if (strcmp(tokens[3], "PING") == 0) {
        if (camera_id != 0 && camera_id != 1 &&
            camera_id != CAMERA_ID_GLOBAL)
            return format_nack(response, size, sequence, camera_id,
                               "BAD_CAMERA_ID");

        return snprintf(response, size, "$ACK,%u,%u,PONG\r\n",
                        sequence, camera_id);
    }

    if (strcmp(tokens[3], "GET_STATUS") == 0) {
        if (camera_id != 0 && camera_id != 1)
            return format_nack(response, size, sequence, camera_id,
                               "BAD_CAMERA_ID");

        return snprintf(response, size,
                        "$ACK,%u,%u,GET_STATUS,UART=READY,"
                        "CAMERA_BACKEND=NOT_CONNECTED\r\n",
                        sequence, camera_id);
    }

    return format_nack(response, size, sequence, camera_id,
                       "UNKNOWN_COMMAND");
}

static int write_all(int fd, const char *data, size_t size)
{
    size_t used = 0;

    while (used < size) {
        ssize_t count = write(fd, data + used, size - used);

        if (count > 0) {
            used += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };

            if (poll(&pfd, 1, 1000) > 0)
                continue;
        }
        return -1;
    }

    return 0;
}

static int process_frame(const char *frame, int output_fd)
{
    char response[RESPONSE_MAX];
    int length = dispatch_command(frame, response, sizeof(response));

    if (length < 0 || (size_t)length >= sizeof(response)) {
        errno = EMSGSIZE;
        return -1;
    }

    if (write_all(output_fd, response, (size_t)length) < 0)
        return -1;

    return 0;
}

static int consume_bytes(struct frame_reader *reader, const char *data,
                         size_t size, int output_fd)
{
    size_t i;

    for (i = 0; i < size; ++i) {
        char byte = data[i];

        if (byte == '\r')
            continue;

        if (byte == '\n') {
            if (reader->discarding) {
                reader->discarding = 0;
                reader->used = 0;
                if (process_frame("", output_fd) < 0)
                    return -1;
                continue;
            }

            if (reader->used == 0)
                continue;

            reader->data[reader->used] = '\0';
            if (process_frame(reader->data, output_fd) < 0)
                return -1;
            reader->used = 0;
            continue;
        }

        if (reader->discarding)
            continue;

        if (reader->used + 1 >= sizeof(reader->data)) {
            reader->discarding = 1;
            reader->used = 0;
            continue;
        }

        reader->data[reader->used++] = byte;
    }

    return 0;
}

static int run_service(int input_fd, int output_fd)
{
    struct frame_reader reader = {0};
    char buffer[128];

    while (!stop_requested) {
        struct pollfd pfd = { .fd = input_fd, .events = POLLIN };
        ssize_t count;
        int ready = poll(&pfd, 1, 500);

        if (ready < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            return 1;
        }
        if (ready == 0)
            continue;

        if (pfd.revents & (POLLERR | POLLNVAL)) {
            fprintf(stderr, "UART poll error: revents=0x%x\n",
                    pfd.revents);
            return 1;
        }

        count = read(input_fd, buffer, sizeof(buffer));
        if (count > 0) {
            if (consume_bytes(&reader, buffer, (size_t)count,
                              output_fd) < 0) {
                perror("process UART frame");
                return 1;
            }
            continue;
        }

        if (count == 0 && input_fd == STDIN_FILENO)
            break;

        if (count < 0 && errno != EINTR && errno != EAGAIN &&
            errno != EWOULDBLOCK) {
            perror("read");
            return 1;
        }
    }

    return 0;
}

static int expect_response(const char *request, const char *expected)
{
    char response[RESPONSE_MAX];
    int length = dispatch_command(request, response, sizeof(response));

    if (length < 0 || (size_t)length >= sizeof(response) ||
        strcmp(response, expected) != 0) {
        fprintf(stderr, "SELF_TEST_FAIL request=%s response=%s\n",
                request, length >= 0 ? response : "<format-error>");
        return -1;
    }

    return 0;
}

static int run_self_test(void)
{
    if (expect_response("$CAM,1,255,PING",
                        "$ACK,1,255,PONG\r\n") < 0 ||
        expect_response("$CAM,2,0,GET_STATUS",
                        "$ACK,2,0,GET_STATUS,UART=READY,"
                        "CAMERA_BACKEND=NOT_CONNECTED\r\n") < 0 ||
        expect_response("$CAM,3,2,GET_STATUS",
                        "$NACK,3,2,BAD_CAMERA_ID\r\n") < 0 ||
        expect_response("$CAM,4,1,SET_FPS",
                        "$NACK,4,1,UNKNOWN_COMMAND\r\n") < 0)
        return 1;

    puts("PROTOCOL_SELF_TEST_PASS");
    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s [-d DEVICE]\n"
            "  %s --stdio\n"
            "  %s --self-test\n"
            "\nDefault: DEVICE=%s, 115200 8N1, no flow control\n",
            program, program, program, DEFAULT_DEVICE);
}

int main(int argc, char **argv)
{
    const char *device = DEFAULT_DEVICE;
    struct termios saved;
    int saved_valid = 0;
    int stdio_mode = 0;
    int fd = -1;
    int rc;

    if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
        return run_self_test();

    if (argc == 2 && strcmp(argv[1], "--stdio") == 0) {
        stdio_mode = 1;
    } else if (argc == 3 && strcmp(argv[1], "-d") == 0) {
        device = argv[2];
    } else if (argc != 1) {
        usage(argv[0]);
        return 2;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (stdio_mode)
        return run_service(STDIN_FILENO, STDOUT_FILENO);

    fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        perror(device);
        return 1;
    }

    if (ioctl(fd, TIOCEXCL) < 0) {
        perror("TIOCEXCL");
        close(fd);
        return 1;
    }

    if (configure_uart(fd, &saved) < 0) {
        close(fd);
        return 1;
    }
    saved_valid = 1;

    fprintf(stderr, "camera_uartd listening on %s (115200 8N1)\n",
            device);
    rc = run_service(fd, fd);

    if (saved_valid && tcsetattr(fd, TCSANOW, &saved) < 0)
        perror("restore termios");
    close(fd);
    return rc;
}
