// SPDX-License-Identifier: MIT

// Tests the rules for how long the effects ring after a sequence (EffectTail, hsf/model.h): without a delay, with
// a delay and with a reverb. Then the length analysis of a note sent to a delay whose rounding leaves -1 in the
// output for ever.

#include "hsf/model.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/rip/length.h"
#include "xsflib/testing/check.h"

#include "hsf/format.h"
#include "hsf/nx.h"
#include "hsf/sar.h"
#include "rip/rip.h"

using namespace ntdlib::testing;
using namespace xsflib::testing;

namespace
{

// A delay on aux bus A: `delay` µs of up to 1 s, a feedback of `feedback`, and the bus's sound at full level with no
// dry part, on 2 channels.
hsf::AuxEffect Delay(float delay, float feedback)
{
    hsf::AuxEffect effect;
    effect.type = hsf::AuxEffect::kDelay;
    effect.parameters = {1000000.0f, delay, 1.0f, feedback, 0.0f, 0.0f, 0.0f, 1.0f};

    return effect;
}

// A frame of 240 stereo pairs, each sample `value`.
std::vector<int16_t> Frame(int16_t value)
{
    return std::vector<int16_t>(480, value);
}

// Without a delay, the effects ring in a frame with any sample other than 0, however quiet, and in no other.
void TestTailWithoutDelay()
{
    hsf::AuxEffect reverb;
    reverb.parameters.assign(12, 0.0f);
    hsf::EffectTail none({});
    hsf::EffectTail reverbed({reverb});
    std::vector<int16_t> one_sample = Frame(0);
    one_sample[77] = -1;

    const bool before = none.Ringing();
    none.Frame(one_sample);
    reverbed.Frame(one_sample);
    const bool rang = none.Ringing() && reverbed.Ringing();
    none.Frame(Frame(0));
    reverbed.Frame(Frame(0));

    XSFLIB_CHECK(!before && rang);
    XSFLIB_CHECK(!none.Ringing() && !reverbed.Ringing());
}

// With a delay of 250 ms (50 frames), the effects ring for 50 frames after the last audible frame. Frames of -1 and 8,
// below the level of sound, don't ring. The 51st frame ends the ringing. A delay of 250,001 µs rings a frame longer. Of
// two delays, the longer counts. nn::atk holds a delay to its longest delay. A longer one therefore counts as the
// longest.
void TestTailWithDelay()
{
    hsf::EffectTail tail({Delay(250000.0f, 0.5f)});
    tail.Frame(Frame(9));
    bool rang = tail.Ringing();
    for (int frame = 1; frame <= 50; ++frame)
    {
        tail.Frame(Frame(frame % 2 == 0 ? int16_t{-1} : int16_t{8}));
        rang = rang && tail.Ringing();
    }
    const bool rang_through = rang;
    tail.Frame(Frame(-1));

    XSFLIB_CHECK(!hsf::EffectTail({Delay(250000.0f, 0.5f)}).Ringing());
    XSFLIB_CHECK(rang_through && !tail.Ringing());

    hsf::AuxEffect held = Delay(300000.0f, 0.5f);
    held.parameters[hsf::kDelayTimeMax] = 100000.0f;
    const std::vector<std::pair<std::vector<hsf::AuxEffect>, int>> cases = {
        {{Delay(250001.0f, 0.5f)}, 52},
        {{Delay(5000.0f, 0.5f), Delay(250000.0f, 0.5f)}, 51},
        {{held}, 21},
        {{Delay(0.0f, 0.5f)}, 1}};
    for (const auto& [effects, frames] : cases)
    {
        hsf::EffectTail other(effects);
        other.Frame(Frame(-9));
        int ringing = 0;
        for (; ringing < 100 && other.Ringing(); ++ringing)
        {
            other.Frame(Frame(0));
        }
        Check(ringing == frames,
              "a delay's frames ring for " + std::to_string(frames) + " frames, not " + std::to_string(ringing));
    }
}

// With the renderer's reverb, the effects ring for 110 frames after the last audible frame: the reverb's lines at
// their longest, 542.6 ms in whole frames, and one frame more. A frame of 8 doesn't ring. Of a reverb and a delay of
// 1 s, the delay counts.
void TestTailWithReverb()
{
    hsf::AuxEffect reverb;
    reverb.type = hsf::AuxEffect::kReverb;
    reverb.parameters = {2.0f, 0.7f, 15000.0f, 1.0f, 0.7f, 2500000.0f, 0.8f, 0.7f, 0.4f, 1.0f, 0.0f, 2.0f};
    const std::vector<std::pair<std::vector<hsf::AuxEffect>, int>> cases = {{{reverb}, 110},
                                                                            {{reverb, Delay(1000000.0f, 0.5f)}, 201}};
    for (const auto& [effects, frames] : cases)
    {
        hsf::EffectTail tail(effects);
        tail.Frame(Frame(9));
        int ringing = 0;
        for (; ringing < 300 && tail.Ringing(); ++ringing)
        {
            tail.Frame(Frame(ringing % 2 == 0 ? int16_t{8} : int16_t{-1}));
        }

        Check(ringing == frames,
              "the effects ring for " + std::to_string(frames) + " frames, not " + std::to_string(ringing));
    }
}

// A little-endian archive with one sequence: C4 for 250 ms, at full level on the main bus and on aux bus A. Program 0
// plays a square wave of 1 s that ends: 0x1000 and -0x1000 by turns, 16 samples of each.
std::vector<uint8_t> SentNoteArchive()
{
    const std::vector<uint8_t> kBytecode = {0xB0, 48, 0xE1, 120, 0x00, 0xD9, 127, 60, 127, 24, 0xFF};
    std::vector<uint8_t> samples(64000);
    for (std::size_t i = 0; i < samples.size(); i += 2)
    {
        const uint16_t value = (i / 32) % 2 == 0 ? 0x1000 : 0xF000;
        samples[i] = static_cast<uint8_t>(value);
        samples[i + 1] = static_cast<uint8_t>(value >> 8);
    }
    sar_files::WaveSpec wave;
    wave.encoding = 1;
    wave.loop_end = 32000;
    wave.channels = {samples};

    sar_files::ArchiveSpec spec;
    spec.banks = {{"BANK", 0}};
    spec.wave_archives = {{"WARC", 1}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{sar_files::Bank(false, sar_files::OneRegionBank()), "", false},
                  {sar_files::WaveArchive(false, {sar_files::Wave(false, wave)}), "", false},
                  {sar_files::Sequence(false, kBytecode, {{"Start", 0}}), "", false}};
    sar_files::SoundSpec sound;
    sound.name = "SEQ_SENT";
    sound.volume = 127;
    sound.banks = {0};
    sound.file = 2;
    spec.sounds = {sound};

    return sar_files::Archive(false, spec);
}

// A note sent to a delay of 400 ms with a feedback of 0.5. Its echoes come 400 ms apart, with silence between them. The
// delay rounds down. Its output therefore never returns to 0. The analysis still ends: at the last audible frame, and
// the 0.5 s after it. That frame comes from a run of the model for 20 s.
void TestDelayRings()
{
    hsf::ArchiveDescriptor settings;
    settings.effects = {Delay(400000.0f, 0.5f)};
    const std::vector<uint8_t> bytes = SentNoteArchive();
    const hsf::fsar::SoundArchive archive = hsf::fsar::SoundArchive::Load(bytes, hsf::kSarOptions);
    hsf::atk::ArchiveModel model(archive, hsf::EngineOptionsFor(settings));
    hsf::AppendEffects(*model.engine, settings.effects);
    model.engine->StartSequence(0);
    uint64_t last_audible = 0;
    bool heard = false;
    bool gap = false;     // an inaudible stretch between two sounds, longer than the analysis's idle frames
    bool ringing = false; // whether the last delay's frames have a sample other than 0
    uint64_t quiet_run = 0;
    uint64_t last_audible_quieter = 0; // the last frame that's audible at a volume of 0.25
    const auto audible_quieter = [](int16_t sample)
    {
        return std::abs(std::round(static_cast<double>(sample) * 0.25)) > xsflib::rip::kSilenceLevel;
    };
    for (uint64_t frame = 0; frame < 4000; ++frame)
    {
        model.engine->RunFrame();
        const std::span<const int16_t> output = model.renderer.DeviceOutput();
        if (std::any_of(output.begin(), output.end(), audible_quieter))
        {
            last_audible_quieter = frame;
        }

        if (xsflib::rip::Audible(output))
        {
            gap = gap || (heard && quiet_run > 20);
            heard = true;
            last_audible = frame;
            quiet_run = 0;
        }
        else
        {
            ++quiet_run;
        }

        const auto nonzero = [](int16_t sample)
        {
            return sample != 0;
        };
        ringing = ringing || (frame >= 3920 && std::any_of(output.begin(), output.end(), nonzero));
    }

    const hsf::rip::LengthAnalyzer analyzer(hsf::fsar::SoundArchive::Load(bytes, hsf::kSarOptions), settings);
    const std::optional<hsf::rip::Timing> timing = analyzer.Analyze(0);

    XSFLIB_CHECK(gap && ringing && last_audible > 160 && last_audible < 3000);
    const auto expected = static_cast<long long>((last_audible + 1) * 5 + 500);
    Check(timing && !timing->silent && timing->fade_ms == 0 && timing->length_ms == expected,
          "a sound sent to a delay ends 0.5 s after its last audible echo, " + std::to_string(expected) + " ms, not " +
              std::to_string(timing ? timing->length_ms : -1));

    // The player scales the output by the mini's volume tag. The analysis judges the output at that volume.
    const std::optional<hsf::rip::Timing> quieter =
        analyzer.Analyze(0, ntdlib::rip::kFullTrackVolumes, nullptr, nullptr, 0.25f);
    const auto expected_quieter = static_cast<long long>((last_audible_quieter + 1) * 5 + 500);
    XSFLIB_CHECK(last_audible_quieter > 0 && last_audible_quieter < last_audible);
    Check(quieter && !quieter->silent && quieter->length_ms == expected_quieter,
          "at a volume of 0.25, the sound ends 0.5 s after its last echo that's audible at that volume, " +
              std::to_string(expected_quieter) + " ms, not " + std::to_string(quieter ? quieter->length_ms : -1));

    const std::optional<hsf::rip::Timing> muted =
        analyzer.Analyze(0, ntdlib::rip::kFullTrackVolumes, nullptr, nullptr, 0.0f);
    Check(muted && muted->silent, "at a volume of 0, the sound is silent");
}

} // namespace

int main()
{
    TestTailWithoutDelay();
    TestTailWithDelay();
    TestTailWithReverb();
    TestDelayRings();

    return Summary("model");
}
