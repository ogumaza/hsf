// SPDX-License-Identifier: MIT

// The arithmetic of JAudio2's CPU side as the Super Mario Galaxy games' `main` builds it for AArch64: the conversions
// of floats to integers, JASCalc's pow2 and clamp01, and JMath's table of sines. The build fuses a product and a sum
// into one rounding where the source writes them in one expression. The model writes those with std::fma and rounds
// every other step separately.

#pragma once

#include <cstdint>

namespace hsf::jaudio
{

// AArch64's conversions of a float to a 32-bit integer (fcvtzs and fcvtzu): toward zero, saturated at the type's
// limits, with NaN giving 0. A conversion to a narrower field keeps the low bits of the 32-bit result.
int32_t ToInt32(float value);
uint32_t ToUint32(float value);

// JASCalc::pow2: 2 to the power `x`, from a polynomial for the fraction and the exponent's bits. An exponent above
// 128 gives infinity.
float Pow2(float x);

// JASCalc::clamp01: `value` within 0 to 1. A NaN stays NaN.
float Clamp01(float value);

// JMath's table of sines: entry `index` (0 to 16383) holds the sine of index × 2π / 16384, in single precision. The
// game fills the table at start-up from the system's sine in double precision, with 2π rounded to single precision
// first. The model computes the same values without the host's library. Every host then gets the same table.
float SinTableEntry(uint32_t index);

// JMASinLap: the sine of a fraction of a turn, through the table, with the index truncated toward zero.
float SinLap(float turns);

// JMASinRadian: the sine of an angle in radians, through the table.
float SinRadian(float radians);

} // namespace hsf::jaudio
