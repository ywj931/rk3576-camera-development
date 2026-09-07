/* pps-gprmc-sender v3: the PPS edge and the GPRMC content both derive from
 * the PTP-synchronized PHC (read via the PTP_SYS_OFFSET ioctl - the fd-based
 * clock_gettime is broken on this kernel).  On each PHC whole second: 30ms
 * break (PPS) on the UART TX, then 100ms later one GPRMC sentence carrying
 * that second's time.  SCHED_FIFO + busy-wait for a tight edge. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/ptp_clock.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int phc_fd = -1;
static int uart_fd = -1;
static volatile sig_atomic_t g_save_on_request = 0;

static void save_on_handler(int signal_number)
{
    (void)signal_number;
    g_save_on_request = 1;
}

static void quit_handler(int signal_number)
{
    (void)signal_number;
    /* Release the break before exiting: dying mid-break leaves the UART TX
       line stuck low and blocks every later writer on the port. */
    if (uart_fd >= 0)
        ioctl(uart_fd, TIOCCBRK, 0);
    _exit(0);
}

static uint64_t phc_now_ns(void)
{
    struct ptp_sys_offset so;
    memset(&so, 0, sizeof(so));
    if (ioctl(phc_fd, PTP_SYS_OFFSET, &so) < 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    }
    return (uint64_t)so.ts[0].sec * 1000000000ULL + (uint64_t)so.ts[0].nsec;
}

static void emit_break_us(long usec)
{
    ioctl(uart_fd, TIOCSBRK, 0);
    usleep(usec);
    ioctl(uart_fd, TIOCCBRK, 0);
}

static void send_gprmc(uint64_t phc_sec)
{
    time_t utc_sec = (time_t)phc_sec;
    struct tm tmv;
    char buf[128];
    int len;
    unsigned char ck = 0;
    int i;

    gmtime_r(&utc_sec, &tmv);
    len = snprintf(buf, sizeof(buf),
                   "GPRMC,%02d%02d%02d.00,A,2232.0000,N,11357.0000,E,0.0,0.0,%02d%02d%02d,,,A",
                   tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                   tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year % 100);
    for (i = 0; i < len; i++)
        ck ^= (unsigned char)buf[i];
    char out[160];
    int olen = snprintf(out, sizeof(out), "$%s*%02X\r\n", buf, ck);
    (void)!write(uart_fd, out, (size_t)olen);
    printf("EDGE phc=%llu GPRMC: $%s*%02X\n",
           (unsigned long long)phc_sec, buf, ck);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    const char *phc_dev = argc > 1 ? argv[1] : "/dev/ptp0";
    const char *uart_dev = argc > 2 ? argv[2] : "/dev/ttyS4";
    struct termios tio;
    struct sched_param sp = { .sched_priority = 90 };
    uint64_t next_edge;

    phc_fd = open(phc_dev, O_RDONLY | O_CLOEXEC);
    if (phc_fd < 0) { perror("open phc"); return 1; }
    uart_fd = open(uart_dev, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (uart_fd < 0) { perror("open uart"); return 1; }

    tcgetattr(uart_fd, &tio);
    cfmakeraw(&tio);
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    tio.c_cflag |= CLOCAL;
    tcsetattr(uart_fd, TCSANOW, &tio);

    sched_setscheduler(0, SCHED_FIFO, &sp);
    mlockall(MCL_CURRENT | MCL_FUTURE);

    {
        struct sigaction action = {};
        action.sa_handler = save_on_handler;
        sigemptyset(&action.sa_mask);
        sigaction(SIGUSR1, &action, NULL);
        action.sa_handler = quit_handler;
        sigaction(SIGTERM, &action, NULL);
        sigaction(SIGINT, &action, NULL);
    }

    next_edge = (phc_now_ns() / 1000000000ULL + 1ULL) * 1000000000ULL;
    printf("pps-gprmc-sender: phc=%s uart=%s first edge in %lld ms\n",
           phc_dev, uart_dev,
           (long long)((next_edge - phc_now_ns()) / 1000000ULL));
    fflush(stdout);

    for (;;) {
        uint64_t now = phc_now_ns();
        if (now + 2000000ULL < next_edge) {
            struct timespec ts = {
                .tv_sec = (time_t)((next_edge - now - 1500000ULL) / 1000000000ULL),
                .tv_nsec = (long)((next_edge - now - 1500000ULL) % 1000000000ULL),
            };
            nanosleep(&ts, NULL);
        }
        while ((now = phc_now_ns()) < next_edge)
            ;
        emit_break_us(30000);                 /* 30ms PPS pulse */
        {
            uint64_t edge_sec = next_edge / 1000000000ULL;
            usleep(70000);                    /* 100ms after the edge start */
            send_gprmc(edge_sec);
        }
        /* The XDAS save-on frame (AA 05 01 11 73).  Sent from THIS process
           on SIGUSR1 because the 3588's ttyS4 only transmits for the fd
           that owns the port (the new openers' writes load the TX FIFO but
           never shift out).  Emitted in the quiet window between this
           edge's GPRMC and the next edge to avoid colliding with the PPS
           stream. */
        if (g_save_on_request) {
            /* The 5-byte XDAS frame padded to 68 bytes with 0x55 filler:
               the 3588's ttyS4 only shifts out writes of roughly the GPRMC
               size (the short writes load the TX FIFO but never transmit).
               The camera's parser consumes the exact 5-byte frame and the
               filler harmlessly falls into the stray-byte sink. */
            static const uint8_t save_on_frame[68] = {
                0xaa, 0x05, 0x01, 0x11, 0x73,
            };
            for (int attempt = 0; attempt < 5; ++attempt) {
                (void)!write(uart_fd, save_on_frame, sizeof(save_on_frame));
                usleep(100000);
            }
            g_save_on_request = 0;
            printf("SAVE_ON_SENT x5\n");
            fflush(stdout);
        }
        next_edge += 1000000000ULL;
        if (phc_now_ns() > next_edge)
            next_edge = (phc_now_ns() / 1000000000ULL + 1ULL) * 1000000000ULL;
    }
    return 0;
}
