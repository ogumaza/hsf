// SPDX-License-Identifier: MIT

#include "atk/util.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "atk/tables.h"

namespace hsf::atk
{
namespace util
{

float Fmax(float a, float b)
{
    if (std::isnan(a))
    {
        return a;
    }
    if (std::isnan(b))
    {
        return b;
    }
    if (a == 0.0f && b == 0.0f)
    {
        return std::signbit(a) ? b : a;
    }

    return a > b ? a : b;
}

float Fminnm(float a, float b)
{
    if (std::isnan(a))
    {
        return b;
    }

    return std::min(a, b);
}

float Fmin(float a, float b)
{
    if (std::isnan(a))
    {
        return a;
    }
    if (std::isnan(b))
    {
        return b;
    }
    if (a == 0.0f && b == 0.0f)
    {
        return std::signbit(a) ? a : b;
    }

    return a < b ? a : b;
}

int32_t ToInt32(float x)
{
    if (std::isnan(x))
    {
        return 0;
    }
    if (x <= -2147483648.0f)
    {
        return INT32_MIN;
    }
    if (x >= 2147483648.0f)
    {
        return INT32_MAX;
    }

    return static_cast<int32_t>(x);
}

uint64_t ToUint64(float x)
{
    if (!(x >= 1.0f))
    {
        return 0;
    }
    if (x >= 18446744073709551616.0f)
    {
        return UINT64_MAX;
    }

    return static_cast<uint64_t>(x);
}

float CalcPitchRatio(int pitch)
{
    // Whole octaves first, then semitones and 1/256 semitones from the tables. Doubling and halving are exact.
    int octave = 0;
    if (pitch < 0)
    {
        const int down = (-1 - pitch) / 3072 + 1;
        octave = -down;
        pitch += down * 3072;
    }
    if (pitch >= 3072)
    {
        const int up = (pitch - 3072) / 3072 + 1;
        octave += up;
        pitch -= up * 3072;
    }

    const int semitone = pitch / 256;
    const int fine = pitch - semitone * 256;
    float ratio = 1.0f;
    for (int i = 0; i < octave; ++i)
    {
        ratio += ratio;
    }
    for (int i = 0; i > octave; --i)
    {
        ratio *= 0.5f;
    }
    if (semitone != 0)
    {
        ratio *= tables::kSemitone[semitone];
    }
    if (fine != 0)
    {
        ratio *= tables::kFine[fine];
    }

    return ratio;
}

float CalcVolumeRatio(float decibel)
{
    // fmax holds the level to -90.4 dB, a level above 6 dB is 6 dB, and NaN converts to the entry for 0 dB.
    const float held = Fmax(decibel, -90.4f) * 10.0f;
    const float tenths = decibel > 6.0f ? 60.0f : held;
    return tables::kDecibel[ToInt32(tenths) + 904];
}

float CalcPanRatio(float pan, const PanInfo& info, OutputMode mode)
{
    // fminnm holds the pan to 1, and a pan below -1, or NaN, takes the curve's first entry.
    const float x = (Fminnm(pan, 1.0f) + 1.0f) * 0.5f;
    const float position = pan < -1.0f || std::isnan(pan) ? 0.5f : x * 256.0f + 0.5f;

    // A sound's pan curve comes from its archive. Archives give 0 to 2, and the game doesn't check the curve.
    const bool surround = !info.front_bypass && mode == OutputMode::kSurround && info.surround_curves;
    const float* curve = surround ? tables::kSurroundPan[info.curve % 3] : tables::kStereoPan[info.curve % 3];
    float ratio = curve[ToInt32(position)];
    if (info.center_zero_db)
    {
        ratio /= curve[128];
    }

    const float limit = info.clamp_to_unity ? 1.0f : 2.0f;
    if (ratio > limit)
    {
        return limit;
    }

    return Fmax(ratio, 0.0f);
}

float CalcSurroundPanRatio(float span, const PanInfo& info)
{
    const float x = Fminnm(span, 2.0f) * 0.5f;
    const float position = span < 0.0f || std::isnan(span) ? 0.5f : x * 256.0f + 0.5f;
    const float ratio = tables::kStereoPan[info.curve % 3][ToInt32(position)];
    if (ratio > 2.0f)
    {
        return 2.0f;
    }

    return Fmax(ratio, 0.0f);
}

uint16_t CalcLpfFreq(float lpf)
{
    // The steps are a thirtieth apart from 0.13561438, and the code divides by 1/30 rather than multiplying by 30.
    constexpr float kFirstStep = 0x1.15bcfep-3f; // 0.13561438
    constexpr float kStep = 0x1.111112p-5f;      // 1/30
    const float value = lpf < 0.0f || std::isnan(lpf) ? 0.0f : Fminnm(lpf, 1.0f);
    if (value < kFirstStep)
    {
        return 80;
    }
    if (value >= 0.9f)
    {
        return 16000;
    }

    return tables::kLpfFreq[ToInt32((value - kFirstStep) / kStep)];
}

std::array<int16_t, 5> CalcLowPassBiquad(uint16_t frequency, int32_t sample_rate, bool from_table)
{
    if (from_table)
    {
        // A binary search of the cutoffs, then the row of the table for the renderer's rate.
        int index = -1;
        int low = 0;
        int high = 23;
        while (low <= high)
        {
            const int middle = (low + high) / 2;
            if (tables::kLpfFreq[middle] < frequency)
            {
                low = middle + 1;
            }
            else if (tables::kLpfFreq[middle] > frequency)
            {
                high = middle - 1;
            }
            else
            {
                index = middle;
                break;
            }
        }

        const int16_t (*table)[5] = sample_rate == 48000   ? tables::kLowPass48k
                                    : sample_rate == 32000 ? tables::kLowPass32k
                                                           : nullptr;
        if (index < 0 || table == nullptr)
        {
            return {};
        }

        const int16_t* row = table[index];
        return {row[0], row[1], row[2], row[3], row[4]};
    }

    // The exact one-pole: c = 2 - cos(2 pi f / fs), with nn::util's cosine from a turn in 32-bit fixed point, and a1 =
    // (c - sqrt(c^2 - 1)) in Q14. The code works a1 out as (sqrt(c^2 - 1) - c) times -16384.
    const int64_t turn = sample_rate == 0 ? 0 : (static_cast<int64_t>(frequency) << 32) / sample_rate;
    const uint32_t row = static_cast<uint32_t>(turn >> 24) & 0xFF;
    const float fraction = static_cast<float>(static_cast<uint32_t>(turn) & 0xFFFFFF) * 0x1p-24f;
    const float cosine = tables::kSinCos[row][0] + tables::kSinCos[row][2] * fraction;
    const float c = 2.0f - cosine;
    const float root = std::sqrt(c * c + -1.0f);
    const int32_t a1 = ToInt32((root - c) * -16384.0f);
    return {static_cast<int16_t>(0x4000 - a1), 0, 0, static_cast<int16_t>(a1), 0};
}

} // namespace util
} // namespace hsf::atk
