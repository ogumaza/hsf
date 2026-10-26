// SPDX-License-Identifier: MIT

// Tests the ripper's length analysis on ntdlib's length cases (ntdlib/testing/length_cases.h): sequences that each take
// one of its paths, in a little-endian archive. A counted loop that runs past the limit follows. Then come the rules
// for how long the effects ring (EffectTail), and a note sent to a delay whose rounding leaves -1 in the output for
// ever. Last, the test checks that the ripper takes a big-endian archive, and only that, for a Wii U archive.

#include "rip/rip.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/testing/length_cases.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/rip/length.h"
#include "xsflib/testing/check.h"

#include "hsf/format.h"
#include "hsf/model.h"
#include "hsf/sar.h"

using namespace ntdlib::testing;
using namespace xsflib::testing;

namespace
{

// The length that the ripper gives the case `name`, or -1.
long long LengthOf(const hsf::rip::LengthAnalyzer& analyzer, const std::string& name)
{
    for (std::size_t i = 0; i < length_cases::Cases().size(); ++i)
    {
        if (name == length_cases::Cases()[i].name)
        {
            const std::optional<hsf::rip::Timing> timing = analyzer.Analyze(static_cast<uint32_t>(i));
            return timing ? timing->length_ms : -1;
        }
    }

    return -1;
}

// A little-endian archive with one sequence: a loop of 64 passes, each C4 for 250 ms and a rest, 10 s in all. The
// passes take 640 s, past the limit of 600 s. Program 0 plays a wave of 1 s that ends, as in the length cases.
std::vector<uint8_t> CountedLoopArchive()
{
    length_cases::Code code(false);
    code.LoopStart(64).Note(60, 127, 24).Wait(936).LoopEnd().End();

    sar_files::ArchiveSpec spec;
    spec.banks = {{"BANK", 0}};
    spec.wave_archives = {{"WARC", 1}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{sar_files::Bank(false, sar_files::OneRegionBank()), "", false},
                  {sar_files::WaveArchive(false, {sar_files::Wave(false, sar_files::ConstantWave(false))}), "", false},
                  {sar_files::Sequence(false, code.Bytes(), {{"Start", 0}}), "", false}};
    sar_files::SoundSpec sound;
    sound.name = "SEQ_COUNTED_LOOP";
    sound.volume = 127;
    sound.allocate_track_flags = 1;
    sound.banks = {0};
    sound.file = 2;
    spec.sounds = {sound};

    return sar_files::Archive(false, spec);
}

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
    for (uint64_t frame = 0; frame < 4000; ++frame)
    {
        model.engine->RunFrame();
        const std::span<const int16_t> output = model.renderer.DeviceOutput();
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
}

} // namespace

int main()
{
    const hsf::rip::LengthAnalyzer analyzer(
        hsf::fsar::SoundArchive::Load(length_cases::Archive(false), hsf::kSarOptions));

    // HSF's analysis waits for the loops of the tracks other than the main track (its model's OthersLooped).
    length_cases::CheckCases([&analyzer](uint32_t index) { return analyzer.Analyze(index); }, true);

    // In HSF, SEQ_END ends at the same frame as SEQ_REST_LOOP and SEQ_LONG_REST. A different rule ends each one. A
    // model whose output runs a frame behind its engine can end SEQ_END sooner.
    const long long end = LengthOf(analyzer, "SEQ_END");
    Check(end > 0 && end == LengthOf(analyzer, "SEQ_REST_LOOP"), "SEQ_END ends at the same frame as SEQ_REST_LOOP");

    // The counted loop reaches the limit. The analysis then takes it as endless: two passes of 10 s, then the fade.
    const hsf::rip::LengthAnalyzer counted(hsf::fsar::SoundArchive::Load(CountedLoopArchive(), hsf::kSarOptions));
    const std::optional<hsf::rip::Timing> timing = counted.Analyze(0);
    Check(timing && timing->length_ms >= 19995 && timing->length_ms <= 20015 && timing->fade_ms == 10000,
          "a counted loop that runs past the limit loops after two passes, " +
              std::to_string(timing ? timing->length_ms : -1) + " ms");

    TestTailWithoutDelay();
    TestTailWithDelay();
    TestDelayRings();

    // The Wii U's archives are big-endian. The ripper refuses them, and takes the Switch's archives.
    Check(hsf::rip::IsWiiUArchive(sar_files::Archive(true, {})), "a big-endian archive is the Wii U's");
    Check(!hsf::rip::IsWiiUArchive(length_cases::Archive(false)), "a little-endian archive isn't the Wii U's");
    Check(!hsf::rip::IsWiiUArchive(std::vector<uint8_t>{'F', 'S', 'A', 'R'}), "four bytes aren't an archive");

    return Summary("rip");
}
