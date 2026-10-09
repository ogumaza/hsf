// SPDX-License-Identifier: MIT

// nn::atk's arithmetic helpers (nn::atk::detail::Util). They're NintendoWare for Cafe's nw::snd helpers on AArch64.
// Each function gives the address of the routine it follows in WarioWare: Get It Together!'s main.

#pragma once

#include <array>
#include <cstdint>

namespace hsf::atk
{

// nn::atk's output modes.
enum class OutputMode : uint8_t
{
    kMono = 0,
    kStereo = 1,
    kSurround = 2,
};

// How a pan becomes a gain (Util::PanInfo).
struct PanInfo
{
    uint32_t curve = 0;           // 0 the square root, 1 the sine and cosine, 2 linear
    bool center_zero_db = false;  // scaled so that the centre is unity
    bool clamp_to_unity = false;  // held to 1 rather than 2
    bool front_bypass = false;    // the stereo curves, even for surround output
    bool surround_curves = false; // the surround curves for surround output where the front doesn't bypass them
};

namespace util
{

// AArch64's conversion of a float to a 32-bit integer (fcvtzs): toward zero, held to the integer's range, and 0 for
// NaN.
int32_t ToInt32(float x);

// AArch64's conversion of a float to an unsigned 64-bit integer (fcvtzu): toward zero, held to the integer's range, and
// 0 for NaN and what's below 1.
uint64_t ToUint64(float x);

// AArch64's fmax: a NaN gives NaN, and +0 wins over -0.
float Fmax(float a, float b);

// AArch64's fminnm against a number `b`: a NaN `a` gives `b`.
float Fminnm(float a, float b);

// AArch64's fmin: a NaN gives NaN, and -0 wins over +0.
float Fmin(float a, float b);

// The pitch ratio for a pitch in 1/256 semitones (Util::CalcPitchRatio, 0xc1a580).
float CalcPitchRatio(int pitch);

// The gain for a level in dB, held to -90.4 and +6.0 (Util::CalcVolumeRatio, 0xc1a6e0).
float CalcVolumeRatio(float decibel);

// The gain for a pan from -1 (left) to 1 (right) (Util::CalcPanRatio, 0xc1a480).
float CalcPanRatio(float pan, const PanInfo& info, OutputMode mode);

// The gain for a surround pan from 0 (front) to 2 (rear) (Util::CalcSurroundPanRatio, 0xc1a520).
float CalcSurroundPanRatio(float span, const PanInfo& info);

// The cutoff in Hz for a low-pass value from 0 to 1 (Util::CalcLpfFreq, 0xc1a2a0).
uint16_t CalcLpfFreq(float lpf);

// The low-pass at a cutoff that CalcLpfFreq gives, as a one-pole biquad (b0, b1, b2, a1 and a2 in Q14), for a renderer
// at `sample_rate` (Util::CalcLowPassBiquad, 0xc1a310). From the tables the rate must be 32 or 48 kHz, and the cutoff
// one of the tables' cutoffs; anything else gives zeros, where the game stops. Otherwise it's the exact one-pole for
// the rate.
std::array<int16_t, 5> CalcLowPassBiquad(uint16_t frequency, int32_t sample_rate, bool from_table);

} // namespace util

// nn::atk's random numbers (Util::CalcRandom, 0xc1a720): x = x * 0x19660D + 0x3C6EF35F, giving x's top 16 bits, from
// 0x12345678.
class Random
{
public:
    uint16_t Next()
    {
        x_ = x_ * 0x19660Du + 0x3C6EF35Fu;

        return static_cast<uint16_t>(x_ >> 16);
    }

    uint32_t State() const
    {
        return x_;
    }

private:
    uint32_t x_ = 0x12345678;
};

} // namespace hsf::atk
