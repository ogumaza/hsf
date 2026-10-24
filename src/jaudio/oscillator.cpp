// SPDX-License-Identifier: MIT

#include "jaudio/oscillator.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include "jaudio/calc.h"

namespace hsf::jaudio
{

namespace
{

// A point's value is a fraction of 32768.
constexpr float kPointScale = 1.0f / 32768.0f;

// The four curves of 17 values each, in the order that main's ro holds them (from 0x65cad8): linear, the DSP's
// sample cells, square root and square. In the game, a curve's last step can read the first value of the next
// curve. The model reads the same values. Past the last curve, the model reads 0.
// clang-format off
constexpr std::array<uint32_t, 68> kCurveBits = {
    0x3f800000, 0x3f700000, 0x3f600000, 0x3f500000, 0x3f400000, 0x3f300000, 0x3f200000, 0x3f100000, 0x3f000000,
    0x3ee00000, 0x3ec00000, 0x3ea00000, 0x3e800000, 0x3e400000, 0x3e000000, 0x3d800000, 0x00000000,
    0x3f800000, 0x3f7871f8, 0x3f480193, 0x3f0bd912, 0x3eccb189, 0x3e942118, 0x3e5931ca, 0x3e214163, 0x3de6a1a5,
    0x3da7814e, 0x3d6d81e4, 0x3d32c16e, 0x3cfc81fc, 0x3cc2418e, 0x3c7a0225, 0x3c160129, 0x00000000,
    0x3f800000, 0x3f60fffc, 0x3f440000, 0x3f28fffc, 0x3f100000, 0x3ef1fff8, 0x3ec80000, 0x3ea1fff8, 0x3e800000,
    0x3e43ffef, 0x3e100000, 0x3dc7fff9, 0x3d800000, 0x3d0ffff3, 0x3c800000, 0x3b800000, 0x00000000,
    0x3f800000, 0x3f77def8, 0x3f6f774b, 0x3f66c15d, 0x3f5db3d0, 0x3f544391, 0x3f4a62bb, 0x3f400000, 0x3f3504f7,
    0x3f295400, 0x3f1cc469, 0x3f0f1bbd, 0x3f000000, 0x3eddb3e1, 0x3eb504e6, 0x3e800000, 0x00000000,
};
// clang-format on

// Where each curve starts in kCurveBits, by the curve's number: linear, square, square root, then the sample cells.
// main's list of the curves (0x875950) names them in this order.
constexpr std::array<uint32_t, 4> kCurveStarts = {0, 51, 34, 17};

float CurveValue(uint8_t curve, uint32_t index)
{
    // A curve number past 3 reads the list past its end in the game. The model takes the linear curve. No table of
    // the games' data names such a curve.
    const uint32_t at = (curve < kCurveStarts.size() ? kCurveStarts[curve] : 0) + index;

    return at < kCurveBits.size() ? std::bit_cast<float>(kCurveBits[at]) : 0.0f;
}

// JASLfo's step: a 72nd of the LFO's pitch for each step (0x3c638e39).
constexpr float kLfoStep = 1.0f / 72.0f;

} // namespace

OscPoint Oscillator::PointAt(std::span<const OscPoint> table, uint32_t index)
{
    if (index < table.size())
    {
        return table[index];
    }

    return {15, 0, 0};
}

void Oscillator::InitStart(const OscData* data)
{
    data_ = data;
    fields_.phase = 0.0f;
    fields_.value = 0.0f;
    fields_.start = 0.0f;
    fields_.point = 0;
    fields_.direct_release = 0;
    if (data == nullptr)
    {
        fields_.state = kStop;
        return;
    }
    if (data->table.empty())
    {
        fields_.state = kSustain;
        fields_.value = 1.0f;
        return;
    }

    const OscPoint first = PointAt(data->table, 0);
    fields_.target = static_cast<float>(first.value) * kPointScale;
    fields_.curve = static_cast<uint8_t>(first.mode);
    fields_.state = kAttack;
}

void Oscillator::IncCounter(float step)
{
    if (fields_.state == kStop || fields_.state == kSustain)
    {
        return;
    }

    fields_.phase = std::fma(step, data_->rate, fields_.phase);
    Update();
}

void Oscillator::Update()
{
    if (fields_.state == kDirectRelease)
    {
        const float time = static_cast<float>(fields_.direct_release & 0x3fff);
        if (fields_.phase < time)
        {
            UpdateCurrentValue(time);
        }
        else
        {
            fields_.value = fields_.target;
            fields_.state = kStop;
        }
        return;
    }
    if (fields_.state == kSustain)
    {
        return;
    }

    // In the game, a stopped oscillator updates along the release table too. A channel's first update does that
    // when the channel's release stopped the oscillator before the DSP channel started.
    const std::span<const OscPoint> table = fields_.state == kAttack ? data_->table : data_->release;
    if (table.empty())
    {
        fields_.state = kSustain;
        return;
    }

    // Each segment that the phase has passed ends at its target. The next point starts the next segment, holds or
    // stops, or names the point to go on at.
    float time = static_cast<float>(PointAt(table, fields_.point).time);
    while (fields_.phase >= time)
    {
        fields_.phase = fields_.phase - time;
        fields_.point = static_cast<uint16_t>(fields_.point + 1);
        fields_.value = fields_.target;
        fields_.start = fields_.target;

        const OscPoint point = PointAt(table, fields_.point);
        if (point.mode == 15)
        {
            fields_.state = kStop;
            return;
        }
        if (point.mode == 14)
        {
            fields_.state = kSustain;
            return;
        }
        if (point.mode == 13)
        {
            fields_.point = static_cast<uint16_t>(point.value);
        }
        else
        {
            fields_.curve = static_cast<uint8_t>(point.mode);
            fields_.target = static_cast<float>(point.value) * kPointScale;
        }
        time = static_cast<float>(PointAt(table, fields_.point).time);
    }

    UpdateCurrentValue(time);
}

void Oscillator::UpdateCurrentValue(float time)
{
    const float place = fields_.phase / time;
    const float scaled = place * 16.0f;
    const uint32_t index = ToUint32(scaled);
    const float fraction = scaled - static_cast<float>(index);
    const float curve =
        std::fma(CurveValue(fields_.curve, index), 1.0f - fraction, fraction * CurveValue(fields_.curve, index + 1));
    fields_.value = std::fma(fields_.start, curve, fields_.target * (1.0f - curve));
}

void Oscillator::Release()
{
    const std::span<const OscPoint> release = data_->release;
    if (release.empty() && fields_.direct_release == 0)
    {
        fields_.state = kStop;
        return;
    }

    if (fields_.direct_release != 0)
    {
        // The value falls to 0 along the curve in the release's top two bits.
        fields_.start = fields_.value;
        fields_.phase = 0.0f;
        fields_.curve = static_cast<uint8_t>(fields_.direct_release >> 14);
        fields_.target = 0.0f;
        fields_.point = 0;
        fields_.state = kDirectRelease;
        Update();
        return;
    }

    // A separate release table starts from the current value. When the release table is the attack table, the
    // release goes on along it.
    if (data_->table.data() != release.data())
    {
        const OscPoint first = PointAt(release, 0);
        fields_.start = fields_.value;
        fields_.phase = 0.0f;
        fields_.target = static_cast<float>(first.value) * kPointScale;
        fields_.point = 0;
        fields_.curve = static_cast<uint8_t>(first.mode);
    }
    fields_.state = kRelease;
    Update();
}

float Oscillator::Value() const
{
    return std::fma(fields_.value, data_->scale, data_->offset);
}

float Lfo::Value() const
{
    if (fields_.countdown != 0 || fields_.depth == 0.0f)
    {
        return 0.0f;
    }

    return fields_.depth * SinLap(fields_.phase);
}

void Lfo::IncCounter(float step)
{
    if (fields_.countdown != 0)
    {
        fields_.countdown--;
        return;
    }

    fields_.phase = std::fma(fields_.pitch * kLfoStep, step, fields_.phase);
    if (fields_.phase >= 1.0f)
    {
        fields_.phase = fields_.phase + -1.0f;
    }
}

void Lfo::ResetCounter(float free_run_phase)
{
    fields_.countdown = fields_.delay;
    if (fields_.mode == 1)
    {
        fields_.phase = 0.0f;
    }
    else if (fields_.mode == 0)
    {
        fields_.phase = free_run_phase;
    }
}

} // namespace hsf::jaudio
