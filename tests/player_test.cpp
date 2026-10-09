// SPDX-License-Identifier: MIT

// Tests the player and its playback (src/hsf/player.* and playback.*), on sets made from a small archive. The output
// must be the model's output. A tagged length and fade must be kept. A set without a length must end half a second
// after its sound. A seek must land where a straight render does. The volume tag must apply. A group file outside the
// archive must be found. A reverb on an aux bus must play as the model plays it. A set that can't play must report why.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "atk/effect.h"
#include "atk/engine.h"
#include "audio/i3dl2.h"
#include "check.h"
#include "fsar/formats.h"
#include "fsar_files.h"
#include "hsf/format.h"
#include "hsf/playback.h"

namespace
{

// The archive's sounds: a note of 24 ticks that ends, the same note played over and over, and the note sent only to aux
// bus A.
constexpr uint32_t kNote = 0x01000000;
constexpr uint32_t kLoop = 0x01000001;
constexpr uint32_t kSent = 0x01000002;

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
    std::vector<uint8_t> samples(2 * 32000);
    for (std::size_t i = 0; i < samples.size(); i += 2)
    {
        samples[i + 1] = 0x10; // 0x1000, little-endian
    }
    fsar_files::WaveSpec wave;
    wave.encoding = 1;
    wave.loop_end = 32000;
    wave.channels = {samples};

    fsar_files::Region region;
    region.original_key = 60;
    region.adshr = {127, 127, 127, 0, 127};
    fsar_files::VelocityTable velocities;
    velocities.entries = {{0, region}};
    fsar_files::KeyTable keys;
    keys.entries = {{0, velocities}};
    fsar_files::BankSpec bank;
    bank.waves = {{0x05000000, 0}};
    bank.instruments = {keys};

    // Timebase 48 and tempo 120, with the Switch's little-endian arguments: the note at offset 5 lasts 250 ms. The
    // third sound sends it only to aux bus A (a main send of 0 and a send to A of 127).
    const std::vector<uint8_t> kBytecode = {0xB0, 48,   0xE1, 120,  0x00, 60,   127,  24,   0xFF, 0xB0, 48,   0xE1,
                                            120,  0x00, 60,   127,  24,   0x89, 0x0E, 0x00, 0x00, 0xFF, 0xB0, 48,
                                            0xE1, 120,  0x00, 0xDB, 0,    0xD9, 127,  60,   127,  24,   0xFF};
    fsar_files::ArchiveSpec spec;
    for (const uint32_t start : {0u, 9u, 22u})
    {
        fsar_files::SoundSpec sound;
        sound.name = start == 0 ? "SEQ_NOTE" : start == 9 ? "SEQ_LOOP" : "SEQ_SENT";
        sound.volume = 127;
        sound.banks = {0};
        sound.start_offset = start;
        spec.sounds.push_back(sound);
    }
    spec.banks = {{"BANK", 1}};
    spec.wave_archives = {{"WARC", 2}};
    spec.players = {{"PLAYER", 2}};
    const std::vector<uint8_t> waves = fsar_files::WaveArchive(false, {fsar_files::Wave(false, wave)});
    spec.files = {{fsar_files::Sequence(false, kBytecode, {{"Start", 0}}), "", false},
                  {fsar_files::Bank(false, bank), "", false},
                  {grouped ? std::vector<uint8_t>{} : waves, "", grouped}};
    if (grouped)
    {
        spec.groups = {{"GROUP_WAVES", 0xFFFFFFFF}};
    }

    return {fsar_files::Archive(false, spec),
            grouped ? fsar_files::Group(false, {{2, waves}}) : std::vector<uint8_t>{}};
}

// The files of the sets under test, and a reader of them.
std::map<std::string, std::vector<uint8_t>> files;

const hsf::FileReader kReader = [](const std::string& path, std::vector<uint8_t>& out)
{
    const auto it = files.find(std::filesystem::path(path).lexically_normal().generic_string());
    if (it == files.end())
    {
        return false;
    }

    out = it->second;

    return true;
};

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

// The whole of a playback, rendered in blocks of `block` frames, or nothing if it doesn't start.
std::vector<int16_t> RenderAll(const std::string& path, std::size_t block = 1000)
{
    hsf::Playback playback;
    if (!Check(playback.Open(path, kReader) && playback.Start(), path + " plays: " + playback.Error()))
    {
        return {};
    }

    std::vector<int16_t> out;
    std::vector<int16_t> buffer(block * 2);
    while (const std::size_t got = playback.Render(buffer.data(), block))
    {
        out.insert(out.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(got * 2));
    }

    return out;
}

// The model's output for `sound` (`frames` frames), with `reverb` on aux bus A when given, and the frame in which the
// model went idle with a silent output (0 if it never did).
std::vector<int16_t> ModelOutput(const std::vector<uint8_t>& archive_bytes, uint32_t sound, int frames, int& idle,
                                 const hsf::audio::I3dl2ReverbParameterSet* reverb = nullptr)
{
    const hsf::fsar::SoundArchive archive = hsf::fsar::SoundArchive::Load(archive_bytes);
    hsf::atk::ArchiveModel model(archive);
    if (reverb)
    {
        model.engine->AppendEffect(0, std::make_unique<hsf::atk::I3dl2AuxEffect>(*reverb));
    }
    model.engine->StartSequence(sound & 0xFFFFFF);
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

} // namespace

int main()
{
    const TestArchive plain = MakeArchive(false);
    const std::string library = WriteLibrary("plain", plain, {});
    int idle = 0;
    const std::vector<int16_t> loop = ModelOutput(plain.archive, kLoop, 600, idle);
    const std::vector<int16_t> note = ModelOutput(plain.archive, kNote, 400, idle);

    // A tagged length and fade: 1 s, then a fade of half a second, rendered in blocks that don't divide a frame.
    const std::vector<int16_t> tagged =
        RenderAll(WriteMini("tagged", library, kLoop, {{"length", "0:01"}, {"fade", "0.5"}}), 1001);
    Check(tagged.size() == 2 * 72000, "the tagged set plays for 1.5 s");
    Check(tagged.size() >= 2 * 48000 && std::equal(tagged.begin(), tagged.begin() + 2 * 48000, loop.begin()),
          "the tagged set plays the model's output");
    bool faded = tagged.size() == 2 * 72000;
    for (std::size_t i = 0; faded && i < 2 * 24000 && 2 * 48000 + i < loop.size(); ++i)
    {
        const double gain = 1.0 - static_cast<double>(i / 2) / 24000.0;
        faded = tagged[2 * 48000 + i] == static_cast<int16_t>(std::round(loop[2 * 48000 + i] * gain));
    }
    Check(faded, "the fade falls linearly over its half second");

    // Without a length, the sound ends half a second after the model goes idle and its output falls silent.
    const std::vector<int16_t> untagged = RenderAll(WriteMini("untagged", library, kNote, {}));
    Check(idle > 0 && untagged.size() == 2 * (static_cast<std::size_t>(idle) * kFrame + 24000),
          "the set without a length ends half a second after its sound");
    Check(note.size() >= untagged.size() && std::equal(untagged.begin(), untagged.end(), note.begin()),
          "the set without a length plays the model's output");

    // A seek forward and a seek back land where the straight render does.
    {
        hsf::Playback playback;
        std::vector<int16_t> buffer(2 * 1000);
        const bool opened = playback.Open("sets/tagged.minihsf", kReader) && playback.Start();
        const bool forward = opened && playback.Seek(30000) && playback.Render(buffer.data(), 1000) == 1000 &&
                             std::equal(buffer.begin(), buffer.end(), tagged.begin() + 2 * 30000);
        const bool back = opened && playback.Seek(10000) && playback.Position() == 10000 &&
                          playback.Render(buffer.data(), 1000) == 1000 &&
                          std::equal(buffer.begin(), buffer.end(), tagged.begin() + 2 * 10000);
        Check(forward && back, "seeks land where a straight render does");
    }

    // The volume tag scales every sample.
    const std::vector<int16_t> quiet = RenderAll(WriteMini("quiet", library, kNote, {{"volume", "0.5"}}));
    bool scaled = quiet.size() == untagged.size();
    for (std::size_t i = 0; scaled && i < quiet.size(); ++i)
    {
        scaled = quiet[i] == static_cast<int16_t>(std::round(untagged[i] * 0.5));
    }
    Check(scaled, "the volume tag halves every sample");

    // A wave archive that only a group file outside the archive holds is found there.
    const TestArchive grouped = MakeArchive(true);
    const std::string grouped_library = WriteLibrary("grouped", grouped, {});
    Check(RenderAll(WriteMini("grouped", grouped_library, kNote, {})) == untagged,
          "a set whose waves are in a group file plays the same");

    // A reverb on aux bus A plays as the model plays it, and its tail runs on after the note before the sound ends.
    {
        const hsf::audio::I3dl2ReverbParameterSet kReverb{-1000.0f, -1000.0f, 2.0f,  0.8f, -700.0f, 0.005f,
                                                          -500.0f,  0.04f,    50.0f, 3.0f, 6000.0f, 0.25f};
        hsf::ArchiveDescriptor descriptor;
        hsf::AuxEffect effect;
        effect.parameters = {kReverb.room_gain,
                             kReverb.room_hf_gain,
                             kReverb.late_reverb_decay_time,
                             kReverb.late_reverb_hf_decay_ratio,
                             kReverb.reflections_gain,
                             kReverb.reflections_delay,
                             kReverb.reverb_gain,
                             kReverb.late_reverb_delay,
                             kReverb.late_reverb_diffusion,
                             kReverb.late_reverb_density,
                             kReverb.hf_reference,
                             kReverb.dry_gain};
        descriptor.effects = {effect};
        const std::string reverb_library = WriteLibrary("reverb", plain, descriptor);
        int dry_idle = 0;
        int wet_idle = 0;
        ModelOutput(plain.archive, kSent, 2000, dry_idle);
        const std::vector<int16_t> wet = ModelOutput(plain.archive, kSent, 2000, wet_idle, &kReverb);
        const std::vector<int16_t> played = RenderAll(WriteMini("reverb", reverb_library, kSent, {}));
        Check(wet_idle > dry_idle + 20 && played.size() == 2 * (static_cast<std::size_t>(wet_idle) * kFrame + 24000) &&
                  std::equal(played.begin(), played.end() - 2 * 24000, wet.begin()),
              "a set with a reverb plays the model's output, and its tail");
    }

    // Sets that can't play: a group file the set doesn't hold, a sound the archive doesn't have, an item that isn't a
    // sound, and a descriptor's voice count beyond the most a player has.
    hsf::ArchiveDescriptor missing_group;
    missing_group.archive_path = "content/sound/test.bfsar";
    missing_group.group_paths = {"content/sound/GROUP_ELSEWHERE.bfgrp"};
    hsf::ProgramBuilder b;
    b.AddFile(missing_group.archive_path, grouped.archive);
    files["sets/missing.hsflib"] = hsf::WritePsf(hsf::kPsfVersionHsf, missing_group.Serialize(), b.Data(), {});
    hsf::ArchiveDescriptor many;
    many.voice_count = 97;
    const std::string many_library = WriteLibrary("many", plain, many);
    const std::map<std::string, std::string> refusals = {
        {WriteMini("missing", "sets/missing.hsflib", kNote, {}), "the set has no file content/sound/GROUP_ELSEWHERE"},
        {WriteMini("absent", library, 0x01000005, {}), "isn't a sequence"},
        {WriteMini("bank", library, 0x05000000, {}), "isn't a sequence"},
        {WriteMini("many", many_library, kNote, {}), "voice count is 97"}};
    for (const auto& [path, error] : refusals)
    {
        hsf::Playback playback;
        const bool refused = !(playback.Open(path, kReader) && playback.Start());
        Check(refused && playback.Error().find(error) != std::string::npos,
              path + " is refused for its reason: " + playback.Error());
    }

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    return 0;
}
