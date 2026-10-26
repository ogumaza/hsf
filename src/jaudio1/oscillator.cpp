// SPDX-License-Identifier: MIT

#include "jaudio1/oscillator.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "jaudio1/calc.h"

namespace hsf::jaudio1
{

namespace
{

// A point's value is a fraction of 32768.
constexpr float kPointScale = 1.0f / 32768.0f;

// The curves of a segment's modes 1 to 3, 17 values each, from main's ro (the list at 0xb63320): mode 1 the square
// root, mode 2 the square, mode 3 the curve of the DSP's sample cells.
// clang-format off
constexpr std::array<std::array<uint32_t, 17>, 3> kCurveBits = {{
    {0x3f800000, 0x3f77def8, 0x3f6f774b, 0x3f66c15d, 0x3f5db3d0, 0x3f544391, 0x3f4a62bb, 0x3f400000, 0x3f3504f7,
     0x3f295400, 0x3f1cc469, 0x3f0f1bbd, 0x3f000000, 0x3eddb3e1, 0x3eb504e6, 0x3e800000, 0x00000000},
    {0x3f800000, 0x3f60fffc, 0x3f440000, 0x3f28fffc, 0x3f100000, 0x3ef1fff8, 0x3ec80000, 0x3ea1fff8, 0x3e800000,
     0x3e43ffef, 0x3e100000, 0x3dc7fff9, 0x3d800000, 0x3d0ffff3, 0x3c800000, 0x3b800000, 0x00000000},
    {0x3f800000, 0x3f7871f8, 0x3f480193, 0x3f0bd912, 0x3eccb189, 0x3e942118, 0x3e5931ca, 0x3e214163, 0x3de6a1a5,
     0x3da7814e, 0x3d6d81e4, 0x3d32c16e, 0x3cfc81fc, 0x3cc2418e, 0x3c7a0225, 0x3c160129, 0x00000000},
}};
// clang-format on

// The table of a forced stop (JASOscillator's oscTableForceStop): a stop at once.
constexpr std::array<OscPoint, 2> kForceStopTable = {{{0, 15, 0}, {15, 0, 0}}};

float Curve(uint32_t mode, uint32_t index)
{
    return std::bit_cast<float>(kCurveBits[mode - 1][index]);
}

} // namespace

float Oscillator::TimeScale(float dac_rate)
{
    return dac_rate / 80.0f / 600.0f;
}

void Oscillator::Init()
{
    data_ = nullptr;
    fields_ = Fields{};
}

void Oscillator::InitStart(const OscData* data, float dac_rate)
{
    if (data == nullptr)
    {
        fields_.state = kStop;
        return;
    }

    fields_.state = kStart;
    data_ = data;
    fields_.direct_release = 0;
    if (data->attack.Null())
    {
        fields_.phase = 0.0f;
        return;
    }

    fields_.point = 0;
    fields_.countdown = 0.0f;
    fields_.target = 0.0f;
    fields_.countdown = fields_.countdown - data->rate;
    IncCounter(dac_rate);
}

void Oscillator::IncCounter(float dac_rate)
{
    OscTable table;
    switch (fields_.state)
    {
    case kStop:
    case kHold:
        return;

    case kRelease:
        table = data_->release;
        break;

    case kForceStop:
        // The forced stop's table steps by 1 whatever the rate.
        fields_.countdown = fields_.countdown + -1.0f;
        Calc({kForceStopTable.data(), kForceStopTable.size()}, dac_rate);
        return;

    default:
        table = data_->attack;
        break;
    }

    // Without a table, the value stays at the full scale. A direct release needs none.
    if (table.Null() && fields_.state != kDirectRelease)
    {
        fields_.phase = 1.0f;
        return;
    }

    fields_.countdown = fields_.countdown - data_->rate;
    Calc(table, dac_rate);
}

float Oscillator::Value() const
{
    if (fields_.state == kStop)
    {
        return 1.0f;
    }

    return fields_.phase * data_->width + data_->vertex;
}

bool Oscillator::Cycling() const
{
    if (data_ == nullptr || fields_.state != kStart)
    {
        return false;
    }

    // A point past the table's data reads as a stop. The point counts the next point to read.
    const OscTable& table = data_->attack;
    for (std::size_t i = 0; i < table.size; i++)
    {
        const OscPoint point = table.At(i);
        if (point.mode == 13)
        {
            const uint16_t to = static_cast<uint16_t>(point.value);
            return to < i && fields_.point > to;
        }

        if (point.mode == 14 || point.mode == 15)
        {
            return false;
        }
    }

    return false;
}

bool Oscillator::Release(float dac_rate)
{
    if (fields_.state == kForceStop)
    {
        return false;
    }

    // A release table of its own starts from the value where the note is.
    if (!(data_->attack == data_->release))
    {
        fields_.point = 0;
        fields_.countdown = 0.0f;
        fields_.target = fields_.phase;
    }
    if (data_->release.Null() && fields_.direct_release == 0)
    {
        fields_.direct_release = 16;
    }
    if (fields_.direct_release == 0)
    {
        fields_.state = kRelease;
        return true;
    }

    // A direct release: its curve in the top two bits, and its time in 600ths of a second in the low 14.
    fields_.state = kDirectRelease;
    fields_.curve = static_cast<uint8_t>(fields_.direct_release >> 14);
    const float time = static_cast<float>(fields_.direct_release & 0x3fff);
    fields_.target = 0.0f;
    const float countdown = std::fmax(TimeScale(dac_rate) * time, 1.0f);
    const float step = 0.0f - fields_.phase;
    fields_.countdown = countdown;
    fields_.length = countdown;
    fields_.rate = fields_.curve != 0 ? step : step / countdown;

    return true;
}

float Oscillator::Calc(OscTable table, float dac_rate)
{
    // The next segments, while the current one's time has run out. A point of mode 13 jumps to the point that its
    // value names, 14 holds the value, and 15 stops. A point of no time moves the target at once.
    while (!(fields_.countdown > 0.0f))
    {
        fields_.phase = fields_.target;
        if (fields_.state == kDirectRelease)
        {
            fields_.state = kStop;
            break;
        }

        const OscPoint point = table.At(fields_.point);
        const uint16_t mode = static_cast<uint16_t>(point.mode);
        if (mode == 13)
        {
            fields_.point = static_cast<uint16_t>(point.value);
            continue;
        }
        if (mode == 14)
        {
            fields_.state = kHold;
            return fields_.target * data_->width + data_->vertex;
        }
        if (mode == 15)
        {
            fields_.state = kStop;
            break;
        }

        fields_.curve = static_cast<uint8_t>(mode);
        if (point.time == 0)
        {
            fields_.target = static_cast<float>(point.value) * kPointScale;
            fields_.point++;
            continue;
        }

        const float scale = TimeScale(dac_rate);
        fields_.target = static_cast<float>(point.value) * kPointScale;
        fields_.countdown = scale * static_cast<float>(point.time);
        fields_.length = fields_.countdown;
        const float step = fields_.target - fields_.phase;
        fields_.rate = fields_.curve != 0 ? step : step / fields_.countdown;
        fields_.point++;
    }

    if (data_->width == 0.0f)
    {
        return data_->vertex;
    }

    float phase = 0.0f;
    if (fields_.length == 0.0f)
    {
        phase = fields_.target;
    }
    else if (fields_.curve == 0 || fields_.rate == 0.0f || fields_.curve > 3)
    {
        phase = fields_.target - fields_.rate * fields_.countdown;
    }
    else
    {
        // A curve: its value at the segment's place, interpolated between the table's 17 steps. The curve runs the
        // other way for a falling segment.
        const float done = fields_.countdown / fields_.length;
        const float place = fields_.rate < 0.0f ? 1.0f - done : done;
        const uint32_t whole = ToUint32(place * 16.0f);
        uint32_t index = whole;
        float fraction = place * 16.0f - static_cast<float>(whole);
        if (whole > 15)
        {
            index = 15;
            fraction = 1.0f;
        }
        const float low = Curve(fields_.curve, index);
        const float high = Curve(fields_.curve, index + 1);
        const float along = std::fabs(fields_.rate * (low + (high - low) * fraction));
        phase = fields_.rate < 0.0f ? fields_.target + along : fields_.target + -(fields_.rate - along);
    }
    fields_.phase = phase;

    return phase * data_->width + data_->vertex;
}

void Vibrate::Init()
{
    pitch = 1.0f / 18.0f;
    depth = 0.0f;
    phase = 0.0f;
}

void Vibrate::IncCounter()
{
    phase = phase + pitch;
    if (static_cast<double>(phase) >= 4.0)
    {
        phase = phase - 4.0f;
    }
}

float Vibrate::Value() const
{
    if (depth == 0.0f)
    {
        return 1.0f;
    }

    return PitchToCent(depth * SinRadian(phase * kHalfPi), 12.0f);
}

} // namespace hsf::jaudio1
