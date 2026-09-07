#include "imx586_v4l2_metadata.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <limits>

namespace imx586_v4l2 {
namespace {

constexpr uint32_t kExposureFraction = 16;
constexpr uint32_t kBaseIso = 50;
constexpr uint32_t kAnalogGainUnity = 1024;
constexpr uint32_t kAnalogGainMaximumCode = 1008;
constexpr uint32_t kDigitalGainUnityCode = 1264;
constexpr uint32_t kGainMinimumCode = 112;
constexpr uint32_t kGainMaximumCode = 5103;

bool rounded_u32(long double value, uint32_t *output)
{
    if (!output || !std::isfinite(value) || value <= 0.0L ||
        value > static_cast<long double>(std::numeric_limits<uint32_t>::max()))
        return false;
    *output = static_cast<uint32_t>(std::llround(value));
    return *output != 0;
}

bool gain_from_code(uint32_t code, uint32_t *gain_x1000)
{
    long double gain = 0.0L;
    if (code >= kGainMinimumCode && code <= kAnalogGainMaximumCode) {
        gain = static_cast<long double>(kAnalogGainUnity) * 1000.0L /
               static_cast<long double>(kAnalogGainUnity - code);
    } else if (code >= kDigitalGainUnityCode && code <= kGainMaximumCode) {
        const uint32_t digital_code = code - kAnalogGainMaximumCode;
        gain = 64000.0L * static_cast<long double>(digital_code) / 256.0L;
    } else {
        return false;
    }
    return rounded_u32(gain, gain_x1000);
}

}  // namespace

bool read_timing_registers(const char *i2c_bus_device, uint8_t slave_addr,
                           TimingRegisters *timing)
{
    if (!timing || !i2c_bus_device || !*i2c_bus_device)
        return false;
    timing->valid = false;

    const int fd = open(i2c_bus_device, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return false;

    struct timespec stamp = {};
    clock_gettime(CLOCK_MONOTONIC, &stamp);

    const auto read16 = [&](uint8_t reg_hi, uint8_t reg_lo,
                            uint16_t *value) {
        const uint8_t address[2] = {reg_hi, reg_lo};
        uint8_t data[2] = {};
        struct i2c_msg messages[2] = {
            {slave_addr, 0, sizeof(address), const_cast<uint8_t *>(address)},
            {slave_addr, I2C_M_RD, sizeof(data), data},
        };
        struct i2c_rdwr_ioctl_data payload = {messages, 2};
        if (ioctl(fd, I2C_RDWR, &payload) < 0)
            return false;
        *value = static_cast<uint16_t>((data[0] << 8) | data[1]);
        return true;
    };

    TimingRegisters snapshot;
    uint16_t exposure = 0;
    uint16_t vts = 0;
    uint16_t hts = 0;
    uint16_t offset = 0;
    const bool ok = read16(0x02, 0x02, &exposure) &&
                    read16(0x03, 0x40, &vts) &&
                    read16(0x03, 0x42, &hts) &&
                    read16(0x3f, 0x68, &offset);
    close(fd);
    if (!ok)
        return false;

    snapshot.valid = true;
    snapshot.exposure_lines = exposure;
    snapshot.vts_lines = vts;
    snapshot.hts_pck = hts;
    snapshot.xvs_offset_lines = offset;
    snapshot.read_monotonic_ns =
        static_cast<uint64_t>(stamp.tv_sec) * 1000000000ULL +
        static_cast<uint64_t>(stamp.tv_nsec);
    *timing = snapshot;
    return true;
}

bool write_xvs_slave_registers(const char *i2c_bus_device, uint8_t slave_addr,
                               long exposure_lines, std::string *report)
{
    if (!i2c_bus_device || !*i2c_bus_device) {
        if (report)
            *report = "missing i2c bus device";
        return false;
    }
    const int fd = open(i2c_bus_device, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        if (report)
            *report = "open failed: " + std::string(std::strerror(errno));
        return false;
    }
    const auto write_regs = [&](uint16_t reg, const uint8_t *data,
                                uint8_t len) {
        uint8_t buffer[5] = {static_cast<uint8_t>(reg >> 8),
                             static_cast<uint8_t>(reg & 0xff), 0, 0, 0};
        std::memcpy(buffer + 2, data, len);
        struct i2c_msg message = {slave_addr, 0,
                                  static_cast<uint16_t>(2 + len), buffer};
        struct i2c_rdwr_ioctl_data payload = {&message, 1};
        return ioctl(fd, I2C_RDWR, &payload) < 0 ? errno : 0;
    };
    const auto write8 = [&](uint16_t reg, uint8_t value) {
        return write_regs(reg, &value, 1);
    };
    const auto write16 = [&](uint16_t reg, uint16_t value) {
        const uint8_t data[2] = {static_cast<uint8_t>(value >> 8),
                                 static_cast<uint8_t>(value & 0xff)};
        return write_regs(reg, data, 2);
    };
    const auto write24 = [&](uint16_t reg, uint32_t value) {
        const uint8_t data[3] = {static_cast<uint8_t>(value >> 16),
                                 static_cast<uint8_t>(value >> 8),
                                 static_cast<uint8_t>(value & 0xff)};
        return write_regs(reg, data, 3);
    };
    const auto read_bytes = [&](uint16_t reg, uint8_t *data, uint8_t len) {
        const uint8_t address[2] = {static_cast<uint8_t>(reg >> 8),
                                    static_cast<uint8_t>(reg & 0xff)};
        struct i2c_msg messages[2] = {
            {slave_addr, 0, sizeof(address),
             const_cast<uint8_t *>(address)},
            {slave_addr, I2C_M_RD, len, data},
        };
        struct i2c_rdwr_ioctl_data payload = {messages, 2};
        return ioctl(fd, I2C_RDWR, &payload) < 0 ? errno : 0;
    };

    long exposure = exposure_lines;
    if (exposure < 0) {
        uint8_t buffer[2] = {0, 0};
        if (read_bytes(0x0202, buffer, sizeof(buffer)) != 0) {
            if (report)
                *report = "read COARSE_INTEG_TIME failed";
            close(fd);
            return false;
        }
        exposure = (static_cast<long>(buffer[0]) << 8) | buffer[1];
    }
    const uint32_t prsh = static_cast<uint32_t>(exposure) + 34U;

    /* Same registers, same order as imx586_configure_xvs_slave() in the
       PCL kernel driver (drivers/media/i2c/imx586.c). */
    int error = write8(0x3f70, 0x01); /* MC_MODE fixed */
    if (!error)
        error = write8(0x3041, 0x00); /* slave mode */
    if (!error)
        error = write8(0x3040, 0x00); /* XVS input */
    if (!error)
        error = write8(0x3f71, 0x00); /* XVS output disable */
    if (!error)
        error = write8(0x4b85, 0x01); /* XVS low-active */
    if (!error)
        error = write8(0x0350, 0x01); /* frame length auto */
    if (!error)
        error = write16(0x3f68, 0); /* internal XVS offset lines */
    if (!error)
        error = write16(0x3f6a, 0); /* internal XVS offset VTPXCK */
    if (!error)
        error = write8(0x3f6e, 0x00); /* input XVS mult sel */
    if (!error)
        error = write8(0x3f6f, 0x00); /* input XVS mult 1:1 */
    if (!error)
        error = write24(0x3f79, prsh); /* PRSH length lines */
    if (error) {
        if (report)
            *report = "register write failed: " +
                      std::string(std::strerror(error));
        close(fd);
        return false;
    }

    uint8_t verify[4] = {0, 0, 0, 0};
    uint8_t prsh_buf[3] = {0, 0, 0};
    if (read_bytes(0x3041, verify, 1) != 0 ||
        read_bytes(0x3040, verify + 1, 1) != 0 ||
        read_bytes(0x0350, verify + 2, 1) != 0 ||
        read_bytes(0x4b85, verify + 3, 1) != 0 ||
        read_bytes(0x3f79, prsh_buf, sizeof(prsh_buf)) != 0) {
        if (report)
            *report = "readback verify failed";
        close(fd);
        return false;
    }
    const uint32_t prsh_read = (static_cast<uint32_t>(prsh_buf[0]) << 16) |
                               (static_cast<uint32_t>(prsh_buf[1]) << 8) |
                               prsh_buf[2];
    if (verify[0] != 0x00 || verify[1] != 0x00 || verify[2] != 0x01 ||
        verify[3] != 0x01 || prsh_read != prsh) {
        if (report) {
            char text[128];
            std::snprintf(text, sizeof(text),
                          "verify mismatch: slave=%u io=%u flen=%u pol=%u "
                          "prsh=%u want=%u",
                          verify[0], verify[1], verify[2], verify[3],
                          prsh_read, prsh);
            *report = text;
        }
        close(fd);
        return false;
    }
    if (report) {
        char text[96];
        std::snprintf(text, sizeof(text),
                      "slave=on low-active exposure=%ld prsh=%u", exposure,
                      prsh);
        *report = text;
    }
    close(fd);
    return true;
}

bool convert_controls(const ControlSnapshot &controls, Metadata *metadata)
{
    if (!metadata || controls.exposure_lines_x16 == 0 ||
        controls.active_width == 0 || controls.pixel_rate == 0 ||
        controls.horizontal_blanking >
            std::numeric_limits<uint32_t>::max() - controls.active_width)
        return false;

    Metadata converted;
    const uint64_t line_length =
        static_cast<uint64_t>(controls.active_width) +
        controls.horizontal_blanking;
    const long double exposure_us =
        static_cast<long double>(controls.exposure_lines_x16) *
        static_cast<long double>(line_length) * 1000000.0L /
        (static_cast<long double>(controls.pixel_rate) * kExposureFraction);
    if (!rounded_u32(exposure_us, &converted.exposure_us) ||
        !gain_from_code(controls.gain_code, &converted.gain_x1000))
        return false;

    const uint64_t iso_numerator =
        static_cast<uint64_t>(converted.gain_x1000) * kBaseIso + 500U;
    converted.iso = static_cast<uint32_t>(iso_numerator / 1000U);
    if (converted.iso == 0)
        return false;

    *metadata = converted;
    return true;
}

bool self_test()
{
    Metadata output;
    ControlSnapshot nominal;
    nominal.exposure_lines_x16 = 0x0b00;
    nominal.active_width = 4000;
    nominal.horizontal_blanking = 0x2310 - 4000;
    nominal.pixel_rate = 678400000;
    nominal.gain_code = 112;
    if (!convert_controls(nominal, &output) || output.exposure_us != 2329 ||
        output.gain_x1000 != 1123 || output.iso != 56)
        return false;

    nominal.gain_code = 1008;
    if (!convert_controls(nominal, &output) || output.gain_x1000 != 64000)
        return false;
    nominal.gain_code = 1264;
    if (!convert_controls(nominal, &output) || output.gain_x1000 != 64000)
        return false;
    nominal.gain_code = 5103;
    if (!convert_controls(nominal, &output) || output.gain_x1000 != 1023750)
        return false;
    nominal.gain_code = 1100;
    if (convert_controls(nominal, &output))
        return false;
    nominal.gain_code = 112;
    nominal.pixel_rate = 0;
    return !convert_controls(nominal, &output);
}

}  // namespace imx586_v4l2
