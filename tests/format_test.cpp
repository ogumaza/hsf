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
    d.version = 2;
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

    // A player only accepts version 1, with no flags, in stereo, with 1 to 96 voices, and I3DL2 reverbs of 12 finite
    // parameters on the aux buses 0 to 2.
    XSFLIB_CHECK(d.Check().has_value());

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

    // A wrong PSF version, a set without a descriptor or a sound, and a descriptor a version 1 player can't play.
    ArchiveDescriptor v2;
    v2.version = 2;
    fs["dir/z.miniwsf"] = WritePsf(0x27, {}, {}, {{"_lib", "lib.hsflib"}});
    fs["dir/w.minihsf"] = WritePsf(kVersion, {}, {}, {{"title", "w"}});
    fs["dir/nosound.hsflib"] = WritePsf(kVersion, archive.Serialize(), {}, {});
    fs["dir/v2.hsflib"] = WritePsf(kVersion, v2.Serialize(), {}, {});

    XSFLIB_CHECK(LoadError("dir/z.miniwsf", reader).find("not an HSF file") != std::string::npos);
    XSFLIB_CHECK(LoadError("dir/w.minihsf", reader).starts_with("no archive descriptor"));
    XSFLIB_CHECK(LoadError("dir/nosound.hsflib", reader).starts_with("no sound selected"));
    XSFLIB_CHECK(LoadError("dir/v2.hsflib", reader).find("version 2") != std::string::npos);
}

} // namespace

int main()
{
    TestVersion();
    TestArchiveDescriptor();
    TestLoadSet();

    return Summary("format");
}
