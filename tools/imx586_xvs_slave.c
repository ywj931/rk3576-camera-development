/*
 * imx586_xvs_slave - force IMX586 sensors into XVS slave mode from
 * userspace while the sensors are in standby (before any streaming).
 *
 * This mirrors imx586_configure_xvs_slave() in the PCL kernel driver
 * (drivers/media/i2c/imx586.c) for boards whose flashed device tree
 * lacks the "sony,xvs-slave-mode" property: the driver then never
 * programs the slave registers and the sensor free-runs.
 *
 * Register access uses the raw I2C_RDWR ioctl with an explicit slave
 * address, the same technique as imx586_v4l2_metadata.cpp, so it works
 * while the kernel driver is bound to 0x1a.
 *
 * IMPORTANT: these registers must be programmed while the sensor is in
 * standby.  Run from init before rkaiq_3A / camera_aiq_test start
 * streaming, or after stopping them.
 *
 * Usage:
 *   imx586_xvs_slave [--polarity low|high] [--thin 0|1]
 *                    [--exposure-lines N] /dev/i2c-BUS [...]
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define IMX586_ADDR 0x1a

#define REG_FRAME_LENGTH_CTRL 0x0350
#define FRAME_LENGTH_AUTO 0x01
#define REG_XVS_IO_CTRL 0x3040
#define XVS_INPUT 0x00
#define REG_MASTER_SLAVE_SEL 0x3041
#define SLAVE_MODE 0x00
#define REG_INTERNAL_XVS_OFFSET_LINE 0x3f68
#define REG_INTERNAL_XVS_OFFSET_VTPXCK 0x3f6a
#define REG_INPUT_XVS_MULT_SEL 0x3f6e
#define REG_INPUT_XVS_MULT 0x3f6f
#define REG_MC_MODE 0x3f70
#define MC_MODE_FIXED 0x01
#define REG_XVS_OUTPUT_ENABLE 0x3f71
#define XVS_OUTPUT_DISABLE 0x00
#define REG_PRSH_LENGTH_LINES 0x3f79
#define PRSH_MARGIN_LINES 34
#define REG_XVS_INPUT_POLARITY 0x4b85
#define XVS_LOW_ACTIVE 0x01
#define XVS_HIGH_ACTIVE 0x00
#define REG_EXPOSURE_H 0x0202

static int xfer(int fd, struct i2c_msg *msgs, uint32_t count)
{
    struct i2c_rdwr_ioctl_data payload;
    payload.msgs = msgs;
    payload.nmsgs = count;
    if (ioctl(fd, I2C_RDWR, &payload) < 0)
        return -errno;
    return 0;
}

static int write_regs(int fd, uint16_t reg, const uint8_t *data, uint8_t len)
{
    uint8_t buf[5];
    struct i2c_msg msg;
    buf[0] = (uint8_t)(reg >> 8);
    buf[1] = (uint8_t)(reg & 0xff);
    memcpy(buf + 2, data, len);
    msg.addr = IMX586_ADDR;
    msg.flags = 0;
    msg.len = (uint16_t)(2 + len);
    msg.buf = buf;
    return xfer(fd, &msg, 1);
}

static int write8(int fd, uint16_t reg, uint8_t value)
{
    return write_regs(fd, reg, &value, 1);
}

static int write16(int fd, uint16_t reg, uint16_t value)
{
    uint8_t data[2] = {(uint8_t)(value >> 8), (uint8_t)(value & 0xff)};
    return write_regs(fd, reg, data, 2);
}

static int write24(int fd, uint16_t reg, uint32_t value)
{
    uint8_t data[3] = {(uint8_t)(value >> 16), (uint8_t)(value >> 8),
                       (uint8_t)(value & 0xff)};
    return write_regs(fd, reg, data, 3);
}

static int read_regs(int fd, uint16_t reg, uint8_t *data, uint8_t len)
{
    uint8_t address[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xff)};
    struct i2c_msg msgs[2];
    msgs[0].addr = IMX586_ADDR;
    msgs[0].flags = 0;
    msgs[0].len = sizeof(address);
    msgs[0].buf = address;
    msgs[1].addr = IMX586_ADDR;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len = len;
    msgs[1].buf = data;
    return xfer(fd, msgs, 2);
}

static int configure_sensor(const char *device, uint8_t polarity,
                            uint8_t thin, long forced_exposure)
{
    const int fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "%s: open failed: %s\n", device, strerror(errno));
        return 1;
    }
    int rc = 1;
    long exposure = forced_exposure;
    if (exposure < 0) {
        uint8_t buffer[2] = {0, 0};
        if (read_regs(fd, REG_EXPOSURE_H, buffer, sizeof(buffer)) < 0) {
            fprintf(stderr, "%s: read exposure failed: %s\n", device,
                    strerror(errno));
            goto out;
        }
        exposure = ((long)buffer[0] << 8) | buffer[1];
    }
    const uint32_t prsh = (uint32_t)exposure + PRSH_MARGIN_LINES;

    /* Same order as imx586_configure_xvs_slave() in the kernel driver. */
    int err = write8(fd, REG_MC_MODE, MC_MODE_FIXED);
    if (!err)
        err = write8(fd, REG_MASTER_SLAVE_SEL, SLAVE_MODE);
    if (!err)
        err = write8(fd, REG_XVS_IO_CTRL, XVS_INPUT);
    if (!err)
        err = write8(fd, REG_XVS_OUTPUT_ENABLE, XVS_OUTPUT_DISABLE);
    if (!err)
        err = write8(fd, REG_XVS_INPUT_POLARITY, polarity);
    if (!err)
        err = write8(fd, REG_FRAME_LENGTH_CTRL, FRAME_LENGTH_AUTO);
    if (!err)
        err = write16(fd, REG_INTERNAL_XVS_OFFSET_LINE, 0);
    if (!err)
        err = write16(fd, REG_INTERNAL_XVS_OFFSET_VTPXCK, 0);
    if (!err)
        err = write8(fd, REG_INPUT_XVS_MULT_SEL, 0);
    if (!err)
        err = write8(fd, REG_INPUT_XVS_MULT, thin);
    if (!err)
        err = write24(fd, REG_PRSH_LENGTH_LINES, prsh);
    if (err) {
        fprintf(stderr, "%s: register write failed: %s\n", device,
                strerror(-err));
        goto out;
    }

    uint8_t verify[4] = {0, 0, 0, 0};
    uint8_t prsh_buf[3] = {0, 0, 0};
    if (read_regs(fd, REG_MASTER_SLAVE_SEL, verify, 1) < 0 ||
        read_regs(fd, REG_XVS_IO_CTRL, verify + 1, 1) < 0 ||
        read_regs(fd, REG_FRAME_LENGTH_CTRL, verify + 2, 1) < 0 ||
        read_regs(fd, REG_XVS_INPUT_POLARITY, verify + 3, 1) < 0 ||
        read_regs(fd, REG_PRSH_LENGTH_LINES, prsh_buf, sizeof(prsh_buf)) <
            0) {
        fprintf(stderr, "%s: readback verify failed: %s\n", device,
                strerror(errno));
        goto out;
    }
    const uint32_t prsh_read =
        ((uint32_t)prsh_buf[0] << 16) | ((uint32_t)prsh_buf[1] << 8) |
        prsh_buf[2];
    if (verify[0] != SLAVE_MODE || verify[1] != XVS_INPUT ||
        verify[2] != FRAME_LENGTH_AUTO || verify[3] != polarity ||
        prsh_read != prsh) {
        fprintf(stderr,
                "%s: verify mismatch: slave=%u io=%u flen=%u pol=%u prsh=%u "
                "(want %u)\n",
                device, verify[0], verify[1], verify[2], verify[3],
                prsh_read, prsh);
        goto out;
    }
    printf("%s: XVS slave enabled: %s, exposure=%ld lines, prsh=%u lines\n",
           device, polarity == XVS_LOW_ACTIVE ? "low-active" : "high-active",
           exposure, prsh);
    rc = 0;
out:
    close(fd);
    return rc;
}

int main(int argc, char **argv)
{
    uint8_t polarity = XVS_LOW_ACTIVE;
    uint8_t thin = 0;
    long forced_exposure = -1;
    int devices = 0;
    int rc = 0;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--polarity") && i + 1 < argc) {
            if (!strcmp(argv[i + 1], "low"))
                polarity = XVS_LOW_ACTIVE;
            else if (!strcmp(argv[i + 1], "high"))
                polarity = XVS_HIGH_ACTIVE;
            else {
                fprintf(stderr, "bad --polarity: %s\n", argv[i + 1]);
                return 2;
            }
            ++i;
            continue;
        }
        if (!strcmp(argv[i], "--thin") && i + 1 < argc) {
            thin = (uint8_t)strtol(argv[i + 1], NULL, 0);
            if (thin > 1) {
                fprintf(stderr, "bad --thin: %s\n", argv[i + 1]);
                return 2;
            }
            ++i;
            continue;
        }
        if (!strcmp(argv[i], "--exposure-lines") && i + 1 < argc) {
            forced_exposure = strtol(argv[i + 1], NULL, 0);
            if (forced_exposure <= 0 || forced_exposure > 0xffff) {
                fprintf(stderr, "bad --exposure-lines: %s\n", argv[i + 1]);
                return 2;
            }
            ++i;
            continue;
        }
        if (argv[i][0] == '/') {
            rc |= configure_sensor(argv[i], polarity, thin, forced_exposure);
            ++devices;
            continue;
        }
        fprintf(stderr, "unknown argument: %s\n", argv[i]);
        return 2;
    }
    if (!devices) {
        fprintf(stderr,
                "usage: %s [--polarity low|high] [--thin 0|1] "
                "[--exposure-lines N] /dev/i2c-BUS [...]\n",
                argv[0]);
        return 2;
    }
    return rc;
}
