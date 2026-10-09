// SPDX-License-Identifier: MIT

// Checks the model of nn::atk's arithmetic without game data: the pitch and volume ratios against the formulas their
// tables hold, the pan curves' ends, the low-pass cutoffs, the random numbers, the envelope's stages and the LFO's
// curves and timing.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "atk/envelope.h"
#include "atk/util.h"
#include "check.h"

namespace
{

namespace atk = hsf::atk;
namespace util = hsf::atk::util;

bool Near(double a, double b, double tolerance)
{
    return std::fabs(a - b) <= tolerance;
}

void TestPitchAndVolume()
{
    // Exact octaves, and a semitone's and a 1/256 semitone's ratios to within a float's step.
    HSF_CHECK(util::CalcPitchRatio(0) == 1.0f);
    HSF_CHECK(util::CalcPitchRatio(3072) == 2.0f);
    HSF_CHECK(util::CalcPitchRatio(-3072) == 0.5f);
    HSF_CHECK(util::CalcPitchRatio(3 * 3072) == 8.0f);
    for (int pitch = -6000; pitch <= 6000; pitch += 37)
    {
        const double expected = std::pow(2.0, pitch / 3072.0);
        Check(Near(util::CalcPitchRatio(pitch), expected, expected * 1e-6),
              "the ratio of pitch " + std::to_string(pitch));
    }

    // Unity at 0 dB, silence at -90.4 dB and below, and +6 dB the most.
    HSF_CHECK(util::CalcVolumeRatio(0.0f) == 1.0f);
    HSF_CHECK(util::CalcVolumeRatio(-90.4f) == 0.0f);
    HSF_CHECK(util::CalcVolumeRatio(-200.0f) == 0.0f);
    HSF_CHECK(util::CalcVolumeRatio(6.0f) == util::CalcVolumeRatio(40.0f));
    HSF_CHECK(Near(util::CalcVolumeRatio(-6.0f), std::pow(10.0, -6.0 / 20.0), 1e-6));
}

void TestPanAndFilter()
{
    // The square-root curve: full left and right, and the centre at sqrt(1/2), or unity when it's centred at 0 dB.
    atk::PanInfo info;
    HSF_CHECK(util::CalcPanRatio(-1.0f, info, atk::OutputMode::kStereo) == 1.0f);
    HSF_CHECK(util::CalcPanRatio(1.0f, info, atk::OutputMode::kStereo) == 0.0f);
    HSF_CHECK(Near(util::CalcPanRatio(0.0f, info, atk::OutputMode::kStereo), std::sqrt(0.5), 1e-6));
    info.center_zero_db = true;
    HSF_CHECK(util::CalcPanRatio(0.0f, info, atk::OutputMode::kStereo) == 1.0f);
    HSF_CHECK(Near(util::CalcPanRatio(-1.0f, info, atk::OutputMode::kStereo), std::sqrt(2.0), 1e-6));
    info.clamp_to_unity = true;
    HSF_CHECK(util::CalcPanRatio(-1.0f, info, atk::OutputMode::kStereo) == 1.0f);

    // The low-pass's cutoffs: the lowest below the first step, the table's cutoffs in between, and the filter off at
    // the top.
    HSF_CHECK(util::CalcLpfFreq(0.0f) == 80);
    HSF_CHECK(util::CalcLpfFreq(0.5f) > 80 && util::CalcLpfFreq(0.5f) < 16000);
    HSF_CHECK(util::CalcLpfFreq(1.0f) == 16000);
}

void TestRandom()
{
    // The generator's first steps from its seed: x = x * 0x19660D + 0x3C6EF35F.
    atk::Random random;
    uint32_t x = 0x12345678;
    for (int i = 0; i < 1000; ++i)
    {
        x = x * 0x19660Du + 0x3C6EF35Fu;
        if (random.Next() != static_cast<uint16_t>(x >> 16))
        {
            Check(false, "random number " + std::to_string(i));
            break;
        }
    }
}

void TestEnvelope()
{
    // A slow attack rises from -90.4 dB to 0 dB, holds, decays to the sustain's level, and releases to silence.
    atk::CurveAdshr envelope;
    envelope.SetAttack(100);
    envelope.SetHold(20);
    envelope.SetDecay(100);
    envelope.SetSustain(64);
    envelope.SetRelease(100);
    HSF_CHECK(envelope.GetStatus() == atk::CurveAdshr::Status::kAttack && envelope.GetValue() < -90.0f);

    int frames = 0;
    while (envelope.GetStatus() == atk::CurveAdshr::Status::kAttack && frames < 10000)
    {
        envelope.Update(5);
        ++frames;
    }
    HSF_CHECK(envelope.GetStatus() == atk::CurveAdshr::Status::kHold && envelope.GetValue() == 0.0f);
    while (envelope.GetStatus() != atk::CurveAdshr::Status::kSustain && frames < 10000)
    {
        envelope.Update(5);
        ++frames;
    }
    HSF_CHECK(envelope.GetStatus() == atk::CurveAdshr::Status::kSustain);
    HSF_CHECK(Near(envelope.GetValue(), 20.0 * std::log10((64.0 / 127.0) * (64.0 / 127.0)), 0.1));

    envelope.SetStatus(atk::CurveAdshr::Status::kRelease);
    while (envelope.GetValue() > -90.4f && frames < 20000)
    {
        envelope.Update(5);
        ++frames;
    }
    HSF_CHECK(frames < 20000);

    // An attack of 127 is immediate: the level reads 0 dB while the attack lasts.
    atk::CurveAdshr instant;
    HSF_CHECK(instant.GetStatus() == atk::CurveAdshr::Status::kAttack && instant.GetValue() == 0.0f);
}

void TestLfo()
{
    // A sine at 1 cycle a second, starting at its phase: a quarter cycle in reaches the depth times the range.
    atk::Random random;
    atk::CurveLfo lfo;
    lfo.param.depth = 0.5f;
    lfo.param.speed = 1.0f;
    lfo.param.range = 2;
    lfo.Reset();
    for (int ms = 0; ms < 250; ms += 5)
    {
        lfo.Update(5);
    }
    HSF_CHECK(Near(lfo.GetValue(random), 1.0, 1e-3));

    // No value at depth 0 or within the delay.
    atk::CurveLfo silent;
    silent.param.depth = 0.0f;
    silent.Reset();
    silent.Update(5);
    HSF_CHECK(silent.GetValue(random) == 0.0f);
    atk::CurveLfo delayed;
    delayed.param.depth = 1.0f;
    delayed.param.delay = 100;
    delayed.Reset();
    delayed.Update(50);
    HSF_CHECK(delayed.GetValue(random) == 0.0f);

    // The triangle, the saw and the square at a quarter of a cycle: 1, 0.25 and 0.
    const float kExpected[] = {1.0f, 0.25f, 0.0f};
    for (uint8_t curve = 1; curve <= 3; ++curve)
    {
        atk::CurveLfo shape;
        shape.param.depth = 1.0f;
        shape.param.speed = 1.0f;
        shape.param.curve = curve;
        shape.Reset();
        shape.Update(250);
        Check(Near(shape.GetValue(random), kExpected[curve - 1], 1e-6), "curve " + std::to_string(curve));
    }
}

} // namespace

int main()
{
    TestPitchAndVolume();
    TestPanAndFilter();
    TestRandom();
    TestEnvelope();
    TestLfo();
    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);

        return 1;
    }

    return 0;
}
