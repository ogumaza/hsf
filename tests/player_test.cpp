// SPDX-License-Identifier: MIT

// Tests the player and its playback (src/hsf/player.* and xsflib's Playback), on sets made from a small archive. The
// output must be the model's output. A tagged length and fade must be kept. A set without a length must end half a
// second after its sound. A seek must land where a straight render does, from a snapshot of the model too. The volume
// tag must apply. A group file outside the archive must be found. A reverb on an aux bus must play as the model plays
// it. So must a delay, whose output never returns to 0, and a set without a length must still end. Track volumes in a
// mini's descriptor must reach the sound's tracks. A scripted start in a mini's descriptor must start the sound as it
// says, with each kind of sequence variable. A set that can't play must report why. A set of version 3 must play a Wii
// sound archive's sequence on Skyward Sword HD's layer, end after it, and seek from its snapshots.

#include "hsf/player.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/nx/atk/effect.h"
#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/nx/audio/i3dl2.h"
#include "ntdlib/testing/rsar_files.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/rip/length.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/sets.h"

#include "hsf/format.h"
#include "hsf/model.h"
#include "hsf/nx.h"
#include "hsf/sar.h"

using namespace ntdlib::testing;
using namespace xsflib::testing;

namespace
{

using ntdlib::testing::rsar_files::MakeAudibleArchive;

// The archive's sounds: a note of 24 ticks that ends, the same note played over and over, the note sent only to aux
// bus A, the note played only when three variables hold 5, and the note played in each tick in which user procedure 2
// writes -1 to variable 14.
constexpr uint32_t kNote = 0x01000000;
constexpr uint32_t kLoop = 0x01000001;
constexpr uint32_t kSent = 0x01000002;
constexpr uint32_t kGated = 0x01000003;
constexpr uint32_t kAsks = 0x01000004;

// A frame of the model: 5 ms, 240 stereo pairs.
constexpr std::size_t kFrame = 240;

// The archive, and a group file holding its wave archive (file 2) when `grouped` leaves that file to the group.
struct TestArchive
{
    std::vector<uint8_t> archive;
    std::vector<uint8_t> group;
};

TestArchive MakeArchive(bool grouped)
{
    // Timebase 48 and tempo 120, with the Switch's little-endian arguments: the note at offset 5 lasts 250 ms. The
    // third sound sends it only to aux bus A (a main send of 0 and a send to A of 127). The fourth sound ends at its
    // first tick (0xA2 0xFF) unless its variable 0, the global variable 20 and track 0's variable 32 all hold 5
    // (0xF0 0x95: true when the variable isn't the value). The fifth sound runs 200 ticks with its notes' waits off
    // (0xC7 0). In each tick it sets its variable 14 to 4 (0xF0 0x80), calls user procedure 2 (0xF0 0xE0), and plays
    // the note when variable 14 then holds -1 (0xF0 0x90, then the note behind 0xA2).
    const std::vector<uint8_t> kBytecode = {
        0xB0, 48,   0xE1, 120,  0x00, 60,   127,  24,   0xFF, 0xB0, 48,   0xE1, 120,  0x00, 60,   127,
        24,   0x89, 0x0E, 0x00, 0x00, 0xFF, 0xB0, 48,   0xE1, 120,  0x00, 0xDB, 0,    0xD9, 127,  60,
        127,  24,   0xFF, 0xB0, 48,   0xE1, 120,  0x00, 0xF0, 0x95, 0,    5,    0x00, 0xA2, 0xFF, 0xF0,
        0x95, 20,   5,    0x00, 0xA2, 0xFF, 0xF0, 0x95, 32,   5,    0x00, 0xA2, 0xFF, 60,   127,  24,
        0xFF, 0xB0, 48,   0xE1, 120,  0x00, 0xC7, 0x00, 0xD4, 200,  0xF0, 0x80, 14,   0x04, 0x00, 0xF0,
        0xE0, 0x02, 0x00, 0xF0, 0x90, 14,   0xFF, 0xFF, 0xA2, 60,   127,  24,   0x80, 0x01, 0xFC, 0xFF};
    sar_files::ArchiveSpec spec;
    for (const uint32_t start : {0u, 9u, 22u, 35u, 65u})
    {
        sar_files::SoundSpec sound;
        sound.name = start == 0    ? "SEQ_NOTE"
                     : start == 9  ? "SEQ_LOOP"
                     : start == 22 ? "SEQ_SENT"
                     : start == 35 ? "SEQ_GATED"
                                   : "SEQ_ASKS";
        sound.volume = 127;
        sound.banks = {0};
        sound.start_offset = start;
        spec.sounds.push_back(sound);
    }
    spec.banks = {{"BANK", 1}};
    spec.wave_archives = {{"WARC", 2}};
    spec.players = {{"PLAYER", 2}};
    const std::vector<uint8_t> waves =
        sar_files::WaveArchive(false, {sar_files::Wave(false, sar_files::ConstantWave(false))});
    spec.files = {{sar_files::Sequence(false, kBytecode, {{"Start", 0}}), "", false},
                  {sar_files::Bank(false, sar_files::OneRegionBank()), "", false},
                  {grouped ? std::vector<uint8_t>{} : waves, "", grouped}};
    if (grouped)
    {
        spec.groups = {{"GROUP_WAVES", hsf::fsar::kExternalGroupFile}};
    }

    return {sar_files::Archive(false, spec), grouped ? sar_files::Group(false, {{2, waves}}) : std::vector<uint8_t>{}};
}

// The files of the sets under test, and a reader of them.
std::map<std::string, std::vector<uint8_t>> files;

const hsf::FileReader kReader = xsflib::testing::MapReader(files);

// Writes a library holding `archive` (and its group file, if it has one) with `descriptor`, and returns the library's
// path.
std::string WriteLibrary(const std::string& name, const TestArchive& archive, hsf::ArchiveDescriptor descriptor)
{
    hsf::ProgramBuilder b;
    descriptor.archive_path = "content/sound/test.bfsar";
    b.AddFile(descriptor.archive_path, archive.archive);
    if (!archive.group.empty())
    {
        descriptor.group_paths = {"content/sound/GROUP_WAVES.bfgrp"};
        b.AddFile(descriptor.group_paths[0], archive.group);
    }
    const std::string path = "sets/" + name + ".hsflib";
    files[path] = hsf::WritePsf(hsf::kPsfVersionHsf, descriptor.Serialize(), b.Data(), {});

    return path;
}

// Writes a .minihsf selecting `sound` from the library at `library`, with `tags`, and returns its path.
std::string WriteMini(const std::string& name, const std::string& library, uint32_t sound, hsf::Tags tags)
{
    hsf::ProgramBuilder b;
    b.AddSound(sound);
    tags["_lib"] = std::filesystem::path(library).filename().string();
    const std::string path = "sets/" + name + ".minihsf";
    files[path] = hsf::WritePsf(hsf::kPsfVersionHsf, {}, b.Data(), tags);

    return path;
}

// Writes a .minihsf like WriteMini's, with `descriptor` in its reserved area. The descriptor names the archive of a
// library that WriteLibrary wrote without a group file. The set loads it last. It therefore takes the place of the
// library's.
std::string WriteMiniWithDescriptor(const std::string& name, const std::string& library, uint32_t sound,
                                    hsf::ArchiveDescriptor descriptor)
{
    hsf::ProgramBuilder b;
    b.AddSound(sound);
    descriptor.archive_path = "content/sound/test.bfsar";
    const std::string path = "sets/" + name + ".minihsf";
    const hsf::Tags tags = {{"_lib", std::filesystem::path(library).filename().string()}};
    files[path] = hsf::WritePsf(hsf::kPsfVersionHsf, descriptor.Serialize(), b.Data(), tags);

    return path;
}

// The whole of a playback, rendered in blocks of `block` frames, or nothing if it doesn't start.
std::vector<int16_t> RenderAll(const std::string& path, std::size_t block = 1000)
{
    return xsflib::testing::RenderAll<hsf::Playback>(path, kReader, block);
}

// Checks seeks from snapshots in a playback of the set at `path` that takes a snapshot every `interval` frames. The
// playback first renders the set to its end. That render has to give `straight`, the set's whole output without
// snapshots. The snapshots then lie inside the sound, and inside the model's frames. Each seek to one of `frames`, in
// turn, starts from a snapshot after the start. A restart would drop the snapshots. Every one of them therefore has to
// stay. After each seek, the position is the frame, and the next 1,000 frames are those of `straight`.
void CheckSnapshotSeeks(const std::string& path, const std::vector<int16_t>& straight, uint64_t interval,
                        const std::vector<uint64_t>& frames)
{
    constexpr std::size_t kBlock = 1000;
    hsf::PlaybackOptions options;
    options.snapshot_interval = interval;
    hsf::Playback playback;
    std::vector<int16_t> played;
    const bool started = playback.Open(path, kReader, options) && playback.Start();
    if (started)
    {
        played = RenderFrames([&playback](int16_t* out, std::size_t count) { return playback.Render(out, count); },
                              SIZE_MAX, kBlock);
    }
    const std::size_t snapshots = playback.SnapshotCount();
    Check(played == straight && snapshots > 1, path + ": a render that takes snapshots plays the same");

    std::vector<int16_t> buffer(2 * kBlock);
    bool landed = started;
    for (const uint64_t frame : frames)
    {
        const uint64_t from = playback.SeekStart(frame);
        landed = landed && from > 0 && from <= frame && playback.Seek(frame) && playback.Position() == frame &&
                 playback.SnapshotCount() >= snapshots && playback.Render(buffer.data(), kBlock) == kBlock &&
                 straight.size() >= 2 * (frame + kBlock) &&
                 std::equal(buffer.begin(), buffer.end(), straight.begin() + static_cast<std::ptrdiff_t>(2 * frame));
    }
    Check(landed, path + ": seeks from snapshots land where a straight render does");
}

// The model's output for `sound` (`frames` frames), with `reverb` on aux bus A when given, and the frame in which the
// model went idle with a silent output (0 if it never did).
std::vector<int16_t> ModelOutput(const std::vector<uint8_t>& archive_bytes, uint32_t sound, int frames, int& idle,
                                 const hsf::audio::I3dl2ReverbParameterSet* reverb = nullptr)
{
    const hsf::fsar::SoundArchive archive = hsf::fsar::SoundArchive::Load(archive_bytes, hsf::kSarOptions);
    hsf::atk::ArchiveModel model(archive);
    if (reverb)
    {
        model.engine->AppendEffect(0, std::make_unique<hsf::atk::I3dl2AuxEffect>(*reverb));
    }
    model.engine->StartSequence(hsf::fsar::ItemNumber(sound));
    std::vector<int16_t> out;
    idle = 0;
    for (int frame = 1; frame <= frames; ++frame)
    {
        model.engine->RunFrame();
        const std::span<const int16_t> output = model.renderer.DeviceOutput();
        out.insert(out.end(), output.begin(), output.end());
        const bool silent = std::all_of(output.begin(), output.end(), [](int16_t s) { return s == 0; });
        if (idle == 0 && silent && !model.engine->IsBusy())
        {
            idle = frame;
        }
    }

    return out;
}

// A set with a tagged length and fade, and seeks in it: straight, and from snapshots of the model.
void TestTagged(const TestArchive& plain, const std::string& library)
{
    int idle = 0;
    const std::vector<int16_t> loop = ModelOutput(plain.archive, kLoop, 600, idle);

    // A tagged length and fade: 1 s, then a fade of half a second, rendered in blocks that don't divide a frame.
    const std::string tagged_mini = WriteMini("tagged", library, kLoop, {{"length", "0:01"}, {"fade", "0.5"}});
    const std::vector<int16_t> tagged = RenderAll(tagged_mini, 1001);
    CheckLengthAndFade(tagged, loop, 48000, 24000);

    // A seek forward and a seek back land where the straight render does.
    CheckSeeks<hsf::Playback>(tagged_mini, kReader, tagged, {30000, 10000});

    // So do seeks back and forward from snapshots taken every 2,000 frames, in the middle of the notes.
    CheckSnapshotSeeks(tagged_mini, tagged, 2000, {65000, 30123, 47999, 2345, 70500});
}

// A set without a length, the same set with a volume tag, and one whose waves are in a group file.
void TestUntagged(const TestArchive& plain, const std::string& library)
{
    int idle = 0;
    const std::vector<int16_t> note = ModelOutput(plain.archive, kNote, 400, idle);

    // Without a length, the sound ends half a second after the model goes idle and its output falls silent.
    const std::vector<int16_t> untagged = RenderAll(WriteMini("untagged", library, kNote, {}));
    CheckEnd(untagged, note, static_cast<std::size_t>(idle) * kFrame, 24000);

    // The volume tag scales every sample.
    CheckVolume(RenderAll(WriteMini("quiet", library, kNote, {{"volume", "0.5"}})), untagged, 0.5);

    // A wave archive that only a group file outside the archive holds is found there.
    const TestArchive grouped = MakeArchive(true);
    const std::string grouped_library = WriteLibrary("grouped", grouped, {});
    Check(RenderAll(WriteMini("grouped", grouped_library, kNote, {})) == untagged,
          "a set whose waves are in a group file plays the same");
}

// An I3DL2 reverb on aux bus A plays as the model plays it, and its tail runs on after the note before the sound ends.
void TestI3dl2Reverb(const TestArchive& plain)
{
    const hsf::audio::I3dl2ReverbParameterSet kReverb{-1000.0f, -1000.0f, 2.0f,  0.8f, -700.0f, 0.005f,
                                                      -500.0f,  0.04f,    50.0f, 3.0f, 6000.0f, 0.25f};
    hsf::ArchiveDescriptor descriptor;
    hsf::AuxEffect effect;
    effect.parameters = hsf::I3dl2Values(kReverb);
    descriptor.effects = {effect};
    const std::string reverb_library = WriteLibrary("reverb", plain, descriptor);

    int dry_idle = 0;
    int wet_idle = 0;
    ModelOutput(plain.archive, kSent, 2000, dry_idle);
    const std::vector<int16_t> wet = ModelOutput(plain.archive, kSent, 2000, wet_idle, &kReverb);

    const std::string reverb_mini = WriteMini("reverb", reverb_library, kSent, {});
    const std::vector<int16_t> played = RenderAll(reverb_mini);
    Check(wet_idle > dry_idle + 20 && played.size() == 2 * (static_cast<std::size_t>(wet_idle) * kFrame + 24000) &&
              std::equal(played.begin(), played.end() - 2 * 24000, wet.begin()),
          "a set with a reverb plays the model's output, and its tail");

    // A seek from a snapshot in the reverb's tail lands where the straight render does: a snapshot holds the reverb's
    // state.
    const auto dry_end = static_cast<uint64_t>(dry_idle) * kFrame;
    const auto wet_end = static_cast<uint64_t>(wet_idle) * kFrame;
    CheckSnapshotSeeks(reverb_mini, played, 2000, {wet_end - 1500, dry_end + 777, (dry_end + wet_end) / 2, 4321});
}

// An archive with one sequence: C4 for 250 ms, at full level on the main bus and on aux bus A. Program 0 plays a square
// wave of 1 s that ends: 0x1000 and -0x1000 by turns, 16 samples of each.
TestArchive SquareArchive()
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
    sar_files::SoundSpec sound;
    sound.name = "SEQ_SQUARE";
    sound.volume = 127;
    sound.banks = {0};
    spec.sounds = {sound};
    spec.banks = {{"BANK", 1}};
    spec.wave_archives = {{"WARC", 2}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{sar_files::Sequence(false, kBytecode, {{"Start", 0}}), "", false},
                  {sar_files::Bank(false, sar_files::OneRegionBank()), "", false},
                  {sar_files::WaveArchive(false, {sar_files::Wave(false, wave)}), "", false}};

    return {sar_files::Archive(false, spec), {}};
}

// A delay of 400 ms on aux bus A, with a feedback of 0.5 and no dry part, plays as the model plays it. The delay rounds
// down. Its output therefore never returns to 0. A set without a length still ends: half a second after the effects
// stop ringing, 81 frames after the last audible frame. A seek back into the echoes from a snapshot ends there too.
void TestDelay()
{
    const TestArchive square = SquareArchive();
    hsf::AuxEffect delay;
    delay.type = hsf::AuxEffect::kDelay;
    delay.parameters = {1000000.0f, 400000.0f, 1.0f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f};
    hsf::ArchiveDescriptor descriptor;
    descriptor.effects = {delay};
    const std::string mini = WriteMini("delay", WriteLibrary("delay", square, descriptor), 0x01000000, {});

    // The model's output for 20 s, its last audible frame, and whether its last 80 frames hold a sample other than 0.
    const hsf::fsar::SoundArchive archive = hsf::fsar::SoundArchive::Load(square.archive, hsf::kSarOptions);
    hsf::atk::ArchiveModel model(archive);
    hsf::AppendEffects(*model.engine, descriptor.effects);
    model.engine->StartSequence(0);
    std::vector<int16_t> wet;
    std::size_t last_audible = 0;
    bool ringing = false;
    for (std::size_t frame = 1; frame <= 4000; ++frame)
    {
        model.engine->RunFrame();
        const std::span<const int16_t> output = model.renderer.DeviceOutput();
        wet.insert(wet.end(), output.begin(), output.end());
        last_audible = xsflib::rip::Audible(output) ? frame : last_audible;
        ringing =
            ringing || (frame > 3920 && std::any_of(output.begin(), output.end(), [](int16_t s) { return s != 0; }));
    }

    const std::vector<int16_t> played = RenderAll(mini);
    Check(ringing && last_audible > 160 && last_audible < 3000, "the delay's echoes die away, and -1 stays");
    CheckEnd(played, wet, (last_audible + 81) * kFrame, 24000);

    hsf::PlaybackOptions options;
    options.snapshot_interval = 2000;
    hsf::Playback playback;
    const auto render = [&playback](int16_t* out, std::size_t frames)
    {
        return playback.Render(out, frames);
    };
    const uint64_t target = (last_audible + 40) * kFrame;
    bool same = playback.Open(mini, kReader, options) && playback.Start() &&
                RenderFrames(render, SIZE_MAX, 1000) == played && playback.SeekStart(target) > 0 &&
                playback.Seek(target);
    const std::vector<int16_t> rest = RenderFrames(render, SIZE_MAX, 1000);
    same = same && 2 * target + rest.size() == played.size() &&
           std::equal(rest.begin(), rest.end(), played.begin() + static_cast<std::ptrdiff_t>(2 * target));
    Check(same, "a seek back into the delay's echoes, from a snapshot, ends where the straight render does");
}

// The renderer's reverb on aux bus A, on 4 channels with no dry part, plays as the model plays it. A set without a
// length ends half a second after the effects stop ringing, 110 frames after the last audible frame. The reverb's tail
// differs from what went in.
void TestRendererReverb()
{
    const TestArchive square = SquareArchive();
    hsf::AuxEffect reverb;
    reverb.type = hsf::AuxEffect::kReverb;
    reverb.parameters = {2.0f, 0.7f, 15000.0f, 1.0f, 0.7f, 1500000.0f, 0.8f, 0.7f, 0.4f, 1.0f, 0.0f, 2.0f};
    hsf::ArchiveDescriptor descriptor;
    descriptor.effects = {reverb};
    const std::string mini = WriteMini("reverb", WriteLibrary("reverb", square, descriptor), 0x01000000, {});

    // The model's output for 20 s and its last audible frame, then the same without the reverb.
    const hsf::fsar::SoundArchive archive = hsf::fsar::SoundArchive::Load(square.archive, hsf::kSarOptions);
    const auto render = [&archive](const std::vector<hsf::AuxEffect>& effects, std::size_t& last_audible)
    {
        hsf::atk::ArchiveModel model(archive);
        hsf::AppendEffects(*model.engine, effects);
        model.engine->StartSequence(0);
        std::vector<int16_t> out;
        for (std::size_t frame = 1; frame <= 4000; ++frame)
        {
            model.engine->RunFrame();
            const std::span<const int16_t> output = model.renderer.DeviceOutput();
            out.insert(out.end(), output.begin(), output.end());
            last_audible = xsflib::rip::Audible(output) ? frame : last_audible;
        }

        return out;
    };

    std::size_t last_audible = 0;
    std::size_t last_dry = 0;
    const std::vector<int16_t> wet = render(descriptor.effects, last_audible);
    const std::vector<int16_t> dry = render({}, last_dry);

    const std::vector<int16_t> played = RenderAll(mini);
    Check(last_audible > last_dry + 20 && last_audible < 3000, "the reverb's tail rings on, then dies away");
    CheckEnd(played, wet, (last_audible + 110) * kFrame, 24000);
}

// Track volumes in a mini's descriptor (docs/hsf.md, "Track volumes"). The sound's note plays on track 0. A volume of 0
// for track 0 silences it, and 0.5 halves its level. The volumes of another sound leave the sound as it is.
void TestTrackVolumes(const std::string& library)
{
    const std::vector<int16_t> full = RenderAll(WriteMini("full", library, kNote, {}));
    const auto with_volume = [&library](const std::string& name, uint32_t listed, float volume)
    {
        hsf::SoundTrackVolumes entry;
        entry.sound = listed;
        entry.volumes[0] = volume;
        hsf::ArchiveDescriptor descriptor;
        descriptor.flags = hsf::ArchiveDescriptor::kFlagTrackVolumes;
        descriptor.track_volumes = {entry};

        return RenderAll(WriteMiniWithDescriptor(name, library, kNote, descriptor));
    };

    const std::vector<int16_t> silenced = with_volume("silenced", kNote, 0.0f);
    const std::vector<int16_t> half = with_volume("half", kNote, 0.5f);
    const int peak = Peak(full);
    Check(peak > 1000 && !silenced.empty() && Peak(silenced) == 0, "a track volume of 0 silences the track");
    Check(half.size() == full.size() && std::abs(2 * Peak(half) - peak) <= 2,
          "a track volume of 0.5 halves the track's level: a peak of " + std::to_string(Peak(half)) + " for " +
              std::to_string(peak));
    Check(with_volume("other", kLoop, 0.0f) == full, "the track volumes of another sound leave the sound as it is");
}

// Scripted starts in a mini's descriptor (docs/hsf.md, "Scripted starts"). The gated sound plays the note only when
// its variable 0, the global variable 20 and track 0's variable 32 all hold 5 at its first tick. It then plays as the
// plain note does. Without any one of them, or with another value, it ends at its first tick. So it does with a script
// of another sound, and with a write 20 frames after the start. A lead-in of 10 frames leaves the note's first 10
// frames out of the output. With a start at the end of the lead-in, the output is the note's. A sound that the script
// starts beside the set's sound plays with it, and the set plays on until the script's last step. Seeks land there as
// a straight render does.
void TestScriptedStarts(const std::string& library)
{
    using hsf::ScriptStep;
    const std::vector<int16_t> note = RenderAll(WriteMini("note", library, kNote, {}));
    const auto with_script = [&library](const std::string& name, uint32_t sound, const hsf::ScriptedStart& script)
    {
        hsf::ArchiveDescriptor descriptor;
        descriptor.flags = hsf::ArchiveDescriptor::kFlagScripts;
        descriptor.scripts = {script};

        return WriteMiniWithDescriptor(name, library, sound, descriptor);
    };

    const auto gated = [&with_script](const std::string& name, std::vector<ScriptStep> steps)
    {
        return RenderAll(with_script(name, kGated, {kGated, 0, std::move(steps)}));
    };

    const auto silent = [](const std::vector<int16_t>& samples)
    {
        return !samples.empty() && Peak(samples) == 0;
    };

    const ScriptStep global = ScriptStep::Write(0, 20, 5);
    const ScriptStep start = ScriptStep::Start(0, kGated);
    const ScriptStep local = ScriptStep::Write(0, 0, 5, kGated);
    const ScriptStep track = ScriptStep::Write(0, 32, 5, kGated, 0);
    Check(Peak(note) > 1000 && gated("gated", {global, start, local, track}) == note,
          "a sound's variable, a global variable and a track's variable reach the sequence");
    Check(silent(gated("no local", {global, start, track})), "without its variable 0, the sound ends");
    Check(silent(gated("no global", {start, local, track})), "without the global variable, the sound ends");
    Check(silent(gated("no track", {global, start, local})), "without track 0's variable, the sound ends");
    Check(silent(gated("other value", {global, start, local, ScriptStep::Write(0, 32, 4, kGated, 0)})),
          "a variable of another value ends the sound");
    Check(silent(gated("other track", {global, start, local, ScriptStep::Write(0, 32, 5, kGated, 1)})),
          "another track's variable ends the sound");
    Check(silent(gated("late write", {global, start, local, ScriptStep::Write(20, 32, 5, kGated, 0)})),
          "a write after the sound's first tick comes too late");
    const hsf::ScriptedStart of_note = {kNote,
                                        0,
                                        {global, ScriptStep::Start(0, kNote), ScriptStep::Write(0, 0, 5, kNote),
                                         ScriptStep::Write(0, 32, 5, kNote, 0)}};
    Check(silent(RenderAll(with_script("other sound", kGated, of_note))),
          "another sound's script leaves the sound out");

    // The lead-in.
    constexpr uint32_t kLeadIn = 10;
    const std::vector<int16_t> late =
        RenderAll(with_script("lead-in", kNote, {kNote, kLeadIn, {ScriptStep::Start(0, kNote)}}));
    Check(
        note.size() > 2 * kFrame * kLeadIn &&
            late == std::vector<int16_t>(note.begin() + static_cast<std::ptrdiff_t>(2 * kFrame * kLeadIn), note.end()),
        "a lead-in leaves its frames out of the output");
    Check(RenderAll(with_script("start at the end", kNote, {kNote, kLeadIn, {ScriptStep::Start(kLeadIn, kNote)}})) ==
              note,
          "a start at the end of the lead-in plays from the output's start");

    // A sound beside the set's sound, and a later start that holds the set open. The gated sound plays the note again
    // after the first note's output has ended.
    Check(gated("beside", {ScriptStep::Start(0, kNote), start}) == note, "a sound that the script starts plays too");
    const auto later = static_cast<uint32_t>(note.size() / (2 * kFrame) + 100);
    const hsf::ScriptedStart again = {
        kNote,
        0,
        {ScriptStep::Start(0, kNote), ScriptStep::Write(later, 20, 5), ScriptStep::Start(later, kGated),
         ScriptStep::Write(later, 0, 5, kGated), ScriptStep::Write(later, 32, 5, kGated, 0)}};
    const std::string twice = with_script("twice", kNote, again);
    const std::vector<int16_t> played = RenderAll(twice);
    const std::size_t at = 2 * kFrame * later;
    const auto part = [&played](std::size_t from, std::size_t to)
    {
        return std::vector<int16_t>(played.begin() + static_cast<std::ptrdiff_t>(from),
                                    played.begin() + static_cast<std::ptrdiff_t>(to));
    };
    Check(played.size() == at + note.size() && part(0, note.size()) == note && Peak(part(note.size(), at)) == 0 &&
              part(at, played.size()) == note,
          "a start after the sound has ended plays, and the set waits for it");
    CheckSnapshotSeeks(twice, played, 2000, {at / 2 + 333, 2345, at / 2 - 1500, 2777});
}

// The frames of a render that hold an audible sample, from the first to the last, or (-1, -1) for none.
std::pair<int64_t, int64_t> AudibleFrames(const std::vector<int16_t>& samples)
{
    int64_t first = -1;
    int64_t last = -1;
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        if (samples[i] != 0)
        {
            const auto frame = static_cast<int64_t>(i / (2 * kFrame));
            first = first < 0 ? frame : first;
            last = frame;
        }
    }

    return {first, last};
}

// Writes of kind 2 in a mini's descriptor (docs/hsf.md, "Scripted starts"). The sound plays its note in a tick only
// when user procedure 2 has written -1 to its variable 14 in that tick. A tick comes every 2.08 frames. A write of
// kind 2 from frame 100 takes effect in the first call from then on, and the note plays from that tick for its 250 ms
// (50 frames). The next tick sets the variable to 4 again, and the note doesn't come back. A second write from frame
// 300 plays the note again. A write of kind 1 at frame 100 comes between ticks. The next tick sets the variable to 4
// before its test, and the note never plays. A write of kind 2 for a procedure that the sound never calls never takes
// effect. Nor does one for a sound beside it whose sequence calls no procedure. Seeks land where a straight render
// does, before and after the writes.
void TestWritesInProcedures(const std::string& library)
{
    using hsf::ScriptStep;
    const auto asks = [&library](const std::string& name, std::vector<ScriptStep> steps)
    {
        hsf::ArchiveDescriptor descriptor;
        descriptor.flags = hsf::ArchiveDescriptor::kFlagScripts;
        descriptor.scripts = {{kAsks, 0, std::move(steps)}};

        return WriteMiniWithDescriptor(name, library, kAsks, descriptor);
    };

    const ScriptStep start = ScriptStep::Start(0, kAsks);

    const std::vector<int16_t> once =
        RenderAll(asks("in procedure", {start, ScriptStep::WriteInProcedure(100, 2, 14, -1, kAsks)}));
    const auto [first, last] = AudibleFrames(once);
    Check(first >= 100 && first <= 104 && last >= first + 45 && last <= first + 60,
          "a write of kind 2 plays the note from the first call after its frame: frames " + std::to_string(first) +
              " to " + std::to_string(last));

    const std::string twice = asks("in procedure twice", {start, ScriptStep::WriteInProcedure(100, 2, 14, -1, kAsks),
                                                          ScriptStep::WriteInProcedure(300, 2, 14, -1, kAsks)});
    const std::vector<int16_t> played = RenderAll(twice);
    const std::size_t at = 2 * kFrame * 300;
    const auto [again, end] =
        AudibleFrames(std::vector<int16_t>(played.begin() + static_cast<std::ptrdiff_t>(at), played.end()));
    Check(played.size() == once.size() &&
              std::equal(once.begin(), once.begin() + static_cast<std::ptrdiff_t>(at), played.begin()) && again >= 0 &&
              again <= 4 && end >= again + 45 && end <= again + 60,
          "a second write of kind 2 plays the note again from its frame: frames " + std::to_string(300 + again) +
              " to " + std::to_string(300 + end));

    const auto silent = [](const std::vector<int16_t>& samples)
    {
        return !samples.empty() && AudibleFrames(samples).first < 0;
    };
    Check(silent(RenderAll(asks("timed write", {start, ScriptStep::Write(100, 14, -1, kAsks)}))),
          "a write of kind 1 comes too late for the test");
    Check(silent(RenderAll(asks("other procedure", {start, ScriptStep::WriteInProcedure(100, 3, 14, -1, kAsks)}))),
          "a write for another procedure never takes effect");
    const std::vector<int16_t> note = RenderAll(WriteMini("note alone", library, kNote, {}));
    const std::vector<int16_t> beside = RenderAll(
        asks("other sound", {start, ScriptStep::Start(0, kNote), ScriptStep::WriteInProcedure(100, 2, 14, -1, kNote)}));
    Check(AudibleFrames(beside).second == AudibleFrames(note).second,
          "a write for another sound leaves the sound silent: only the other sound's note plays");

    CheckSnapshotSeeks(twice, played, 2000, {30000, 10000, 49000, 73000, 47000});
}

// The game's handler of user procedures (docs/hsf.md, "User procedures"). The sound runs 200 ticks with its notes'
// waits off. In each tick it sets track variables 14 and 15 to slot 5 and a count of 3, calls user procedure 0, and
// plays the note when variable 14 then holds 1. From the constructor's state, Jamboree's handler draws 1, 0, 2, 0, 1,
// 2, 0, 2 there, as fn_103b80 does in a run of the game's code. The note therefore plays from the first tick. Without
// the handler, variable 14 keeps the slot, and the sound is silent. Superstars' handler reads the sound's variables 14
// and 15, at -1, and changes nothing. Seeks land where a straight render does: the snapshots hold the handler's state.
void TestUserProcedures()
{
    const std::vector<uint8_t> kBytecode = {0xB0, 48,   0xE1, 120,  0x00, 0xC7, 0x00, 0xD4, 200,  0xF0, 0x80, 0x2E,
                                            0x05, 0x00, 0xF0, 0x80, 0x2F, 0x03, 0x00, 0xF0, 0xE0, 0x00, 0x00, 0xF0,
                                            0x90, 0x2E, 0x01, 0x00, 0xA2, 60,   127,  24,   0x80, 0x01, 0xFC, 0xFF};
    sar_files::ArchiveSpec spec;
    sar_files::SoundSpec sound;
    sound.name = "SEQ_DRAWS";
    sound.volume = 127;
    sound.banks = {0};
    spec.sounds = {sound};
    spec.banks = {{"BANK", 1}};
    spec.wave_archives = {{"WARC", 2}};
    spec.players = {{"PLAYER", 2}};
    const std::vector<uint8_t> waves =
        sar_files::WaveArchive(false, {sar_files::Wave(false, sar_files::ConstantWave(false))});
    spec.files = {{sar_files::Sequence(false, kBytecode, {{"Start", 0}}), "", false},
                  {sar_files::Bank(false, sar_files::OneRegionBank()), "", false},
                  {waves, "", false}};
    const TestArchive draws = {sar_files::Archive(false, spec), {}};
    const auto with_handler = [&draws](const std::string& name, uint32_t handler)
    {
        hsf::ArchiveDescriptor descriptor;
        if (handler != hsf::UserProcedureHandler::kNone)
        {
            descriptor.flags = hsf::ArchiveDescriptor::kFlagProcedures;
            descriptor.procedures.handler = handler;
        }

        return WriteMini(name, WriteLibrary(name, draws, descriptor), kNote, {});
    };

    const std::string jamboree = with_handler("draws jamboree", hsf::UserProcedureHandler::kJamboree);
    const std::vector<int16_t> played = RenderAll(jamboree);
    const auto [first, last] = AudibleFrames(played);
    Check(first >= 0 && first <= 2 && last >= 400,
          "Jamboree's handler draws 1 in the first call, and the note plays from the first tick: frames " +
              std::to_string(first) + " to " + std::to_string(last));

    const auto silent = [](const std::vector<int16_t>& samples)
    {
        return !samples.empty() && AudibleFrames(samples).first < 0;
    };
    Check(silent(RenderAll(with_handler("draws none", hsf::UserProcedureHandler::kNone))),
          "without a handler, the variable keeps the slot, and the sound is silent");
    Check(silent(RenderAll(with_handler("draws superstars", hsf::UserProcedureHandler::kSuperstars))),
          "Superstars' handler reads the sound's variables and changes nothing");

    CheckSnapshotSeeks(jamboree, played, 2000, {30000, 10000, 49000, 73000, 47000});
}

// Sets that can't play: a group file the set doesn't hold, a sound the archive doesn't have, an item that isn't a
// sound, and a descriptor's voice count beyond the most a player has.
void TestRefusals(const TestArchive& plain, const std::string& library)
{
    const TestArchive grouped = MakeArchive(true);
    hsf::ArchiveDescriptor missing_group;
    missing_group.archive_path = "content/sound/test.bfsar";
    missing_group.group_paths = {"content/sound/GROUP_ELSEWHERE.bfgrp"};
    hsf::ProgramBuilder b;
    b.AddFile(missing_group.archive_path, grouped.archive);
    files["sets/missing.hsflib"] = hsf::WritePsf(hsf::kPsfVersionHsf, missing_group.Serialize(), b.Data(), {});

    hsf::ArchiveDescriptor many;
    many.voice_count = 97;
    const std::string many_library = WriteLibrary("many", plain, many);

    hsf::ArchiveDescriptor flagged;
    flagged.flags = 0x80000000;
    hsf::ArchiveDescriptor absent_companion;
    absent_companion.flags = hsf::ArchiveDescriptor::kFlagScripts;
    absent_companion.scripts = {{kNote, 0, {hsf::ScriptStep::Start(0, 0x01000005), hsf::ScriptStep::Start(0, kNote)}}};

    const std::map<std::string, std::string> refusals = {
        {WriteMini("missing", "sets/missing.hsflib", kNote, {}), "the set has no file content/sound/GROUP_ELSEWHERE"},
        {WriteMini("absent", library, 0x01000005, {}), "isn't a sequence"},
        {WriteMini("bank", library, 0x05000000, {}), "isn't a sequence"},
        {WriteMini("many", many_library, kNote, {}), "voice count is 97"},
        {WriteMiniWithDescriptor("flagged", library, kNote, flagged), "version 1 has only bits 0"},
        {WriteMiniWithDescriptor("absent companion", library, kNote, absent_companion), "isn't a sequence"}};
    CheckRefusals<hsf::Playback>(refusals, kReader);
}

// Writes a library of version 3 that holds `archive`, and returns its path.
std::string WriteNw4rLibrary(const std::string& name, const std::vector<uint8_t>& archive)
{
    const hsf::ArchiveDescriptor descriptor = hsf::ArchiveDescriptor::Nw4r({}, "Sound/test.brsar", 110400);
    hsf::ProgramBuilder b;
    b.AddFile(descriptor.archive_path, archive);
    const std::string path = "sets/" + name + ".hsflib";
    files[path] = hsf::WritePsf(hsf::kPsfVersionHsf, descriptor.Serialize(), b.Data(), {});

    return path;
}

// The last frame of `samples` with a sample louder than ±8 (xsflib's Audible), or -1.
int64_t LastHeardFrame(const std::vector<int16_t>& samples)
{
    const std::span<const int16_t> all(samples);
    int64_t heard = -1;
    for (std::size_t f = 0; (f + 1) * 2 * kFrame <= samples.size(); ++f)
    {
        if (xsflib::rip::Audible(all.subspan(f * 2 * kFrame, 2 * kFrame)))
        {
            heard = static_cast<int64_t>(f);
        }
    }

    return heard;
}

// Sets of version 3. Each track sets its notes' envelope first (D1 7F D2 7F D3 7F): a full sustain at once and the
// fastest release. The sequences play at 96 ticks a second. Without a length, a set ends once the reverbs on the aux
// buses have stopped ringing, a second after the last frame louder than ±8, and half a second after that.
void TestNw4r()
{
    // A note of 24 ticks that ends: 250 ms of sound.
    const std::string note_library =
        WriteNw4rLibrary("nw4r note", MakeAudibleArchive({0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 60, 127, 24, 0xFF}, 1));
    const std::vector<int16_t> note = RenderAll(WriteMini("nw4r note", note_library, 0, {}));
    const auto [first, last] = AudibleFrames(note);
    const int64_t heard = LastHeardFrame(note);
    const auto end = static_cast<int64_t>(note.size() / (2 * kFrame));
    Check(first >= 0 && first < 20 && last - first >= 48 && last - first <= 60,
          "a set of version 3 plays its note for 250 ms (frames " + std::to_string(first) + " to " +
              std::to_string(last) + ")");
    Check(end >= heard + 299 && end <= heard + 303,
          "a set of version 3 without a length ends a second and a half after its sound (sound to frame " +
              std::to_string(heard) + ", end at frame " + std::to_string(end) + ")");

    // The same note, sent in full to aux bus B (DA 7F). The reverb's arithmetic rounds down. Samples of ±1 go on long
    // after its tail falls below ±8, with frames of silence among them.
    const std::string reverb_library = WriteNw4rLibrary(
        "nw4r reverb", MakeAudibleArchive({0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 0xDA, 0x7F, 60, 127, 24, 0xFF}, 1));
    const std::vector<int16_t> rung = RenderAll(WriteMini("nw4r reverb", reverb_library, 0, {}));
    const int64_t rung_heard = LastHeardFrame(rung);
    const int64_t tail = AudibleFrames(rung).second;
    const auto rung_end = static_cast<int64_t>(rung.size() / (2 * kFrame));
    const std::string frames = "sound to frame " + std::to_string(rung_heard) + ", samples other than 0 to frame " +
                               std::to_string(tail) + ", end at frame " + std::to_string(rung_end);
    Check(
        rung_heard > heard && tail >= rung_heard + 200 && rung_end >= rung_heard + 299 && rung_end <= rung_heard + 303,
        "a set of version 3 without a length ends a second and a half after its reverb's last sound (" + frames + ")");

    // The note played over and over, for 1 s and a fade of half a second. Seeks from snapshots taken every 2,000
    // frames land where a straight render does.
    const std::string loop_library = WriteNw4rLibrary(
        "nw4r loop",
        MakeAudibleArchive({0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 60, 127, 24, 0x89, 0x00, 0x00, 6, 0xFF}, 1));
    const std::string loop = WriteMini("nw4r loop", loop_library, 0, {{"length", "0:01"}, {"fade", "0.5"}});
    const std::vector<int16_t> looped = RenderAll(loop, 1001);
    Check(looped.size() == 2 * 72000, "a set of version 3 keeps its tagged length and fade");
    CheckSnapshotSeeks(loop, looped, 2000, {65000, 30123, 47999, 2345, 70500});

    // A sound that isn't a sequence, and a layer that the player doesn't have.
    hsf::ArchiveDescriptor layer2 = hsf::ArchiveDescriptor::Nw4r({}, "Sound/test.brsar", 110400);
    layer2.nw4r.layer = 2;
    hsf::ProgramBuilder b;
    b.AddSound(0);
    files["sets/nw4r layer 2.minihsf"] =
        hsf::WritePsf(hsf::kPsfVersionHsf, layer2.Serialize(), b.Data(), {{"_lib", "nw4r note.hsflib"}});
    const std::map<std::string, std::string> refusals = {
        {WriteMini("nw4r absent", note_library, 5, {}), "isn't a sequence"},
        {"sets/nw4r layer 2.minihsf", "layer is 2"}};
    CheckRefusals<hsf::Playback>(refusals, kReader);
}

} // namespace

int main()
{
    const TestArchive plain = MakeArchive(false);
    const std::string library = WriteLibrary("plain", plain, {});

    TestTagged(plain, library);
    TestUntagged(plain, library);
    TestI3dl2Reverb(plain);
    TestDelay();
    TestRendererReverb();
    TestTrackVolumes(library);
    TestScriptedStarts(library);
    TestWritesInProcedures(library);
    TestUserProcedures();
    TestRefusals(plain, library);
    TestNw4r();

    return xsflib::testing::Summary("player");
}
