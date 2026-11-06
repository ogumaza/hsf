// SPDX-License-Identifier: MIT

// Tests the ripper's length analysis on ntdlib's length cases (ntdlib/testing/length_cases.h): sequences that each take
// one of its paths, in a little-endian archive. A counted loop that runs past the limit follows. Then come the rules
// for how long the effects ring (EffectTail), and a note sent to a delay whose rounding leaves -1 in the output for
// ever. The analysis plays a sound's tracks at the volumes that the game gives them, and with the variables that the
// game sets. The jobs of a sound's states follow the archive's rules (rip/sound_states.h), and so do those of a piece
// that waits for its variables (rip/variable_states.h). A rhythm piece's script, the frames until a variable takes a
// value and the delay that makes a sound that draws sound come next. Then come the descriptors of the JAudio games'
// variants (JobDescriptor) and the tracks of Pikmin 2's layers (LayerTracks). Last, the test checks that the ripper
// takes a big-endian archive, and only that, for a Wii U archive.

#include "rip/rip.h"

#include <algorithm>
#include <cmath>
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
#include "rip/jaudio1_rip.h"
#include "rip/jaudio_rip.h"
#include "rip/sound_states.h"
#include "rip/variable_states.h"

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

    sar_files::ArchiveSpec spec;
    spec.banks = {{"BANK", 0}};
    spec.wave_archives = {{"WARC", 1}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{sar_files::Bank(false, sar_files::OneRegionBank()), "", false},
                  {sar_files::WaveArchive(false, {sar_files::Wave(false, sar_files::ConstantWave(false))}), "", false},
                  {sar_files::Sequence(false, code.Bytes(), {{"Start", 0}}), "", false}};
    sar_files::SoundSpec sound;
    sound.name = "SEQ_TWO_TRACKS";
    sound.volume = 127;
    sound.allocate_track_flags = 3;
    sound.banks = {0};
    sound.file = 2;
    spec.sounds = {sound};

    return sar_files::Archive(false, spec);
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

    sar_files::ArchiveSpec spec;
    spec.banks = {{"BANK", 0}};
    spec.wave_archives = {{"WARC", 1}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{sar_files::Bank(false, sar_files::OneRegionBank()), "", false},
                  {sar_files::WaveArchive(false, {sar_files::Wave(false, sar_files::ConstantWave(false))}), "", false},
                  {sar_files::Sequence(false, code.Bytes(), {{"Start", 0}}), "", false}};
    sar_files::SoundSpec sound;
    sound.name = "SEQ_WAITING";
    sound.volume = 127;
    sound.allocate_track_flags = 1;
    sound.banks = {0};
    sound.file = 2;
    spec.sounds = {sound};

    return sar_files::Archive(false, spec);
}

// A little-endian archive with one sequence that waits 48 ticks, 500 ms at tempo 120, then writes 7 to the global
// variable 20 and ends.
std::vector<uint8_t> WritingArchive()
{
    length_cases::Code code(false);
    code.Wait(48).SetVariable(20, 7).End();

    sar_files::ArchiveSpec spec;
    spec.banks = {{"BANK", 0}};
    spec.wave_archives = {{"WARC", 1}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{sar_files::Bank(false, sar_files::OneRegionBank()), "", false},
                  {sar_files::WaveArchive(false, {sar_files::Wave(false, sar_files::ConstantWave(false))}), "", false},
                  {sar_files::Sequence(false, code.Bytes(), {{"Start", 0}}), "", false}};
    sar_files::SoundSpec sound;
    sound.name = "SEQ_WRITING";
    sound.volume = 127;
    sound.allocate_track_flags = 1;
    sound.banks = {0};
    sound.file = 2;
    spec.sounds = {sound};

    return sar_files::Archive(false, spec);
}

// A little-endian archive with one sequence that draws a number from 0 to 9 into its variable 0 and plays C4 for 250 ms
// only when the number is `value`. Program 0 plays a wave of 1 s that ends, like the one in the length cases.
std::vector<uint8_t> DrawingArchive(uint16_t value)
{
    length_cases::Code code(false);
    code.SetRandomVariable(0, 0, 9);
    code.CompareEqual(0, value).If().Note(60, 127, 24).End();

    sar_files::ArchiveSpec spec;
    spec.banks = {{"BANK", 0}};
    spec.wave_archives = {{"WARC", 1}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{sar_files::Bank(false, sar_files::OneRegionBank()), "", false},
                  {sar_files::WaveArchive(false, {sar_files::Wave(false, sar_files::ConstantWave(false))}), "", false},
                  {sar_files::Sequence(false, code.Bytes(), {{"Start", 0}}), "", false}};
    sar_files::SoundSpec sound;
    sound.name = "SEQ_DRAWING";
    sound.volume = 127;
    sound.allocate_track_flags = 1;
    sound.banks = {0};
    sound.file = 2;
    spec.sounds = {sound};

    return sar_files::Archive(false, spec);
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
        const std::optional<hsf::rip::Timing> timing = analyzer.Analyze(0, hsf::rip::TrackVolumesOf(muted));
        return timing && !timing->silent ? timing->length_ms : -1;
    };

    const long long full = length(0);
    Check(full >= 5740 && full <= 5780, "two tracks with a note 5 s apart play for " + std::to_string(full) + " ms");
    Check(length(2) >= 740 && length(2) <= 780, "with track 1 at 0, the sequence ends after track 0's note");
    Check(length(1) == full, "with track 0 at 0, the length stays");
    const std::optional<hsf::rip::Timing> muted = analyzer.Analyze(0, hsf::rip::TrackVolumesOf(3));
    Check(muted && muted->silent, "with both tracks at 0, the sequence never sounds");
}

// A layered piece's job takes its first state's volumes. The jobs of its later states follow it, with the state's name
// after the piece's label. A sound of bus_b_later starts with bus B empty, and plays later with each reverb there. The
// other jobs stay as they are. An archive that no rule names has no rules. WithBusBReverb puts a reverb on bus B in
// place of any effect there, and keeps the effects in the order of their buses. A preset of the renderer's reverb
// becomes an effect of type 4.
void TestSoundStates()
{
    ntdlib::sar::ArchiveRules rules;
    rules.layered = {{"SEQ_B", {{"", 0x0006}, {"later", 0x0001}, {"all", 0}}}};
    rules.bus_b_presets = {
        {"I3DL2_Small",
         {-100.0f, -200.0f, 1.0f, 0.5f, -300.0f, 0.01f, -400.0f, 0.02f, 100.0f, 100.0f, 5000.0f, 0.25f}}};
    rules.bus_b = {{"SEQ_B", "I3DL2_Small"}};
    rules.bus_b_later = {"SEQ_C"};
    const std::vector<ntdlib::rip::Job> selected = {{0, "SEQ_A", std::nullopt, 1.0f, ntdlib::rip::kFullTrackVolumes},
                                                    {1, "SEQ_B", std::nullopt, 1.0f, ntdlib::rip::kFullTrackVolumes},
                                                    {2, "SEQ_B2", std::nullopt, 1.0f, ntdlib::rip::kFullTrackVolumes},
                                                    {3, "SEQ_C", std::nullopt, 1.0f, ntdlib::rip::kFullTrackVolumes}};
    const std::vector<ntdlib::rip::Job> jobs = hsf::rip::WithStates(rules, selected);
    const ntdlib::sar::ReverbPreset* small = &rules.bus_b_presets[0];

    ntdlib::rip::TrackVolumes start = ntdlib::rip::kFullTrackVolumes;
    start[1] = 0.0f;
    start[2] = 0.0f;
    ntdlib::rip::TrackVolumes later = ntdlib::rip::kFullTrackVolumes;
    later[0] = 0.0f;
    XSFLIB_CHECK(hsf::rip::TrackVolumesOf(0x0006) == start &&
                 hsf::rip::TrackVolumesOf(0) == ntdlib::rip::kFullTrackVolumes);
    Check(jobs.size() == 7 && jobs[0].label == "SEQ_A" && jobs[0].track_volumes == ntdlib::rip::kFullTrackVolumes &&
              !jobs[0].bus_b_reverb && jobs[1].index == 1 && jobs[1].label == "SEQ_B" &&
              jobs[1].track_volumes == start && jobs[1].bus_b_reverb == small && jobs[2].index == 1 &&
              jobs[2].label == "SEQ_B later" && jobs[2].track_volumes == later && jobs[2].bus_b_reverb == small &&
              jobs[3].index == 1 && jobs[3].label == "SEQ_B all" &&
              jobs[3].track_volumes == ntdlib::rip::kFullTrackVolumes && jobs[4].label == "SEQ_B2" &&
              !jobs[4].bus_b_reverb,
          "a layered piece's states follow it with its reverb, and the other jobs stay as they are");
    Check(jobs[5].index == 3 && jobs[5].label == "SEQ_C" && !jobs[5].bus_b_reverb && jobs[6].index == 3 &&
              jobs[6].label == "SEQ_C Small reverb" && jobs[6].bus_b_reverb == small &&
              jobs[6].track_volumes == ntdlib::rip::kFullTrackVolumes,
          "a sound of bus_b_later starts with bus B empty, and plays later with each reverb");
    Check(hsf::rip::WithStates({}, selected).size() == selected.size(), "with no rules, the jobs stay as they are");
    Check(ntdlib::sar::RulesOf(length_cases::Archive(false)).Empty(), "an archive that no rule names has no rules");

    hsf::ArchiveDescriptor settings;
    hsf::AuxEffect a;
    a.bus = 0;
    a.type = hsf::AuxEffect::kI3dl2Reverb;
    a.parameters.assign(12, 0.0f);
    hsf::AuxEffect b = a;
    b.bus = 1;
    b.type = hsf::AuxEffect::kDelay;
    b.parameters.assign(8, 0.0f);
    hsf::AuxEffect c = a;
    c.bus = 2;
    settings.effects = {c, b, a};
    const hsf::ArchiveDescriptor with = hsf::rip::WithBusBReverb(settings, small);
    Check(with.effects.size() == 3 && with.effects[0].bus == 0 && with.effects[1].bus == 1 &&
              with.effects[1].type == hsf::AuxEffect::kI3dl2Reverb && with.effects[1].parameters.size() == 12 &&
              with.effects[1].parameters[11] == 0.25f && with.effects[2].bus == 2,
          "the reverb takes bus B's place, in the order of the buses");
    const ntdlib::sar::ReverbPreset room = {
        "EFFECT_ROOM",
        {2.0f, 0.4f, 40000.0f, 4.0f, 0.5f, 2000000.0f, 0.7f, 0.1f, 0.6f, 1.0f, 0.25f, 2.0f},
        ntdlib::sar::ReverbType::kReverb};
    const hsf::ArchiveDescriptor renderer = hsf::rip::WithBusBReverb(settings, &room);
    Check(renderer.effects.size() == 3 && renderer.effects[1].bus == 1 &&
              renderer.effects[1].type == hsf::AuxEffect::kReverb &&
              std::equal(room.parameters.begin(), room.parameters.end(), renderer.effects[1].parameters.begin(),
                         renderer.effects[1].parameters.end()) &&
              room.DryGain() == 0.25f && !renderer.Check(),
          "a preset of the renderer's reverb becomes a reverb of type 4, with its dry gain in its eleventh value");
    const hsf::ArchiveDescriptor without = hsf::rip::WithBusBReverb(settings, nullptr);
    Check(
        without.effects.size() == 3 && without.effects[0].bus == 2 && without.effects[1].type == hsf::AuxEffect::kDelay,
        "with no reverb, the settings stay as they are");
}

// A waiting piece's job takes its first state's variables. The jobs of its later states follow it, with the state's
// name after the piece's label. The other jobs stay as they are, and keep their track volumes. An archive that no table
// holds has no waiting pieces.
void TestVariableStates()
{
    const std::vector<hsf::rip::WaitingPiece> pieces = {
        {"SEQ_B", {{"", {{28, 5}, {27, 120}}}, {"fast", {{27, 180}}}}, "", ""}};
    ntdlib::rip::TrackVolumes half = ntdlib::rip::kFullTrackVolumes;
    half[0] = 0.5f;
    const std::vector<ntdlib::rip::Job> selected = {
        {0, "SEQ_A", std::nullopt, 1.0f, half, {}},
        {1, "SEQ_B", std::nullopt, 1.0f, ntdlib::rip::kFullTrackVolumes, {}}};
    const std::vector<ntdlib::rip::Job> jobs = hsf::rip::WithVariableStates(pieces, selected);

    const xsflib::psf::Variables start = {{27, 120}, {28, 5}};
    const xsflib::psf::Variables fast = {{27, 180}};
    Check(jobs.size() == 3 && jobs[0].label == "SEQ_A" && jobs[0].track_volumes == half && jobs[0].variables.empty() &&
              jobs[1].index == 1 && jobs[1].label == "SEQ_B" && jobs[1].variables == start && jobs[2].index == 1 &&
              jobs[2].label == "SEQ_B fast" && jobs[2].variables == fast,
          "a waiting piece's states follow it, and the other jobs stay as they are");
    Check(hsf::rip::WaitingPieces(length_cases::Archive(false)).empty(),
          "an archive that no table holds has no waiting pieces");

    // A job's label finds its piece, with or without a state's name after it.
    Check(hsf::rip::WaitingPieceOf(pieces, "SEQ_B") == &pieces[0] &&
              hsf::rip::WaitingPieceOf(pieces, "SEQ_B fast") == &pieces[0] &&
              !hsf::rip::WaitingPieceOf(pieces, "SEQ_BB") && !hsf::rip::WaitingPieceOf(pieces, "SEQ_A"),
          "a job's label finds its waiting piece");
}

// A rhythm piece's script starts the controller with the tempo at frame 0. A first piece (number 1, 200, 300 or 400)
// starts at frame 2. A later piece's script writes the first piece's 1 to variable 29 at frame 2, starts the piece at
// frame 4 and writes 1 to variable 26 at frame 8. The lead-in is left at 0.
void TestRhythmScript()
{
    using hsf::ScriptStep;
    constexpr uint32_t kPiece = 0x01000007;
    constexpr uint32_t kController = 0x01000002;
    const hsf::ScriptedStart later = hsf::rip::RhythmScript(kPiece, kController, {{27, 180}, {28, 18014}});
    const hsf::ScriptedStart first = hsf::rip::RhythmScript(kPiece, kController, {{27, 120}, {28, 200}});
    const std::vector<ScriptStep> later_steps = {ScriptStep::Write(0, 27, 180), ScriptStep::Start(0, kController),
                                                 ScriptStep::Write(2, 29, 1), ScriptStep::Start(4, kPiece),
                                                 ScriptStep::Write(8, 26, 1)};
    const std::vector<ScriptStep> first_steps = {ScriptStep::Write(0, 27, 120), ScriptStep::Start(0, kController),
                                                 ScriptStep::Start(0, kPiece)};
    Check(later.sound == kPiece && later.lead_in == 0 && later.steps == later_steps,
          "a later piece's script hands over as the piece before it does");
    Check(first.sound == kPiece && first.lead_in == 0 && first.steps == first_steps,
          "a first piece's script starts the piece after the controller");
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

// A variant's descriptor is the library's, with the variant's track states for its sound, or with its muted tracks for
// its piece. Nothing else changes.
void TestJobDescriptor()
{
    hsf::ArchiveDescriptor library;
    library.jaudio.track_states.push_back({0x10005, {1, 1, 0}});
    library.jaudio.library2.pieces.resize(2);
    library.jaudio.library2.pieces[1].muted_tracks = 0x0ff0;

    hsf::rip::JaudioJob job;
    job.id = 0x10005;
    Check(hsf::rip::JobDescriptor(library, job).jaudio == library.jaudio,
          "a sound's job keeps the library's descriptor");

    job.track_states = hsf::JaudioTrackStates{0x10005, {1, 1, 1}};
    hsf::ArchiveDescriptor variant = hsf::rip::JobDescriptor(library, job);
    Check(variant.jaudio.track_states.size() == 1 && variant.jaudio.track_states[0] == *job.track_states,
          "a variant's track states replace those of its sound");

    job.track_states = hsf::JaudioTrackStates{0x10006, {2}};
    variant = hsf::rip::JobDescriptor(library, job);
    Check(variant.jaudio.track_states.size() == 2 && variant.jaudio.track_states[1] == *job.track_states,
          "a variant of a sound without track states adds them");

    job = {};
    job.id = hsf::JaudioLibrary2::kPieceSection | 1;
    job.muted_tracks = 0x0fc0;
    variant = hsf::rip::JobDescriptor(library, job);
    Check(variant.jaudio.library2.pieces[1].muted_tracks == 0x0fc0 &&
              variant.jaudio.library2.pieces[0] == library.jaudio.library2.pieces[0],
          "a variant's muted tracks replace those of its piece");

    job.id = hsf::JaudioLibrary2::kPieceSection | 2;
    Check(hsf::rip::JobDescriptor(library, job).jaudio == library.jaudio, "a piece past the list changes nothing");
}

// The tracks of a track map's layers follow its basic tracks, in the map's order. A track past the 16th has no bit.
void TestLayerTracks()
{
    const hsf::rip::TrackCounts counts = {4, 2, 0, 3, 1, 2};
    Check(hsf::rip::MutedTracks(counts) == 0x0ff0, "the directors mute every layer");
    Check(hsf::rip::LayerTracks(counts, 1) == 0x0030, "the event layer follows the basic tracks");
    Check(hsf::rip::LayerTracks(counts, 2) == 0, "a layer without tracks has none");
    Check(hsf::rip::LayerTracks(counts, 3) == 0x01c0, "the layer of enemies nearby follows the treasure layer");
    Check(hsf::rip::LayerTracks(counts, 4) == 0x0200, "the battle layer follows");
    Check(hsf::rip::LayerTracks(counts, 5) == 0x0c00, "the ground layer comes last");

    const hsf::rip::TrackCounts wide = {14, 4, 0, 0, 0, 255};
    Check(hsf::rip::LayerTracks(wide, 1) == 0xc000, "a layer keeps its tracks up to the 16th");
    Check(hsf::rip::LayerTracks(wide, 5) == 0, "a layer past the 16th track has none");
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
    TestTailWithReverb();
    TestDelayRings();
    TestTrackVolumes();
    TestVariables(end);
    TestSoundStates();
    TestVariableStates();
    TestRhythmScript();
    TestFramesUntil();
    TestSoundingDelay();
    TestJobDescriptor();
    TestLayerTracks();

    // The Wii U's archives are big-endian. The ripper refuses them, and takes the Switch's archives.
    Check(hsf::rip::IsWiiUArchive(sar_files::Archive(true, {})), "a big-endian archive is the Wii U's");
    Check(!hsf::rip::IsWiiUArchive(length_cases::Archive(false)), "a little-endian archive isn't the Wii U's");
    Check(!hsf::rip::IsWiiUArchive(std::vector<uint8_t>{'F', 'S', 'A', 'R'}), "four bytes aren't an archive");

    return Summary("rip");
}
