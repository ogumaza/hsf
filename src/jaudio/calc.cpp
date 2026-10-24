// SPDX-License-Identifier: MIT

#include "jaudio/calc.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace hsf::jaudio
{

namespace
{

constexpr std::size_t kSinTableSize = 16384;

// JASCalc::pow2's polynomial for 2^x - 1 (main's constants), from the highest power down.
constexpr float kPow2Terms[6] = {1.8300806e-4f, 0.0013131053f, 0.009625022f, 0.055502914f, 0.24022661f, 0.6931472f};

// JMASinRadian's factor from radians to the table's index: 16384 / 2π in single precision (0x4522f983).
constexpr float kRadiansToIndex = 2607.594482421875f;

// The sine and cosine of `y`, for |y| up to π/4, from their Taylor series in double precision, nested from the
// highest term: 1 - y²/(n(n - 1)) × (the rest). The terms run up to the 21st power for the sine and the 22nd for the
// cosine. The series' error then lies far below a double's precision.
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

// The sine of `x`, for x from 0 to 2π, in double precision: the angle less the nearest multiple of π/2, then the
// series for that quarter. The multiple of π/2 comes off in two parts. The difference then keeps its precision.
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
        // The game's angle: the index times 2π in single precision, then divided by 16384, in double precision.
        constexpr double kTurn = static_cast<double>(6.2831853071795864769f);
        std::array<float, kSinTableSize> table{};
        for (std::size_t i = 0; i < table.size(); i++)
        {
            const double angle = static_cast<double>(i) * kTurn * (1.0 / 16384.0);
            table[i] = static_cast<float>(Sine(angle));
        }

        return table;
    }();

    return kTable;
}

} // namespace

int32_t ToInt32(float value)
{
    if (std::isnan(value))
    {
        return 0;
    }
    if (value >= 2147483648.0f)
    {
        return std::numeric_limits<int32_t>::max();
    }
    if (value <= -2147483648.0f)
    {
        return std::numeric_limits<int32_t>::min();
    }

    return static_cast<int32_t>(value);
}

uint32_t ToUint32(float value)
{
    if (std::isnan(value) || value <= 0.0f)
    {
        return 0;
    }
    if (value >= 4294967296.0f)
    {
        return std::numeric_limits<uint32_t>::max();
    }

    return static_cast<uint32_t>(value);
}

float Pow2(float x)
{
    // The exponent: x moved half a step towards 0 and truncated.
    const int32_t exponent = ToInt32((x < 0.0f ? 0.5f : -0.5f) + x);
    if (exponent > 128)
    {
        return std::numeric_limits<float>::infinity();
    }

    // The fraction lies between -1.5 and 1.5. A negative one moves up by a half, and 2^-0.5 makes up for it.
    float fraction = x - static_cast<float>(exponent);
    const bool below = fraction < 0.0f;
    fraction = fraction + (below ? 0.5f : 0.0f);
    const float scale = below ? 0.70710677f : 1.0f;

    // Horner's rule for the polynomial, each step fused.
    float sum = std::fma(fraction, kPow2Terms[0], kPow2Terms[1]);
    for (std::size_t i = 2; i < 6; i++)
    {
        sum = std::fma(fraction, sum, kPow2Terms[i]);
    }
    float result = fraction * sum;
    result = result + 0.25f;
    result = scale * result;
    result = std::fma(scale, 0.75f, result);

    const uint32_t bits = 0x3f800000u + (static_cast<uint32_t>(exponent) << 23);

    return result * std::bit_cast<float>(bits);
}

float Clamp01(float value)
{
    const float upper = value >= 1.0f ? 1.0f : value;

    return value <= 0.0f ? 0.0f : upper;
}

float SinTableEntry(uint32_t index)
{
    return SinTable()[index % kSinTableSize];
}

float SinLap(float turns)
{
    const std::array<float, kSinTableSize>& table = SinTable();
    if (turns < 0.0f)
    {
        return -table[static_cast<uint32_t>(ToInt32(turns * -16384.0f)) & 0x3fff];
    }

    return table[static_cast<uint32_t>(ToInt32(turns * 16384.0f)) & 0x3fff];
}

float SinRadian(float radians)
{
    const std::array<float, kSinTableSize>& table = SinTable();
    if (radians < 0.0f)
    {
        return -table[static_cast<uint32_t>(ToInt32(radians * -kRadiansToIndex)) & 0x3fff];
    }

    return table[static_cast<uint32_t>(ToInt32(radians * kRadiansToIndex)) & 0x3fff];
}

} // namespace hsf::jaudio
