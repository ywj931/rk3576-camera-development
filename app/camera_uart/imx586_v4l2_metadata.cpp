#include "imx586_v4l2_metadata.h"

#include <cmath>
#include <cstdint>
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
