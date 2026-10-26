// SPDX-License-Identifier: MIT

#include "jac/calc.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "jaudio/calc.h"

namespace hsf::jac
{

namespace
{

using jaudio::ToInt32;

// C5BASE_PITCHTABLE (0x6b02fc in main's data): the pitch of each key from 0 to 127 against key 60.
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

// KEY_TABLE (0x5bf2d8 in main's ro): the pitch of each 64th of a semitone.
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

// The coefficients of the system's sinf (0x5aa160 in the SDK's module, with its table at 0xb698f0), by their bits:
// 2/π and π/2 for the reduction, then the cosine's polynomial from x^0 to x^8 and the sine's from x^3 to x^7. The
// system's table holds the cosine's polynomial a second time with each sign turned, for the third and fourth
// quarters.
constexpr uint64_t kInverseHalfPiBits = 0x3fe45f306dc9c883;
constexpr uint64_t kHalfPiBits = 0x3ff921fb54442d18;
constexpr std::array<uint64_t, 5> kCosineBits = {0x3ff0000000000000, 0xbfdffffffd0c621c, 0x3fa55553e1068f19,
                                                 0xbf56c087e89a359d, 0x3ef99343027bf8c3};
constexpr std::array<uint64_t, 3> kSineBits = {0xbfc555545995a603, 0x3f81107605230bc4, 0xbf2994eb3774cf24};

double Coefficient(uint64_t bits)
{
    return std::bit_cast<double>(bits);
}

// The system's polynomials in double precision, with each step rounded as the system's code rounds it.
double SinePolynomial(double x, double x2)
{
    const double x3 = x * x2;
    const double x5 = x2 * x3;
    const double low = x + x3 * Coefficient(kSineBits[0]);
    const double high = Coefficient(kSineBits[1]) + x2 * Coefficient(kSineBits[2]);

    return low + x5 * high;
}

double CosinePolynomial(double x2, bool turned)
{
    std::array<double, 5> c{};
    for (std::size_t i = 0; i < c.size(); i++)
    {
        c[i] = turned ? -Coefficient(kCosineBits[i]) : Coefficient(kCosineBits[i]);
    }
    const double x4 = x2 * x2;
    const double x6 = x2 * x4;
    const double low = (c[0] + x2 * c[1]) + x4 * c[2];
    const double high = c[3] + x2 * c[4];

    return low + x6 * high;
}

// The system's sinf for an argument of magnitude below 120: tiny arguments as they are, those below 0.75 by the sine's
// polynomial, and the rest reduced by the nearest multiple of π/2 to the polynomial of their quarter. The system
// reduces larger arguments another way. The tables take arguments from 0 to π/2 only.
float SystemSinf(float y)
{
    const uint32_t top = (std::bit_cast<uint32_t>(y) >> 20) & 0x7ff;
    const double x = y;
    if (top < 0x3f4)
    {
        if (top < 0x398)
        {
            return y;
        }

        return static_cast<float>(SinePolynomial(x, x * x));
    }

    const double r = Coefficient(kInverseHalfPiBits) * x;
    const int64_t quarter = std::llround(r);
    const double reduced = x - Coefficient(kHalfPiBits) * std::round(r);
    const double x2 = reduced * reduced;
    if ((quarter & 1) != 0)
    {
        return static_cast<float>(CosinePolynomial(x2, (quarter & 2) != 0));
    }

    const double sign = (quarter & 2) != 0 ? -1.0 : 1.0;
    return static_cast<float>(SinePolynomial(sign * reduced, x2));
}

constexpr std::size_t kTableSize = 257;

// Jac_InitSinTable's two tables, the second after the first as in the game's memory (SINTABLE at 0x7ca9c0 in main's
// bss). The first: the sines of i × π/2 / 256 in single precision. The second: the sines of (0.32612 + i × 0.34776 /
// 256) × π/2, worked out in double precision with π/2 and 0.32612 rounded to single precision first.
const std::array<float, 2 * kTableSize>& Tables()
{
    static const std::array<float, 2 * kTableSize> kTables = []
    {
        const float kHalfPi = std::bit_cast<float>(0x3fc90fdbu);
        const float kStep = std::bit_cast<float>(0x3b800000u);    // 1/256
        const double kScale = Coefficient(0x3fd641b328b6d86f);    // 0.34776
        const double kFineStep = Coefficient(0x3f70000000000000); // 1/256
        const double kBase = Coefficient(0x3fd4df2660000000);     // 0.32612 in single precision
        const double kTurn = Coefficient(0x3ff921fb60000000);     // π/2 in single precision
        std::array<float, 2 * kTableSize> tables{};
        for (std::size_t i = 0; i < kTableSize; i++)
        {
            const float index = static_cast<float>(i);
            tables[i] = SystemSinf((index * kHalfPi) * kStep);
            const double angle = ((static_cast<double>(index) * kScale) * kFineStep + kBase) * kTurn;
            tables[kTableSize + i] = SystemSinf(static_cast<float>(angle));
        }

        return tables;
    }();

    return kTables;
}

// The tables' entry at `index` from the first table's start, within the two tables.
float TableAt(int64_t index)
{
    const std::array<float, 2 * kTableSize>& tables = Tables();
    const int64_t last = static_cast<int64_t>(tables.size()) - 1;

    return tables[static_cast<std::size_t>(std::clamp<int64_t>(index, 0, last))];
}

} // namespace

int32_t Random::S32()
{
    const uint32_t high = static_cast<uint32_t>(v1_) * 0x98765432u;
    const int32_t low = static_cast<int32_t>(static_cast<uint32_t>(v0_) * 0x13579bdeu);
    const int32_t next = static_cast<int32_t>((high | 1u) + static_cast<uint32_t>(low >> 4));
    v0_ = v1_;
    v1_ = next;

    return next;
}

float Random::SF32()
{
    const int32_t r = S32();
    const float value = static_cast<float>(r & 0xffffff) / 16777215.0f;

    return r < 0 ? -value : value;
}

float Sinf3(float x)
{
    return TableAt(ToInt32(x * 256.0f));
}

float SurroundCurve(float x)
{
    return TableAt(static_cast<int64_t>(kTableSize) + ToInt32(x * 256.0f));
}

float PitchToCent(float value, float range)
{
    // The semitones, split into a whole number toward minus infinity and a fraction from 0 to 1. The game converts
    // the fraction to 64ths by fcvtzs with 6 bits of fraction. That scales without rounding. So does the product by 64.
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
    const uint32_t step = static_cast<uint32_t>(ToInt32(fraction * 64.0f)) & 0xffff;
    const float key_pitch = std::bit_cast<float>(kKeyPitchBits[static_cast<std::size_t>(std::clamp(key, 0, 127))]);

    return key_pitch * std::bit_cast<float>(kFractionPitchBits[std::min<uint32_t>(step, 63)]);
}

float KeyPitch(uint32_t key)
{
    return std::bit_cast<float>(kKeyPitchBits[std::min<uint32_t>(key, 127)]);
}

} // namespace hsf::jac
