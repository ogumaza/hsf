// SPDX-License-Identifier: MIT

// The arithmetic of JAudio1's CPU side as the Switch release of Pikmin 2 builds it for AArch64: the pitch of a key
// (JASDriver::key2pitch_c5), of a fraction of semitones (JASPlayer::pitchToCent), and JMath's table of 2048 sines.
// Unlike the Galaxy games' build, this one fuses no product into a sum: every step rounds on its own.

#pragma once

#include <cstdint>

#include "jaudio/calc.h"

namespace hsf::jaudio1
{

using jaudio::ToInt32;
using jaudio::ToUint32;

// π/2 in single precision.
inline constexpr float kHalfPi = 1.57079637f;

// JMath::TRandom_fast_: x = x × 0x19660d + 0x3c6ef35f. JASInstRand, JAI (JAInter::Const::random), JALCalc
// (getRandom_0_1) and the sound code (PSSystem::oRandom) each draw from a generator of this kind. The game starts each
// generator at 0.
struct RandomFast
{
    uint32_t value = 0;

    // nextFloat_0_1: the next number from 1 to 2, from the generator's top 23 bits, minus 1.
    float Next();
};

// The pitch of `key` against C5 (60): 2 to the (key - 60) / 12, from main's table. The key is clamped to 0 to 127.
float Key2Pitch(int32_t key);

// JASPlayer::pitchToCent: the pitch of `value` × 4 × `range` semitones. The whole semitones come from Key2Pitch, and
// the fraction from a table of 64 steps of a semitone.
float PitchToCent(float value, float range);

// JMASinRadian. JMath's table of sines (TSinCosTable<2048>) holds the sine of i × 2π / 2048 at entry i. JMASinRadian
// reads it with the angle times 2048 / 2π, truncated, in 11 bits. A negative angle reads the table with the angle's
// magnitude and negates the entry.
float SinRadian(float radians);

} // namespace hsf::jaudio1
