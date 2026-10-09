// SPDX-License-Identifier: MIT

#include "atk/envelope.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "atk/tables.h"

namespace hsf::atk
{

namespace
{

// The level in 0.1 dB of (sustain / 127)^2. Archives give 0 to 127; the game reads past the table for more.
float DecibelSquare(uint8_t sustain)
{
    return static_cast<float>(tables::kDecibelSquare[std::min<int>(sustain, 127)]);
}

// AArch64's fcvtzu to a 32-bit integer with `fbits` fraction bits: toward zero, held to the integer's range, 0 for NaN.
uint32_t ToFixedU32(float x, int fbits)
{
    const double scaled = std::ldexp(static_cast<double>(x), fbits);
    if (!(scaled >= 1.0))
    {
        return 0;
    }
    if (scaled >= 4294967296.0)
    {
        return 0xFFFFFFFF;
    }

    return static_cast<uint32_t>(scaled);
}

// nn::util's sine of a turn's fraction (CurveSine, 0xc19c10): the turn in 2^32 steps, the table's row for the top 8
// bits, and its sine plus its step to the next row times the low 24 bits, each product and sum rounded on its own.
float CurveSine(float turn)
{
    const uint32_t steps = ToFixedU32(turn, 32);
    const float fraction = static_cast<float>(steps & 0xFFFFFF) * 0x1p-24f;
    const float* row = tables::kSinCos[steps >> 24];
    return row[1] + fraction * row[3];
}

// Four straight lines through 0, 1, 0, -1 and 0 (CurveTriangle, 0xc19c50), the product and the sum rounded on their
// own. The game takes the line from the quarter without a check; a turn below 1 gives 0 to 3.
float CurveTriangle(float turn)
{
    const size_t line = std::min<size_t>(ToFixedU32(turn, 2), 4);
    return tables::kTriangleSlope[line] * turn + tables::kTriangleOffset[line];
}

// 0, then 1 from halfway (CurveSquare, 0xc19c90).
float CurveSquare(float turn)
{
    return turn < 0.5f ? 0.0f : 1.0f;
}

} // namespace

void CurveAdshr::Initialize(float decibel)
{
    SetAttack(127);
    SetHold(0);
    SetDecay(127);
    SetSustain(127);
    SetRelease(127);
    Reset(decibel);
}

void CurveAdshr::SetAttack(int attack)
{
    // Archives give 0 to 127.
    attack_ = tables::kAttack[std::clamp(attack, 0, 127)];
}

void CurveAdshr::SetHold(int hold)
{
    hold_ = static_cast<uint16_t>((hold + 1) * (hold + 1) / 4);
}

void CurveAdshr::SetDecay(int decay)
{
    decay_ = CalcRelease(decay);
}

void CurveAdshr::SetSustain(int sustain)
{
    sustain_ = static_cast<uint8_t>(sustain);
}

void CurveAdshr::SetRelease(int release)
{
    release_ = CalcRelease(release);
}

float CurveAdshr::CalcRelease(int release)
{
    if (release == 127)
    {
        return 65535.0f;
    }
    if (release == 126)
    {
        return 24.0f;
    }
    if (release < 50)
    {
        return static_cast<float>(release * 2 + 1) * 0.0078125f / 5.0f;
    }

    return 60.0f / static_cast<float>(126 - release) / 5.0f;
}

float CurveAdshr::GetValue() const
{
    if (status_ == Status::kAttack && attack_ == 0.0f)
    {
        return 0.0f;
    }

    return value_ / 10.0f;
}

void CurveAdshr::Update(int msec)
{
    switch (status_)
    {
    case Status::kAttack:
        {
            // The level is multiplied by the attack each millisecond, and the hold starts once it's within 1/32 of 0.1
            // dB of 0.
            float value = value_;
            for (int i = 0; i < msec; ++i)
            {
                value *= attack_;
                if (value > -0.03125f)
                {
                    value_ = 0.0f;
                    hold_counter_ = hold_;
                    status_ = Status::kHold;
                    return;
                }
            }
            value_ = value;
            return;
        }

    case Status::kHold:
        if (msec < hold_counter_)
        {
            hold_counter_ = static_cast<uint16_t>(hold_counter_ - msec);
            return;
        }
        msec -= hold_counter_;
        hold_counter_ = 0;
        status_ = Status::kDecay;
        [[fallthrough]];

    case Status::kDecay:
        {
            // The decay and the release fall by their rate times the milliseconds, the product and the difference
            // rounded on their own (fmul, then fsub), where the Wii U's code fuses them.
            const float sustain = DecibelSquare(sustain_);
            const float value = value_ - decay_ * static_cast<float>(msec);
            if (value < sustain)
            {
                value_ = sustain;
                status_ = Status::kSustain;
                return;
            }
            value_ = value;
            return;
        }

    case Status::kRelease:
        value_ = value_ - release_ * static_cast<float>(msec);
        return;

    default:
        return;
    }
}

void CurveLfo::Reset()
{
    delay_counter_ = 0;
    counter_ = 0.0f;
    random_value_ = 1.0f;
    started_ = false;
    wrapped_ = false;
}

void CurveLfo::Update(int msec)
{
    if (delay_counter_ < param.delay)
    {
        if (delay_counter_ + static_cast<uint32_t>(msec) <= param.delay)
        {
            delay_counter_ += static_cast<uint32_t>(msec);
            return;
        }
        msec -= static_cast<int>(param.delay - delay_counter_);
        delay_counter_ = param.delay;
    }
    if (!(param.speed > 0.0f))
    {
        return;
    }

    // The cycle starts at the phase, and advances by the speed times the seconds.
    if (!started_)
    {
        started_ = true;
        counter_ = static_cast<float>(param.phase) / 127.0f;
    }
    const float counter = counter_ + param.speed * static_cast<float>(msec) / 1000.0f;
    wrapped_ = counter >= 1.0f;
    counter_ = counter - static_cast<float>(util::ToInt32(counter));
}

float CurveLfo::GetValue(Random& random)
{
    if (param.depth == 0.0f || delay_counter_ < param.delay)
    {
        return 0.0f;
    }

    float value = 1.0f;
    switch (param.curve)
    {
    case 0:
        value = CurveSine(counter_);
        break;
    case 1:
        value = CurveTriangle(counter_);
        break;
    case 2:
        value = counter_;
        break;
    case 3:
        value = CurveSquare(counter_);
        break;
    case 4:
        // CurveRandom (0xc19cb0): a number from 0 to 1, drawn while the cycle has just wrapped.
        if (wrapped_)
        {
            random_value_ = static_cast<float>(random.Next()) / 65535.0f;
        }
        value = random_value_;
        break;
    default:
        break;
    }

    return value * param.depth * static_cast<float>(param.range);
}

} // namespace hsf::atk
