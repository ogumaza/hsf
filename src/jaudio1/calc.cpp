// SPDX-License-Identifier: MIT

#include "jaudio1/calc.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace hsf::jaudio1
{

namespace
{

constexpr std::size_t kSinTableSize = 2048;

// JMASinRadian's factor from radians to the table's index: 2048 / 2π in single precision (0x43a2f983).
constexpr float kRadiansToIndex = 325.949310302734375f;

// JASDriver's C5BASE_PITCHTABLE (0x94abc4 in main's ro): the pitch of each key from 0 to 127 against C5.
// clang-format off
constexpr std::array<uint32_t, 128> kKeyPitchBits = {
    0x3d000000, 0x3d079c41, 0x3d0face6, 0x3d18372e, 0x3d214489, 0x3d2adb40, 0x3d3504c5, 0x3d3fc86d,
    0x3d4b2fa9, 0x3d5744f6, 0x3d6411c3, 0x3d71a198, 0x3d800000, 0x3d879c41, 0x3d8face6, 0x3d9837b5,
    0x3da1450f, 0x3daadbc6, 0x3db504c5, 0x3dbfc86d, 0x3dcb302f, 0x3dd744f6, 0x3de411c3, 0x3df1a198,
    0x3e000000, 0x3e079c84, 0x3e0face6, 0x3e1837f8, 0x3e21450f, 0x3e2adc0a, 0x3e350508, 0x3e3fc86d,
    0x3e4b2fec, 0x3e5744f6, 0x3e641206, 0x3e71a1dc, 0x3e800000, 0x3e879c84, 0x3e8face6, 0x3e9837f8,
    0x3ea1450f, 0x3eaadc0a, 0x3eb504e6, 0x3ebfc88e, 0x3ecb2fec, 0x3ed744f6, 0x3ee411e4, 0x3ef1a1ba,
    0x3f000000, 0x3f079c84, 0x3f0facd6, 0x3f1837f8, 0x3f214520, 0x3f2adc0a, 0x3f3504f7, 0x3f3fc88e,
    0x3f4b2ffd, 0x3f574507, 0x3f6411f5, 0x3f71a1cb, 0x3f800000, 0x3f879c7c, 0x3f8facd6, 0x3f9837ef,
    0x3fa14517, 0x3faadc0a, 0x3fb504f7, 0x3fbfc886, 0x3fcb2ff5, 0x3fd744fe, 0x3fe411f5, 0x3ff1a1c2,
    0x40000000, 0x40079c7c, 0x400facd6, 0x401837ef, 0x40214517, 0x402adc0a, 0x403504f7, 0x403fc88a,
    0x404b2ff9, 0x405744fe, 0x406411f5, 0x4071a1c2, 0x40800000, 0x40879c7e, 0x408facd8, 0x409837f1,
    0x40a14519, 0x40aadc0a, 0x40b504f5, 0x40bfc888, 0x40cb2ff9, 0x40d74500, 0x40e411f5, 0x40f1a1c2,
    0x41000000, 0x41079c7d, 0x410facd7, 0x411837f1, 0x41214519, 0x412adc0a, 0x413504f5, 0x413fc889,
    0x414b2ff8, 0x41574500, 0x416411f4, 0x4171a1c3, 0x41800000, 0x41879c7d, 0x418facd7, 0x419837f1,
    0x41a14519, 0x41aadc0a, 0x41b504f5, 0x41bfc889, 0x41cb2ff8, 0x41d74500, 0x41e411f4, 0x41f1a1c3,
    0x42000000, 0x42079c7d, 0x420facd7, 0x421837f1, 0x42214519, 0x422adc0a, 0x423504f5, 0x423fc889,
};
// clang-format on

// JASPlayer's s_key_table (0x94b668): the pitch of each 64th of a semitone.
// clang-format off
constexpr std::array<uint32_t, 64> kFractionPitchBits = {
    0x3f800000, 0x3f801dd2, 0x3f803bac, 0x3f80598e, 0x3f807770, 0x3f809564, 0x3f80b357, 0x3f80d152,
    0x3f80ef56, 0x3f810d5a, 0x3f812b6f, 0x3f814984, 0x3f8167a1, 0x3f8185cf, 0x3f81a3f4, 0x3f81c22b,
    0x3f81e069, 0x3f81fea8, 0x3f821cef, 0x3f823b3f, 0x3f825996, 0x3f8277f7, 0x3f82965f, 0x3f82b4c8,
    0x3f82d339, 0x3f82f1bb, 0x3f83103d, 0x3f832ebe, 0x3f834d51, 0x3f836bec, 0x3f838a87, 0x3f83a92a,
    0x3f83c7d6, 0x3f83e68a, 0x3f840547, 0x3f84240b, 0x3f8442d0, 0x3f8461a6, 0x3f84807c, 0x3f849f5a,
    0x3f84be40, 0x3f84dd2f, 0x3f84fc1e, 0x3f851b1e, 0x3f853a1d, 0x3f855925, 0x3f857836, 0x3f85974e,
    0x3f85b670, 0x3f85d599, 0x3f85f4c3, 0x3f8613fd, 0x3f863337, 0x3f86527a, 0x3f8671c5, 0x3f869119,
    0x3f86b075, 0x3f86cfd1, 0x3f86ef3d, 0x3f870eaa, 0x3f872e1f, 0x3f874d9c, 0x3f876d22, 0x3f878cb0,
};
// clang-format on

// The sine and cosine of `y`, for |y| up to π/4, from their Taylor series in double precision, nested from the
// highest term. The series' error lies far below a double's precision.
double SinSeries(double y)
{
    const double y2 = y * y;
    double sum = 1.0;
    for (int n = 21; n >= 3; n -= 2)
    {
        sum = 1.0 - y2 * sum / (static_cast<double>(n) * static_cast<double>(n - 1));
    }

    return y * sum;
}

double CosSeries(double y)
{
    const double y2 = y * y;
    double sum = 1.0;
    for (int n = 22; n >= 2; n -= 2)
    {
        sum = 1.0 - y2 * sum / (static_cast<double>(n) * static_cast<double>(n - 1));
    }

    return sum;
}

// The sine of `x`, for x from 0 to 2π, in double precision, by the quarter of the turn that holds it.
double Sine(double x)
{
    constexpr double kHalfPiHigh = 1.5707963267341256e+00; // π/2 to 33 bits
    constexpr double kHalfPiLow = 6.0771005065061922e-11;  // the rest of π/2
    const double quarters = std::floor(x / (kHalfPiHigh + kHalfPiLow) + 0.5);
    const double y = (x - quarters * kHalfPiHigh) - quarters * kHalfPiLow;
    switch (static_cast<int>(quarters) & 3)
    {
    case 0:
        return SinSeries(y);
    case 1:
        return CosSeries(y);
    case 2:
        return -SinSeries(y);
    default:
        return -CosSeries(y);
    }
}

const std::array<float, kSinTableSize>& SinTable()
{
    static const std::array<float, kSinTableSize> kTable = []
    {
        // The game's angle: the index times 2π in single precision, divided by 2048, in double precision.
        constexpr double kTurn = static_cast<double>(6.2831853071795864769f);
        std::array<float, kSinTableSize> table{};
        for (std::size_t i = 0; i < table.size(); i++)
        {
            table[i] = static_cast<float>(Sine(static_cast<double>(i) * kTurn / static_cast<double>(kSinTableSize)));
        }

        return table;
    }();

    return kTable;
}

} // namespace

float Key2Pitch(int32_t key)
{
    const int32_t clamped = key < 0 ? 0 : key > 127 ? 127 : key;

    return std::bit_cast<float>(kKeyPitchBits[static_cast<std::size_t>(clamped)]);
}

float PitchToCent(float value, float range)
{
    // The semitones, split into a whole number toward minus infinity and a fraction from 0 to 1.
    const float semitones = value * 4.0f * range;
    int32_t whole = ToInt32(semitones);
    float fraction = semitones - static_cast<float>(whole);
    if (semitones < 0.0f && fraction != 0.0f)
    {
        fraction = fraction + 1.0f;
        whole--;
    }
    if (fraction >= 1.0f)
    {
        fraction = fraction + -1.0f;
        whole++;
    }
    const int32_t key = static_cast<int16_t>(whole) + 60;
    const uint32_t step = static_cast<uint32_t>(ToInt32(fraction * 64.0f));

    return Key2Pitch(key) * std::bit_cast<float>(kFractionPitchBits[step & 63]);
}

float SinRadian(float radians)
{
    const std::array<float, kSinTableSize>& table = SinTable();
    if (radians < 0.0f)
    {
        return -table[static_cast<uint32_t>(ToInt32(radians * -kRadiansToIndex)) & 0x7ff];
    }

    return table[static_cast<uint32_t>(ToInt32(radians * kRadiansToIndex)) & 0x7ff];
}

float RandomFast::Next()
{
    value = value * 0x19660du + 0x3c6ef35fu;

    return std::bit_cast<float>(0x3f800000u | value >> 9) + -1.0f;
}

} // namespace hsf::jaudio1
