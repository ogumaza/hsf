// SPDX-License-Identifier: MIT

// The HSF file format code (src/hsf/format.*): the version byte, the archive descriptor, and _lib loading with HSF's
// traits. xsflib's psf_test checks the PSF container, tags and times, and its psf_streaming_test the program chunks.

#include "hsf/format.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "xsflib/psf/descriptor.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/sets.h"

using namespace xsflib::testing;

namespace
{

using hsf::ArchiveDescriptor;
using hsf::FileReader;
using hsf::LoadedSet;
using hsf::ProgramBuilder;
using hsf::WritePsf;

constexpr uint8_t kVersion = hsf::kPsfVersionHsf;

// The error LoadSet gives for `path`, or an empty string if the set loads.
std::string LoadError(const std::string& path, const FileReader& reader)
{
    LoadedSet set;
    const auto err = hsf::LoadSet(path, reader, set);
    return err ? *err : std::string();
}

// An I3DL2 reverb on aux bus `bus`, with parameters 1 to 12.
hsf::AuxEffect Reverb(uint32_t bus)
{
    hsf::AuxEffect effect;
    effect.bus = bus;
    for (int i = 1; i <= 12; ++i)
    {
        effect.parameters.push_back(static_cast<float>(i) * 0.5f);
    }

    return effect;
}

// docs/hsf.md gives HSF the PSF version byte 0x29. An HSF file starts with "PSF" and that byte.
void TestVersion()
{
    const std::vector<uint8_t> file = WritePsf(kVersion, ArchiveDescriptor().Serialize(), {}, {});

    XSFLIB_CHECK(file.size() > 16 && std::memcmp(file.data(), "PSF", 3) == 0 && file[3] == 0x29);
}

void TestArchiveDescriptor()
{
    ArchiveDescriptor d;
    d.version = 3;
    d.flags = 1;
    d.output_mode = 2;
    d.voice_count = 72;
    d.sdk_version = 100401;
    d.effects = {Reverb(0), Reverb(2)};
    d.effects[1].parameters[11] = -0.0f;
    d.archive_path = "Archive/Sound.bea/Audio/b.bfsar";
    d.group_paths = {"Audio/groups/GROUP_A.bfgrp", "Audio/groups/GROUP_BB.bfgrp"};

    const auto raw = d.Serialize();
    const auto back = ArchiveDescriptor::Parse(raw);

    XSFLIB_CHECK(raw.size() % 4 == 0);
    XSFLIB_CHECK(std::memcmp(raw.data(), "HSFA", 4) == 0);
    XSFLIB_CHECK(back && back->version == d.version && back->flags == d.flags && back->output_mode == d.output_mode &&
                 back->voice_count == d.voice_count && back->sdk_version == d.sdk_version &&
                 back->archive_path == d.archive_path && back->group_paths == d.group_paths);
    XSFLIB_CHECK(back && back->effects.size() == 2 && back->effects[0].bus == 0 && back->effects[1].bus == 2 &&
                 back->effects[0].type == hsf::AuxEffect::kI3dl2Reverb &&
                 back->effects[0].parameters == d.effects[0].parameters &&
                 std::signbit(back->effects[1].parameters[11]));
    XSFLIB_CHECK(!ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.begin() + 40)).has_value());
    XSFLIB_CHECK(!ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.begin() + 100)).has_value());

    // A player accepts version 1 with no flags, in stereo, with 1 to 96 voices, and I3DL2 reverbs of 12 finite
    // parameters on the aux buses 0 to 2. It also accepts version 2 (TestJaudioDescriptor).
    XSFLIB_CHECK(d.Check() && d.Check()->find("version 3") != std::string::npos);

    ArchiveDescriptor v1 = d;
    v1.version = 1;
    v1.flags = 0;
    v1.output_mode = hsf::kOutputStereo;

    XSFLIB_CHECK(!v1.Check());
    XSFLIB_CHECK(!ArchiveDescriptor{}.Check());

    for (const uint32_t voices : {0u, 97u})
    {
        ArchiveDescriptor odd = v1;
        odd.voice_count = voices;
        XSFLIB_CHECK(odd.Check().has_value());
    }

    ArchiveDescriptor bus = v1;
    bus.effects[0].bus = 3;
    ArchiveDescriptor type = v1;
    type.effects[0].type = 2;
    ArchiveDescriptor count = v1;
    count.effects[0].parameters.pop_back();
    ArchiveDescriptor infinite = v1;
    infinite.effects[1].parameters[3] = std::numeric_limits<float>::infinity();

    XSFLIB_CHECK(bus.Check().has_value());
    XSFLIB_CHECK(type.Check().has_value());
    XSFLIB_CHECK(count.Check().has_value());
    XSFLIB_CHECK(infinite.Check().has_value());
}

// Version 2: the version 1 layout, then JAudio's settings after the paths. The settings, the track states and the
// sounds' volumes make the round trip. A version 1 player reads the version 1 part and refuses the version. A version 2
// player refuses descriptors that its model can't play.
void TestJaudioDescriptor()
{
    hsf::JaudioSettings j;
    j.dsp_level = 0x3999;
    j.music_volume = 1.2f;
    j.effect_volume = 1.25f;
    j.category_volumes[3] = 1.3f;
    j.category_volumes[13] = 1.5f;
    j.fx_lines[0] = {1, 64, {0x0d00, 0x0d60}, {0x7000, 0}, {0, 0x1000, 0, 0, 0, 0, 0x3fff, -2}};
    j.fx_lines[3] = {3, 7, {0x0ca0, 0x0f40}, {0x8000, 0x10}, {-0x8000, 1, 2, 3, 4, 5, 6, 0x7fff}};
    j.track_states = {{0x01000005, {2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
                      {0x01000038, {2, 8, 15, 1, 7, 0, 3, 4, 5, 6, 9, 10, 11, 12, 13, 14}}};
    j.sound_volumes = {{0x01000002, 0}, {0x0100000f, 72}, {0x01020304, 255}};

    ArchiveDescriptor d = ArchiveDescriptor::Jaudio(j);
    d.archive_path = "AudioRes/start.szs";
    d.group_paths = {"AudioRes/Seqs/JaiSeq.arc", "AudioRes/Waves"};

    const std::vector<uint8_t> raw = d.Serialize();
    const auto back = ArchiveDescriptor::Parse(raw);

    XSFLIB_CHECK(raw.size() % 4 == 0);
    XSFLIB_CHECK(back && back->version == 2 && back->voice_count == 64 && back->effects.empty() &&
                 back->archive_path == d.archive_path && back->group_paths == d.group_paths);
    XSFLIB_CHECK(back && back->jaudio == j);
    XSFLIB_CHECK(!d.Check());

    // The states go 2 to a byte, track 0 in the high nibble of the first. The volumes follow, each after its sound.
    const std::size_t volumes = raw.size() - 4 - 8 * j.sound_volumes.size();
    const std::size_t states = volumes - 8;
    XSFLIB_CHECK(raw[states - 12] == 0x20 && raw[states - 11] == 0x00 && raw[states] == 0x28 &&
                 raw[states + 1] == 0xf1 && raw[states + 7] == 0xde);
    XSFLIB_CHECK(raw[volumes] == 3 && raw[volumes + 4] == 0x02 && raw[volumes + 7] == 0x01 && raw[volumes + 8] == 0 &&
                 raw[volumes + 16] == 72 && raw[volumes + 20] == 0x04 && raw[volumes + 22] == 0x02 &&
                 raw[volumes + 24] == 255 && raw[volumes + 25] == 0);

    // A descriptor that ends inside the settings, the track states or the volumes.
    XSFLIB_CHECK(!ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.end() - 1)).has_value());
    XSFLIB_CHECK(!ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.end() - 30)).has_value());
    XSFLIB_CHECK(!ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.end() - 40)).has_value());
    XSFLIB_CHECK(!ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.end() - 100)).has_value());

    // A version 1 player reads a version 2 descriptor as a version 1 descriptor: the start, no SDK version, no effects
    // and the paths. Its check of the start then refuses the version.
    std::vector<uint8_t> as_v1 = raw;
    as_v1[4] = 1;
    const auto v1 = ArchiveDescriptor::Parse(as_v1);
    XSFLIB_CHECK(v1 && v1->sdk_version == 0 && v1->effects.empty() && v1->archive_path == d.archive_path &&
                 v1->group_paths == d.group_paths);

    ArchiveDescriptor old_view = *v1;
    old_view.version = 2;
    const auto refusal = xsflib::psf::CheckArchiveStart(old_view, ArchiveDescriptor::kDefaultVoices);
    XSFLIB_CHECK(refusal && *refusal == "archive descriptor version 2; this player supports version 1");

    // What a version 2 player refuses.
    const auto refused = [&d](auto change)
    {
        ArchiveDescriptor odd = d;
        change(odd);
        return odd.Check().has_value();
    };
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.flags = 1; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.output_mode = 2; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.voice_count = 96; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.effects = {Reverb(0)}; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.group_paths.pop_back(); }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library = 2; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.dsp_level = 0x10000; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.music_volume = -1.0f; }));
    XSFLIB_CHECK(
        refused([](ArchiveDescriptor& o) { o.jaudio.category_volumes[15] = std::numeric_limits<float>::quiet_NaN(); }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.fx_lines[2].subframes = 129; }));
    XSFLIB_CHECK(!refused([](ArchiveDescriptor& o) { o.jaudio.fx_lines[2].subframes = 128; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.sound_volumes[1].volume = 256; }));
}

// A set with HSF's traits: the descriptor, the sound and the files load, and HSF's refusals give HSF's messages.
// xsflib's psf_set_test and psf_streaming_test check the loader's other cases.
void TestLoadSet()
{
    // Each file of the set selects a sound. LoadSet reads lib.hsflib (the _lib), x.minihsf and extra.minihsf (the
    // _lib2) in that order. The last file's sound takes effect.
    std::map<std::string, std::vector<uint8_t>> fs;
    const ArchiveDescriptor archive;
    {
        ProgramBuilder b;
        b.AddFile("f", std::vector<uint8_t>{1});
        b.AddSound(0x01000001);
        fs["dir/lib.hsflib"] = WritePsf(kVersion, archive.Serialize(), b.Data(), {{"game", "G"}});
    }
    {
        ProgramBuilder b;
        b.AddFile("f", std::vector<uint8_t>{2});
        b.AddSound(0x01000002);
        fs["dir/x.minihsf"] =
            WritePsf(kVersion, {}, b.Data(), {{"_lib", "lib.hsflib"}, {"_lib2", "extra.minihsf"}, {"title", "X"}});
    }
    {
        ProgramBuilder b;
        b.AddSound(0x01000003);
        fs["dir/extra.minihsf"] = WritePsf(kVersion, {}, b.Data(), {{"title", "extra"}});
    }
    const FileReader reader = MapReader(fs);

    LoadedSet set;
    const auto err = hsf::LoadSet("dir/x.minihsf", reader, set);

    XSFLIB_CHECK(!err && set.archive.has_value() && set.sound == 0x01000003u);
    XSFLIB_CHECK(set.File("f") && *set.File("f") == std::vector<uint8_t>{2} && set.tags["title"] == "X");

    // A wrong PSF version, a set without a descriptor or a sound, and a descriptor the player can't play.
    ArchiveDescriptor v3;
    v3.version = 3;
    fs["dir/z.miniwsf"] = WritePsf(0x27, {}, {}, {{"_lib", "lib.hsflib"}});
    fs["dir/w.minihsf"] = WritePsf(kVersion, {}, {}, {{"title", "w"}});
    fs["dir/nosound.hsflib"] = WritePsf(kVersion, archive.Serialize(), {}, {});
    fs["dir/v3.hsflib"] = WritePsf(kVersion, v3.Serialize(), {}, {});

    XSFLIB_CHECK(LoadError("dir/z.miniwsf", reader).find("not an HSF file") != std::string::npos);
    XSFLIB_CHECK(LoadError("dir/w.minihsf", reader).starts_with("no archive descriptor"));
    XSFLIB_CHECK(LoadError("dir/nosound.hsflib", reader).starts_with("no sound selected"));
    XSFLIB_CHECK(LoadError("dir/v3.hsflib", reader).find("version 3") != std::string::npos);

    // A version 2 set loads with its settings.
    ArchiveDescriptor jaudio = ArchiveDescriptor::Jaudio({});
    jaudio.archive_path = "a";
    jaudio.group_paths = {"s", "w"};
    {
        ProgramBuilder b;
        b.AddSound(0x01000004);
        fs["dir/j.hsf"] = WritePsf(kVersion, jaudio.Serialize(), b.Data(), {});
    }
    LoadedSet j;
    XSFLIB_CHECK(!hsf::LoadSet("dir/j.hsf", reader, j) && j.archive && j.archive->version == 2 &&
                 j.archive->group_paths == jaudio.group_paths && j.sound == 0x01000004u);
}

} // namespace

int main()
{
    TestVersion();
    TestArchiveDescriptor();
    TestJaudioDescriptor();
    TestLoadSet();

    return Summary("format");
}
