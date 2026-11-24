// SPDX-License-Identifier: MIT

// Tests the ripper's length analysis on ntdlib's length cases (ntdlib/testing/length_cases.h): sequences that each take
// one of its paths, in a little-endian archive. The counted loops that run past the limit follow. The analysis plays a
// sound's tracks at the volumes that the game gives them, and with the variables that the game sets. Last come the
// frames until a variable takes a value, and the delay after which a sound that draws a number sounds.

#include "rip/rip.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rip/archive.h"
#include "ntdlib/testing/length_cases.h"
#include "xsflib/testing/check.h"

#include "hsf/format.h"
#include "hsf/sar.h"

using namespace ntdlib::testing;
using namespace xsflib::testing;

namespace
{

// A little-endian archive with one sequence that waits until the global variable 28 holds 5 and then until its
// variable 3 holds 7. It then plays C4 for 250 ms, like SEQ_END. Program 0 plays a wave of 1 s that ends, like the one
// in the length cases.
std::vector<uint8_t> WaitingArchive()
{
    // The waits start after the timebase and the tempo (5 bytes). Each takes 16 bytes: a compare (5), an if and a
    // jump past the wait (5), a wait of a tick (2) and a jump back (4).
    constexpr uint32_t kGlobalWait = 5;
    constexpr uint32_t kLocalWait = 21;
    constexpr uint32_t kNote = 37;
    length_cases::Code code(false);
    code.CompareEqual(28, 5).If().Jump(kLocalWait).Wait(1).Jump(kGlobalWait);
    code.CompareEqual(3, 7).If().Jump(kNote).Wait(1).Jump(kLocalWait);
    XSFLIB_CHECK(code.Bytes().size() == kNote);
    code.Note(60, 127, 24).End();

    return length_cases::SequenceArchive(false, "SEQ_WAITING", code);
}

// A little-endian archive with one sequence that waits 48 ticks, 500 ms at tempo 120, then writes 7 to the global
// variable 20 and ends.
std::vector<uint8_t> WritingArchive()
{
    length_cases::Code code(false);
    code.Wait(48).SetVariable(20, 7).End();

    return length_cases::SequenceArchive(false, "SEQ_WRITING", code);
}

// A little-endian archive with one sequence that draws a number from 0 to 9 into its variable 0 and plays C4 for 250 ms
// only when the number is `value`. Program 0 plays a wave of 1 s that ends, like the one in the length cases.
std::vector<uint8_t> DrawingArchive(uint16_t value)
{
    length_cases::Code code(false);
    code.SetRandomVariable(0, 0, 9);
    code.CompareEqual(0, value).If().Note(60, 127, 24).End();

    return length_cases::SequenceArchive(false, "SEQ_DRAWING", code);
}

// SoundingDelay finds the first later start of a sound that sounds only on some of its draws. At the first start, the
// sequence draws one of the ten numbers. Nine of the ten sequences therefore never sound. Each of them sounds when the
// game starts it after the delay, and not one frame earlier. A sound that never sounds and draws nothing gets no delay.
void TestSoundingDelay()
{
    using hsf::ScriptStep;
    constexpr uint32_t kSound = 0x01000000;
    const hsf::ScriptedStart plain = hsf::ScriptedStart::Plain(kSound);
    const auto started = [](uint32_t frame)
    {
        return hsf::ScriptedStart{kSound, frame, {ScriptStep::Start(frame, kSound)}};
    };

    int silent = 0;
    for (uint16_t value = 0; value < 10; ++value)
    {
        const hsf::rip::LengthAnalyzer analyzer(hsf::fsar::SoundArchive::Load(DrawingArchive(value), hsf::kSarOptions));
        const auto sounds = [&analyzer](const hsf::ScriptedStart& script)
        {
            const std::optional<hsf::rip::Timing> timing = analyzer.Analyze(0, ntdlib::rip::kFullTrackVolumes, &script);
            return timing && !timing->silent;
        };
        if (sounds(plain))
        {
            continue;
        }

        silent++;
        const std::optional<uint32_t> delay = analyzer.SoundingDelay(plain, ntdlib::rip::kFullTrackVolumes, 400, 2000);
        Check(delay && *delay >= 1 && sounds(started(*delay)) && (*delay == 1 || !sounds(started(*delay - 1))),
              "a draw of " + std::to_string(value) + " sounds from the start " + std::to_string(delay.value_or(0)) +
                  " frames later");
    }

    Check(silent == 9, std::to_string(silent) + " of the ten sequences never sound at the first start");

    const hsf::rip::LengthAnalyzer waiting(hsf::fsar::SoundArchive::Load(WaitingArchive(), hsf::kSarOptions));
    Check(!waiting.SoundingDelay(plain, ntdlib::rip::kFullTrackVolumes, 400, 2000),
          "a sound that never sounds and draws nothing gets no delay");
}

// FramesUntil counts the model's frames until a global variable holds a value. The sequence starts in frame 1 and
// writes the global variable 20 after 500 ms, 100 frames, give or take a frame for the ticks' places in the frames. A
// script that starts the sound 50 frames later moves the write by 50 frames. A value that never comes gives nothing,
// and so does a variable that isn't global.
void TestFramesUntil()
{
    using hsf::ScriptStep;
    constexpr uint32_t kSound = 0x01000000;
    const hsf::rip::LengthAnalyzer analyzer(hsf::fsar::SoundArchive::Load(WritingArchive(), hsf::kSarOptions));
    const std::optional<uint32_t> at_once = analyzer.FramesUntil(hsf::ScriptedStart::Plain(kSound), 20, 7, 1000);
    const std::optional<uint32_t> later =
        analyzer.FramesUntil({kSound, 0, {ScriptStep::Start(50, kSound)}}, 20, 7, 1000);
    Check(at_once && *at_once >= 100 && *at_once <= 103 && later && *later == *at_once + 50,
          "the frames until the sequence writes the variable: " + std::to_string(at_once.value_or(0)) + " and " +
              std::to_string(later.value_or(0)));
    Check(!analyzer.FramesUntil(hsf::ScriptedStart::Plain(kSound), 20, 8, 1000) &&
              !analyzer.FramesUntil(hsf::ScriptedStart::Plain(kSound), 3, 7, 1000),
          "a value that never comes, or a variable that isn't global, gives nothing");
}

// The analysis starts a sound by its script, just like the player. With the global variable 28 at 5 and the sound's
// variable 3 at 7, the waiting sequence plays its note and ends as SEQ_END does. Without either of them, it never
// sounds. A lead-in of 20 frames takes 100 ms off the length. A write of variable 3 a second in holds the note back by
// that second, give or take a tick of the sequence. A run of the script alone gives the analysis's first sample.
void TestVariables(long long end)
{
    using hsf::ScriptStep;
    constexpr uint32_t kSound = 0x01000000;
    const hsf::rip::LengthAnalyzer analyzer(hsf::fsar::SoundArchive::Load(WaitingArchive(), hsf::kSarOptions));
    const auto analyze = [&analyzer](std::vector<ScriptStep> steps, uint32_t lead_in = 0)
    {
        const hsf::ScriptedStart script = {kSound, lead_in, std::move(steps)};
        return analyzer.Analyze(0, ntdlib::rip::kFullTrackVolumes, &script);
    };

    const auto length = [](const std::optional<hsf::rip::Timing>& timing)
    {
        return std::to_string(timing ? timing->length_ms : -1) + " ms";
    };

    const ScriptStep global = ScriptStep::Write(0, 28, 5);
    const ScriptStep start = ScriptStep::Start(0, kSound);
    const ScriptStep local = ScriptStep::Write(0, 3, 7, kSound);
    const std::optional<hsf::rip::Timing> both = analyze({global, start, local});
    Check(both && !both->silent && both->length_ms == end,
          "with the variables that it waits for, the sequence ends as SEQ_END does: " + length(both));
    const std::optional<hsf::rip::Timing> no_local = analyze({global, start});
    const std::optional<hsf::rip::Timing> none = analyze({start});
    Check(no_local && no_local->silent && none && none->silent, "without them, it never sounds");

    const std::optional<hsf::rip::Timing> lead = analyze({global, start, local}, 20);
    Check(lead && !lead->silent && lead->length_ms == end - 100,
          "a lead-in of 20 frames takes 100 ms off the length: " + length(lead));
    const std::optional<hsf::rip::Timing> late = analyze({global, start, ScriptStep::Write(200, 3, 7, kSound)});
    Check(late && !late->silent && late->length_ms >= end + 1000 && late->length_ms <= end + 1011,
          "a write a second in holds the note back by a second: " + length(late));

    const auto first_sample = [&analyzer](std::vector<ScriptStep> steps, uint32_t lead_in, uint32_t limit)
    {
        const hsf::ScriptedStart script = {kSound, lead_in, std::move(steps)};
        return analyzer.FirstSampleMs(script, ntdlib::rip::kFullTrackVolumes, limit);
    };
    const std::optional<long long> late_first =
        first_sample({global, start, ScriptStep::Write(200, 3, 7, kSound)}, 0, 400);
    Check(both && both->first_sample_ms && lead && lead->first_sample_ms && late && late->first_sample_ms &&
              first_sample({global, start, local}, 0, 400) == both->first_sample_ms &&
              first_sample({global, start, local}, 20, 400) == lead->first_sample_ms &&
              late_first == late->first_sample_ms && *late_first >= 1000,
          "a run of the script gives the analysis's first sample");
    Check(
        !first_sample({global, start, ScriptStep::Write(200, 3, 7, kSound)}, 0, 150) && !first_sample({start}, 0, 400),
        "no first sample before the limit, or from a sequence that never sounds");
}

// The analysis plays the tracks at the volumes that the game gives them as it starts the sound
// (length_cases::CheckTrackVolumes).
void TestTrackVolumes()
{
    const hsf::rip::LengthAnalyzer analyzer(
        hsf::fsar::SoundArchive::Load(length_cases::TwoTracksArchive(ntdlib::sar::Flavor::kNx), hsf::kSarOptions));
    const auto analyze = [&analyzer](uint16_t muted)
    {
        return analyzer.Analyze(0, ntdlib::rip::TrackVolumesOf(muted));
    };

    length_cases::CheckTrackVolumes(analyze);
}

} // namespace

int main()
{
    const hsf::rip::LengthAnalyzer analyzer(
        hsf::fsar::SoundArchive::Load(length_cases::Archive(false), hsf::kSarOptions));
    const auto analyze = [&analyzer](uint32_t index)
    {
        return analyzer.Analyze(index);
    };

    // HSF's analysis waits for the loops of the tracks other than the main track (its model's OthersLooped).
    length_cases::CheckCases(analyze, true);

    // In HSF, SEQ_END ends at the same frame as SEQ_REST_LOOP and SEQ_LONG_REST. A different rule ends each one. A
    // model whose output runs a frame behind its engine can end SEQ_END sooner.
    const long long end = length_cases::LengthOf(analyze, "SEQ_END");
    Check(end > 0 && end == length_cases::LengthOf(analyze, "SEQ_REST_LOOP"),
          "SEQ_END ends at the same frame as SEQ_REST_LOOP");

    // The counted loops that reach the limit. The analysis then takes the outermost loop of each track as endless.
    const auto measure = [](const std::vector<uint8_t>& bytes)
    {
        return hsf::rip::LengthAnalyzer(hsf::fsar::SoundArchive::Load(bytes, hsf::kSarOptions)).Analyze(0);
    };
    length_cases::CheckLongLoops(ntdlib::sar::Flavor::kNx, measure, "the Switch");

    TestTrackVolumes();
    TestVariables(end);
    TestFramesUntil();
    TestSoundingDelay();

    return Summary("rip");
}
