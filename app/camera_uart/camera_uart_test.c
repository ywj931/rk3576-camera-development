#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef TIOCM_LOOP
#define TIOCM_LOOP 0x8000
#endif

#define DEFAULT_DEVICE "/dev/ttyS9"
#define IO_TIMEOUT_MS 1500

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signo)
{
	(void)signo;
	stop_requested = 1;
}

static long long monotonic_ms(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return -1;
	return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
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
	return 0;
}

static int write_all(int fd, const unsigned char *data, size_t size)
{
	size_t used = 0;

	while (used < size) {
		struct pollfd pfd = { .fd = fd, .events = POLLOUT };
		ssize_t count;
		int ready = poll(&pfd, 1, IO_TIMEOUT_MS);

		if (ready < 0) {
			if (errno == EINTR)
				continue;
			perror("poll write");
			return -1;
		}
		if (ready == 0) {
			fprintf(stderr, "write timeout\n");
			return -1;
		}

		count = write(fd, data + used, size - used);
		if (count > 0) {
			used += (size_t)count;
			continue;
		}
		if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
				  errno == EINTR))
			continue;
		perror("write");
		return -1;
	}

	if (tcdrain(fd) < 0) {
		perror("tcdrain");
		return -1;
	}
	return 0;
}

static int set_internal_loopback(int fd, int enable)
{
	int bits = TIOCM_LOOP;

	return ioctl(fd, enable ? TIOCMBIS : TIOCMBIC, &bits);
}

static int run_internal_loopback(int fd)
{
	static const unsigned char pattern[] =
		"RK3576_UART9_INTERNAL_LOOPBACK_115200_8N1";
	unsigned char received[sizeof(pattern) - 1] = {0};
	long long deadline;
	size_t used = 0;
	int rc = 1;

	set_internal_loopback(fd, 0);
	tcflush(fd, TCIOFLUSH);
	if (set_internal_loopback(fd, 1) < 0) {
		fprintf(stderr, "internal loopback unsupported: %s\n",
			strerror(errno));
		return 2;
	}

	if (write_all(fd, pattern, sizeof(pattern) - 1) < 0)
		goto out;

	deadline = monotonic_ms() + IO_TIMEOUT_MS;
	while (used < sizeof(received)) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		long long remaining = deadline - monotonic_ms();
		ssize_t count;
		int ready;

		if (remaining <= 0)
			break;
		ready = poll(&pfd, 1, (int)remaining);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			perror("poll read");
			goto out;
		}
		if (ready == 0)
			break;

		count = read(fd, received + used, sizeof(received) - used);
		if (count > 0)
			used += (size_t)count;
		else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
			 errno != EINTR) {
			perror("read");
			goto out;
		}
	}

	printf("TX_BYTES=%zu RX_BYTES=%zu\n", sizeof(pattern) - 1, used);
	if (used == sizeof(received) &&
	    memcmp(pattern, received, sizeof(received)) == 0) {
		puts("INTERNAL_LOOPBACK_PASS");
		rc = 0;
	} else {
		puts("INTERNAL_LOOPBACK_FAIL");
	}

out:
	if (set_internal_loopback(fd, 0) < 0)
		fprintf(stderr, "disable loopback: %s\n", strerror(errno));
	return rc;
}

static int run_send(int fd, const char *message)
{
	size_t size = strlen(message);

	tcflush(fd, TCIOFLUSH);
	if (write_all(fd, (const unsigned char *)message, size) < 0)
		return 1;
	printf("TX_BYTES=%zu\n", size);
	return 0;
}

static int run_echo(int fd)
{
	unsigned char buffer[512];
	unsigned long long total = 0;

	tcflush(fd, TCIOFLUSH);
	puts("ECHO_READY (press Ctrl+C to stop)");
	while (!stop_requested) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		ssize_t count;
		int ready = poll(&pfd, 1, 500);

		if (ready < 0) {
			if (errno == EINTR)
				continue;
			perror("poll echo");
			return 1;
		}
		if (ready == 0)
			continue;

		count = read(fd, buffer, sizeof(buffer));
		if (count > 0) {
			if (write_all(fd, buffer, (size_t)count) < 0)
				return 1;
			total += (unsigned long long)count;
			printf("ECHO_BYTES=%zd TOTAL=%llu\n", count, total);
			fflush(stdout);
		} else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
			   errno != EINTR) {
			perror("read echo");
			return 1;
		}
	}

	printf("ECHO_STOP TOTAL=%llu\n", total);
	return 0;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s [-d DEVICE] loop\n"
		"  %s [-d DEVICE] send TEXT\n"
		"  %s [-d DEVICE] echo\n"
		"\nDefault: DEVICE=%s, 115200 8N1, no flow control\n",
		program, program, program, DEFAULT_DEVICE);
}

int main(int argc, char **argv)
{
	const char *device = DEFAULT_DEVICE;
	const char *command;
	struct termios saved;
	int saved_valid = 0;
	int index = 1;
	int fd;
	int rc;

	if (argc > 3 && strcmp(argv[index], "-d") == 0) {
		device = argv[index + 1];
		index += 2;
	}
	if (index >= argc) {
		usage(argv[0]);
		return 2;
	}
	command = argv[index++];

	fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		perror(device);
		return 1;
	}
	if (configure_uart(fd, &saved) < 0) {
		close(fd);
		return 1;
	}
	saved_valid = 1;

	if (strcmp(command, "loop") == 0 && index == argc) {
		rc = run_internal_loopback(fd);
	} else if (strcmp(command, "send") == 0 && index + 1 == argc) {
		rc = run_send(fd, argv[index]);
	} else if (strcmp(command, "echo") == 0 && index == argc) {
		signal(SIGINT, handle_signal);
		signal(SIGTERM, handle_signal);
		rc = run_echo(fd);
	} else {
		usage(argv[0]);
		rc = 2;
	}

	if (saved_valid && tcsetattr(fd, TCSANOW, &saved) < 0)
		perror("restore termios");
	close(fd);
	return rc;
}
