// SPDX-License-Identifier: MIT

#include "game/sheet.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>

#include "xsflib/bytes.h"

namespace hsf::game
{
namespace
{

// The sound effect list's records (s_SoundEffectList, counted by s_SoundEffectListCount): 0x88 bytes, a pointer to the
// name at +0x0, the type at +0x8 (1 for an I3DL2 reverb), and an I3DL2 reverb's 12 parameters from +0x38.
constexpr std::size_t kRecordSize = 0x88;
constexpr uint32_t kI3dl2Type = 1;
constexpr std::size_t kI3dl2Parameters = 0x38;

// The parameters that the list gives in other units than nn::audio, by their indices in I3dl2Preset: the times in ms,
// and the decay ratio and the dry gain in percent.
constexpr std::size_t kDecayTime = 2;
constexpr std::size_t kHfDecayRatio = 3;
constexpr std::size_t kReflectionsDelay = 5;
constexpr std::size_t kReverbDelay = 7;
constexpr std::size_t kDryGain = 11;

// WarioWare: Move It!'s list (s_RawEffectDataList, counted by s_RawEffectDataListCount): 0xB8 bytes a record, a pointer
// to the name at +0x0, and the type at +0x8: 1 for an I3DL2 reverb, 2 for a delay and 3 for a light limiter. An I3DL2
// reverb's 12 parameters start at +0x38, a delay's 7 at +0x68 and a light limiter's 7 at +0x84. The game reads them at
// 0xce372c in its main, and divides the times by 1000 (ms to s).
constexpr std::size_t kRawRecordSize = 0xB8;
constexpr uint32_t kRawDelay = 2;
constexpr uint32_t kRawLightLimiter = 3;
constexpr std::size_t kRawDelayParameters = 0x68;
constexpr std::size_t kRawLightLimiterParameters = 0x84;
constexpr std::size_t kRawParameters = 7;

// The channel mode that the game gives its delays and light limiters: 2 channels.
constexpr float kTwoChannels = 1.0f;

// The float at `offset` in `record`.
float FloatAt(std::span<const uint8_t> record, std::size_t offset)
{
    return std::bit_cast<float>(xsflib::LoadLe32(&record[offset]));
}

// The I3DL2 reverb's 12 parameters at kI3dl2Parameters in `record`, in nn::audio's units. The record gives the times in
// ms. With `percents`, it gives the decay ratio and the dry gain in percent too.
I3dl2Preset ReadI3dl2(std::span<const uint8_t> record, bool percents)
{
    I3dl2Preset p;
    for (std::size_t k = 0; k < p.size(); ++k)
    {
        p[k] = FloatAt(record, kI3dl2Parameters + 4 * k);
    }

    p[kDecayTime] = p[kDecayTime] / 1000.0f;
    p[kReflectionsDelay] = p[kReflectionsDelay] / 1000.0f;
    p[kReverbDelay] = p[kReverbDelay] / 1000.0f;
    if (percents)
    {
        p[kHfDecayRatio] = p[kHfDecayRatio] / 100.0f;
        p[kDryGain] = p[kDryGain] / 100.0f;
    }

    return p;
}

// AArch64's fcvtzs to 64 bits: a float to an integer, toward 0. NaN gives 0. A value past the range gives its end.
int64_t ToInteger(float value)
{
    if (std::isnan(value))
    {
        return 0;
    }

    if (value >= 9.2233720368547758e18f)
    {
        return INT64_MAX;
    }

    if (value <= -9.2233720368547758e18f)
    {
        return INT64_MIN;
    }

    return static_cast<int64_t>(value);
}

// The microseconds of a time that the game gives nn::atk as whole milliseconds: fcvtzs(s * 1000) * 1,000,000 ns.
float WholeMilliseconds(float seconds)
{
    return static_cast<float>(ToInteger(seconds * 1000.0f) * 1000);
}

// The delay that a record's 7 parameters set up. The list gives the longest delay and the delay in ms, the in, feedback
// and dry gains, the low-pass amount and the channel spread. The game gives nn::atk the longest delay in whole ms, and
// the delay in whole µs (0xe9313c in its main).
std::array<float, 8> DelayParameters(std::span<const uint8_t> record)
{
    std::array<float, 8> p{};
    for (std::size_t k = 0; k < kRawParameters; ++k)
    {
        p[k] = FloatAt(record, kRawDelayParameters + 4 * k);
    }

    p[0] = WholeMilliseconds(p[0] / 1000.0f);
    p[1] = static_cast<float>(ToInteger(p[1] / 1000.0f * 1e6f));
    p[7] = kTwoChannels;

    return p;
}

// The light limiter that a record's 7 parameters set up. The list gives the longest look-ahead, the attack, the release
// and the look-ahead in ms, the threshold, and the input and output gains. The game gives nn::atk each time in whole
// ms.
std::array<float, 8> LightLimiterParameters(std::span<const uint8_t> record)
{
    std::array<float, 8> p{};
    for (std::size_t k = 0; k < kRawParameters; ++k)
    {
        p[k] = FloatAt(record, kRawLightLimiterParameters + 4 * k);
    }

    for (std::size_t k = 0; k < 4; ++k)
    {
        p[k] = WholeMilliseconds(p[k] / 1000.0f);
    }

    p[7] = kTwoChannels;

    return p;
}

} // namespace

std::map<std::string, I3dl2Preset> ReadI3dl2Presets(const Sheet& sheet)
{
    std::map<std::string, I3dl2Preset> presets;
    const std::span<const uint8_t> count_bytes = sheet.Symbol("s_SoundEffectListCount");
    const std::span<const uint8_t> records = sheet.Symbol("s_SoundEffectList");
    if (count_bytes.size() < 4)
    {
        return presets;
    }

    const uint32_t count = xsflib::LoadLe32(count_bytes.data());
    for (uint32_t i = 0; i < count && (i + 1) * kRecordSize <= records.size(); ++i)
    {
        const std::size_t base = i * kRecordSize;
        const std::optional<std::string> name = sheet.StringAt("s_SoundEffectList", base);
        if (!name || xsflib::LoadLe32(&records[base + 8]) != kI3dl2Type)
        {
            continue;
        }

        presets[*name] = ReadI3dl2(records.subspan(base, kRecordSize), true);
    }

    return presets;
}

RawEffectPresets ReadRawEffectPresets(const Sheet& sheet)
{
    RawEffectPresets presets;
    const std::span<const uint8_t> count_bytes = sheet.Symbol("s_RawEffectDataListCount");
    const std::span<const uint8_t> records = sheet.Symbol("s_RawEffectDataList");
    if (count_bytes.size() < 4)
    {
        return presets;
    }

    const uint32_t count = xsflib::LoadLe32(count_bytes.data());
    for (uint32_t i = 0; i < count && (i + 1) * kRawRecordSize <= records.size(); ++i)
    {
        const std::span<const uint8_t> record = records.subspan(i * kRawRecordSize, kRawRecordSize);
        const std::optional<std::string> name = sheet.StringAt("s_RawEffectDataList", i * kRawRecordSize);
        const uint32_t type = xsflib::LoadLe32(&record[8]);
        if (!name)
        {
            continue;
        }

        if (type == kI3dl2Type)
        {
            presets.i3dl2[*name] = ReadI3dl2(record, false);
        }
        else if (type == kRawDelay)
        {
            presets.renderer[*name] = {type, DelayParameters(record)};
        }
        else if (type == kRawLightLimiter)
        {
            presets.renderer[*name] = {type, LightLimiterParameters(record)};
        }
    }

    return presets;
}

} // namespace hsf::game
