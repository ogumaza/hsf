// SPDX-License-Identifier: MIT

// Tests the ripper's length analysis on ntdlib's length cases (ntdlib/testing/length_cases.h): sequences that each take
// one of its paths, in a little-endian archive. A counted loop that runs past the limit follows. The analysis plays a
// sound's tracks at the volumes that the game gives them, and with the variables that the game sets. The frames until a
// variable takes a value and the delay that makes a sound that draws sound come next. Last, the test checks that the
// ripper takes a big-endian archive, and only that, for a Wii U archive.

#include "rip/rip.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rip/archive.h"
#include "ntdlib/testing/length_cases.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/testing/check.h"

#include "hsf/format.h"
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

// A little-endian archive with one sequence sound `name` of `code`, on the tracks in the mask `tracks`. Program 0 plays
// a wave of 1 s that ends, as in the length cases.
std::vector<uint8_t> OneSequenceArchive(const std::string& name, const std::vector<uint8_t>& code, uint32_t tracks)
{
    sar_files::ArchiveSpec spec;
    spec.banks = {{"BANK", 0}};
    spec.wave_archives = {{"WARC", 1}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{sar_files::Bank(false, sar_files::OneRegionBank()), "", false},
                  {sar_files::WaveArchive(false, {sar_files::Wave(false, sar_files::ConstantWave(false))}), "", false},
                  {sar_files::Sequence(false, code, {{"Start", 0}}), "", false}};
    sar_files::SoundSpec sound;
    sound.name = name;
    sound.volume = 127;
    sound.allocate_track_flags = tracks;
    sound.banks = {0};
    sound.file = 2;
    spec.sounds = {sound};

    return sar_files::Archive(false, spec);
}

// A little-endian archive with one sequence: a loop of 64 passes, each C4 for 250 ms and a rest, 10 s in all. The
// passes take 640 s, past the limit of 600 s. Program 0 plays a wave of 1 s that ends, as in the length cases.
std::vector<uint8_t> CountedLoopArchive()
{
    length_cases::Code code(false);
    code.LoopStart(64).Note(60, 127, 24).Wait(936).LoopEnd().End();

    return OneSequenceArchive("SEQ_COUNTED_LOOP", code.Bytes(), 1);
}

// A little-endian archive with one sequence of two tracks. Track 0 plays C4 for 250 ms at the start. Track 1 waits 5 s,
// then plays C4 for 250 ms too. Program 0 plays a wave of 1 s that ends, as in the length cases.
std::vector<uint8_t> TwoTrackArchive()
{
    // Track 1 starts after the timebase and the tempo (5 bytes), and track 0's open (5), note (3) and end (1).
    constexpr uint32_t kTrack1 = 14;
    length_cases::Code code(false);
    code.OpenTrack(1, kTrack1).Note(60, 127, 24).End();
    XSFLIB_CHECK(code.Bytes().size() == kTrack1);
    code.Wait(480).Note(60, 127, 24).End();

    return OneSequenceArchive("SEQ_TWO_TRACKS", code.Bytes(), 3);
}

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

    return OneSequenceArchive("SEQ_WAITING", code.Bytes(), 1);
}

// A little-endian archive with one sequence that waits 48 ticks, 500 ms at tempo 120, then writes 7 to the global
// variable 20 and ends.
std::vector<uint8_t> WritingArchive()
{
    length_cases::Code code(false);
    code.Wait(48).SetVariable(20, 7).End();

    return OneSequenceArchive("SEQ_WRITING", code.Bytes(), 1);
}

// A little-endian archive with one sequence that draws a number from 0 to 9 into its variable 0 and plays C4 for 250 ms
// only when the number is `value`. Program 0 plays a wave of 1 s that ends, like the one in the length cases.
std::vector<uint8_t> DrawingArchive(uint16_t value)
{
    length_cases::Code code(false);
    code.SetRandomVariable(0, 0, 9);
    code.CompareEqual(0, value).If().Note(60, 127, 24).End();

    return OneSequenceArchive("SEQ_DRAWING", code.Bytes(), 1);
}

// SoundingDelay finds the first later start of a sound that sounds only on some of its draws. At the first start, the
// sequence draws one of the ten numbers, so nine of the ten sequences never sound. Each of them sounds when the game
// starts it after the delay, and not one frame earlier. A sound that never sounds and draws nothing gets no delay.
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
// that second, give or take a tick of the sequence.
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
}

// The analysis plays the tracks at the volumes that the game gives them as it starts the sound. Track 1's late note
// sets the length: 5 s more than SEQ_END's. With track 1 at 0, the sequence ends after track 0's note, as SEQ_END does.
// With track 0 at 0, the length stays. With both at 0, the sequence never sounds.
void TestTrackVolumes()
{
    const hsf::rip::LengthAnalyzer analyzer(hsf::fsar::SoundArchive::Load(TwoTrackArchive(), hsf::kSarOptions));
    const auto length = [&analyzer](uint16_t muted)
    {
        const std::optional<hsf::rip::Timing> timing = analyzer.Analyze(0, ntdlib::rip::TrackVolumesOf(muted));
        return timing && !timing->silent ? timing->length_ms : -1;
    };

    const long long full = length(0);
    Check(full >= 5740 && full <= 5780, "two tracks with a note 5 s apart play for " + std::to_string(full) + " ms");
    Check(length(2) >= 740 && length(2) <= 780, "with track 1 at 0, the sequence ends after track 0's note");
    Check(length(1) == full, "with track 0 at 0, the length stays");
    const std::optional<hsf::rip::Timing> muted = analyzer.Analyze(0, ntdlib::rip::TrackVolumesOf(3));
    Check(muted && muted->silent, "with both tracks at 0, the sequence never sounds");
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

    TestTrackVolumes();
    TestVariables(end);
    TestFramesUntil();
    TestSoundingDelay();

    return Summary("rip");
}
