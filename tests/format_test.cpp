// SPDX-License-Identifier: MIT

// The HSF file format code (src/hsf/format.*): the version byte, the archive descriptor, and _lib loading with HSF's
// traits. xsflib's psf_test checks the PSF container, tags and times, and its psf_streaming_test the program chunks.

#include "hsf/format.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <map>
#include <string>
#include <string_view>
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
    type.effects[0].type = 4;
    ArchiveDescriptor count = v1;
    count.effects[0].parameters.pop_back();
    ArchiveDescriptor infinite = v1;
    infinite.effects[1].parameters[3] = std::numeric_limits<float>::infinity();

    XSFLIB_CHECK(bus.Check().has_value());
    XSFLIB_CHECK(type.Check().has_value());
    XSFLIB_CHECK(count.Check().has_value());
    XSFLIB_CHECK(infinite.Check().has_value());
}

// A delay and a light limiter (types 2 and 3) of 8 parameters each make the round trip. A player accepts them with
// times of whole microseconds from 0 to 10 s, other parameters that are finite, and a channel mode of 0 to 3. It
// refuses any other.
void TestRendererEffects()
{
    hsf::AuxEffect delay;
    delay.type = hsf::AuxEffect::kDelay;
    delay.parameters = {2000000.0f, 250000.0f, 0.5f, 0.2f, 0.0f, 0.4f, 0.3f, 1.0f};
    hsf::AuxEffect limiter;
    limiter.bus = 2;
    limiter.type = hsf::AuxEffect::kLightLimiter;
    limiter.parameters = {1000.0f, 0.0f, 50000.0f, 10000000.0f, 0.3f, 1.5f, 1.0f, 3.0f};
    ArchiveDescriptor d;
    d.effects = {delay, Reverb(1), limiter};

    const auto back = ArchiveDescriptor::Parse(d.Serialize());

    XSFLIB_CHECK(!d.Check());
    XSFLIB_CHECK(back && back->effects.size() == 3 && back->effects[0].type == hsf::AuxEffect::kDelay &&
                 back->effects[0].parameters == delay.parameters && back->effects[2].bus == 2 &&
                 back->effects[2].type == hsf::AuxEffect::kLightLimiter &&
                 back->effects[2].parameters == limiter.parameters);

    // Each change makes the descriptor one that a player refuses: a time below 0, past 10 s or between microseconds, a
    // number that isn't finite, a channel mode past 3 or between modes, and a parameter too few. A light limiter's
    // fourth parameter is a time, and a delay's third isn't.
    struct Change
    {
        std::size_t effect;
        std::size_t parameter;
        float value;
    };
    const Change kRefused[] = {{0, 0, -1.0f}, {0, 1, 10000001.0f},
                               {0, 1, 0.5f},  {0, 4, std::numeric_limits<float>::quiet_NaN()},
                               {2, 3, 2.5f},  {2, 6, std::numeric_limits<float>::infinity()},
                               {0, 7, 4.0f},  {2, 7, 1.5f}};
    for (const Change& change : kRefused)
    {
        ArchiveDescriptor refused = d;
        refused.effects[change.effect].parameters[change.parameter] = change.value;
        Check(refused.Check().has_value(), "parameter " + std::to_string(change.parameter) + " of effect " +
                                               std::to_string(change.effect) + " makes the descriptor refused");
    }

    ArchiveDescriptor fraction = d;
    fraction.effects[0].parameters[2] = 0.125f;
    ArchiveDescriptor short_list = d;
    short_list.effects[2].parameters.pop_back();

    XSFLIB_CHECK(!fraction.Check());
    XSFLIB_CHECK(short_list.Check() &&
                 short_list.Check()->find("a light limiter has 7 parameters") != std::string::npos);
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
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library = 4; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.dsp_level = 0x10000; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.music_volume = -1.0f; }));
    XSFLIB_CHECK(
        refused([](ArchiveDescriptor& o) { o.jaudio.category_volumes[15] = std::numeric_limits<float>::quiet_NaN(); }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.fx_lines[2].subframes = 129; }));
    XSFLIB_CHECK(!refused([](ArchiveDescriptor& o) { o.jaudio.fx_lines[2].subframes = 128; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.sound_volumes[1].volume = 256; }));
}

// Library 2: version 2's layout, then the effect sequence, the conductor archive, the pieces of music and the sounds
// that start after draws. They make the round trip. A player that supports library 1 alone reads as far as the sounds'
// volumes. A version 2 player refuses the settings that library 2 doesn't have, and pieces that it can't play.
void TestJaudioLibrary2()
{
    hsf::JaudioSettings j;
    j.library = hsf::JaudioSettings::kPikmin2Library;
    j.dsp_level = 15974;
    j.library2.effect_sequence = 0x2c;
    j.library2.effect_sequence_volume = 120;
    j.library2.conductor_archive = "AudioRes/cnd.arc";
    hsf::JaudioPiece jump;
    jump.file = 0x31;
    jump.kind = 2;
    jump.volume = 40;
    jump.kept_levels = 0;
    jump.beat_interval = 30;
    jump.muted_tracks = 0xff80;
    jump.start_point = 9;
    hsf::JaudioPiece conducted;
    conducted.file = 0x102;
    conducted.kind = hsf::JaudioPiece::kConducted;
    conducted.volume = 127;
    conducted.conductor = "c_00_1.cnd";
    j.library2.pieces = {jump, conducted};
    j.library2.draws = {{0x4805, 3}};

    ArchiveDescriptor d = ArchiveDescriptor::Jaudio(j);
    d.archive_path = "AudioRes/start.aaf";
    d.group_paths = {"AudioRes/Seqs/seq.arc", "AudioRes/Banks"};
    const std::vector<uint8_t> raw = d.Serialize();
    const auto back = ArchiveDescriptor::Parse(raw);
    XSFLIB_CHECK(raw.size() % 4 == 0);
    XSFLIB_CHECK(back && back->jaudio == j && back->group_paths == d.group_paths);
    XSFLIB_CHECK(!d.Check());

    // Library 2's fields follow library 1's. The archive's path ends with zeros to a multiple of 4. Each piece takes 12
    // bytes before its conductor's name, and its name ends the same way. Each sound with draws takes 8 bytes.
    ArchiveDescriptor first = d;
    first.jaudio.library = hsf::JaudioSettings::kGalaxyLibrary;
    const std::size_t at = first.Serialize().size();
    std::vector<uint8_t> fields;
    const auto put = [&fields](std::initializer_list<uint8_t> bytes)
    {
        fields.insert(fields.end(), bytes);
    };
    const auto put_text = [&fields](std::string_view text)
    {
        fields.insert(fields.end(), text.begin(), text.end());
    };
    put({0x2c, 0, 120, 0}); // the effect sequence and its volume
    put({16, 0});
    put_text("AudioRes/cnd.arc");
    put({0, 0});
    put({2, 0, 0, 0});                                     // the pieces' count
    put({0x31, 0, 2, 40, 0, 0, 30, 0, 0x80, 0xff, 9, 0});  // the jump piece
    put({0, 0, 0, 0});                                     // its conductor's empty name, and two zeros
    put({0x02, 0x01, 3, 127, 0x1f, 0, 60, 0, 0, 0, 0, 0}); // the conducted piece
    put({10, 0});
    put_text("c_00_1.cnd");
    put({1, 0, 0, 0});       // the count of sounds with draws
    put({0x05, 0x48, 0, 0}); // the sound
    put({3, 0, 0, 0});       // its draws
    XSFLIB_CHECK(raw.size() == at + fields.size() &&
                 std::equal(fields.begin(), fields.end(), raw.begin() + static_cast<std::ptrdiff_t>(at)));

    // A descriptor that ends inside library 2's fields.
    for (const std::size_t cut : {std::size_t{2}, std::size_t{10}, std::size_t{30}, std::size_t{50}, raw.size() - at})
    {
        const std::vector<uint8_t> shorter(raw.begin(), raw.end() - static_cast<std::ptrdiff_t>(cut));
        XSFLIB_CHECK(!ArchiveDescriptor::Parse(shorter).has_value());
    }

    // A player that supports library 1 alone reads the descriptor up to the sounds' volumes, and then refuses the
    // library by its number. The number is 200 bytes before library 2's fields: version 2's fields take 200 bytes
    // without track states or sounds' volumes.
    std::vector<uint8_t> as_first = raw;
    as_first[at - 200] = 1;
    const auto read_first = ArchiveDescriptor::Parse(as_first);
    XSFLIB_CHECK(read_first && read_first->jaudio.library == 1 && read_first->jaudio.library2 == hsf::JaudioLibrary2{});

    const auto refused = [&d](auto change)
    {
        ArchiveDescriptor odd = d;
        change(odd);
        return odd.Check().has_value();
    };
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.category_volumes[2] = 1.2f; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.fx_lines[1].flags = 1; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.track_states.push_back({}); }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.sound_volumes.push_back({}); }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library2.effect_sequence_volume = 128; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library2.pieces[0].kind = 4; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library2.pieces[0].volume = 128; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library2.pieces[0].kept_levels = 0x20; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library2.pieces[0].conductor = "x.cnd"; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library2.pieces[1].conductor.clear(); }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library2.conductor_archive.clear(); }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library2.pieces.resize(0x10001); }));
    XSFLIB_CHECK(!refused([](ArchiveDescriptor& o) { o.jaudio.library2.pieces.resize(1); }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library2.draws[0].draws = 0; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library2.draws[0].draws = 0x10000; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library2.draws.push_back({0x4805, 1}); }));
    XSFLIB_CHECK(!refused([](ArchiveDescriptor& o) { o.jaudio.library2.draws[0].draws = 0xffff; }));
    XSFLIB_CHECK(!refused([](ArchiveDescriptor& o) { o.jaudio.library2.draws.push_back({0x4806, 1}); }));
    XSFLIB_CHECK(d.jaudio.library2.DrawsBefore(0x4805) == 3 && d.jaudio.library2.DrawsBefore(0x4806) == 0);
}

// Library 3: version 2's layout, then the sequence index, the start-up files and the levels, the wave scenes that the
// game loads, the pieces of music, the pieces of the cutscenes' music and the effects. They make the round trip. A
// version 2 player refuses the settings that library 3 doesn't have, and values past their ranges.
void TestJaudioLibrary3()
{
    hsf::JaudioSettings j;
    j.library = hsf::JaudioSettings::kPikminLibrary;
    j.dsp_level = 0x3000;
    j.fx_lines[0].flags = 1;
    j.fx_lines[0].subframes = 50;
    j.fx_lines[0].buses = {0x9a0, 0};
    j.fx_lines[0].levels = {0x6000, 0};
    hsf::JaudioLibrary3& l = j.library3;
    l.sequence_index = "exefs/main/pikiseq.barc";
    l.startup_files = {0, 1, 13, 2};
    l.effect_level = 20000;
    l.cutscene_level = 8000;
    l.music_volume = 0.8f;
    l.loads = {{{0, 0}, 1}, {{0, 3}, 0}, {{14, 0}, 1}};
    hsf::JacPiece piece;
    piece.file = 4;
    piece.playing_tracks = 0x103c;
    piece.volume = 0.5f;
    piece.scene = {4, 0};
    l.pieces = {piece};
    l.parts = {{8, {13, 6}}};
    hsf::JacEffect ship;
    ship.kind = hsf::JacEffect::kEvent;
    ship.type = 7;
    ship.value = 0x5109;
    ship.first = 0xf10d;
    hsf::JacEffect footstep;
    footstep.kind = hsf::JacEffect::kWalk;
    footstep.value = 3;
    hsf::JacEffect cutscene;
    cutscene.kind = hsf::JacEffect::kCutscene;
    cutscene.type = hsf::JacEffect::kCutscenePart;
    cutscene.value = 2;
    cutscene.first = 0x10;
    cutscene.finish = 1097;
    l.effects = {ship, footstep, cutscene};

    ArchiveDescriptor d = ArchiveDescriptor::Jaudio(j);
    d.archive_path = "dataDir/SndData/Banks/pikibank.bx";
    d.group_paths = {"dataDir/SndData/Seqs/pikiseq.arc", "dataDir/SndData/Banks"};
    const std::vector<uint8_t> raw = d.Serialize();
    const auto back = ArchiveDescriptor::Parse(raw);
    XSFLIB_CHECK(raw.size() % 4 == 0);
    XSFLIB_CHECK(back && back->jaudio == j && back->group_paths == d.group_paths);
    XSFLIB_CHECK(!d.Check());

    // Library 3's fields follow library 1's. The index's path ends with zeros to a multiple of 4. Each load, part and
    // effect takes 8 bytes, and each piece 12.
    ArchiveDescriptor first = d;
    first.jaudio.library = hsf::JaudioSettings::kGalaxyLibrary;
    const std::size_t at = first.Serialize().size();
    std::vector<uint8_t> fields;
    const auto put = [&fields](std::initializer_list<uint8_t> bytes)
    {
        fields.insert(fields.end(), bytes);
    };
    put({23, 0});
    const std::string_view index = "exefs/main/pikiseq.barc";
    fields.insert(fields.end(), index.begin(), index.end());
    put({0, 0, 0});                                     // zeros to a multiple of 4
    put({0, 0, 1, 0, 13, 0, 2, 0});                     // the start-up files
    put({0x20, 0x4e, 0x40, 0x1f});                      // the levels
    put({0xcd, 0xcc, 0x4c, 0x3f});                      // the music's volume
    put({3, 0, 0, 0});                                  // the loads' count
    put({0, 0, 0, 0, 1, 0, 0, 0});                      // set (0, 0)
    put({0, 0, 3, 0, 0, 0, 0, 0});                      // load (0, 3)
    put({14, 0, 0, 0, 1, 0, 0, 0});                     // set (14, 0)
    put({1, 0, 0, 0});                                  // the pieces' count
    put({4, 0, 0x3c, 0x10, 0, 0, 0, 0x3f, 4, 0, 0, 0}); // the piece
    put({1, 0, 0, 0});                                  // the parts' count
    put({8, 0, 0, 0, 13, 0, 6, 0});                     // the part
    put({3, 0, 0, 0});                                  // the effects' count
    put({3, 7, 0x09, 0x51, 0x0d, 0xf1, 0, 0});          // the ship's action
    put({6, 0, 3, 0, 0xff, 0xff, 0, 0});                // the footstep
    put({7, 1, 2, 0, 0x10, 0, 0x49, 0x04});             // the cutscene's sound and its finish
    XSFLIB_CHECK(raw.size() == at + fields.size() &&
                 std::equal(fields.begin(), fields.end(), raw.begin() + static_cast<std::ptrdiff_t>(at)));

    // A descriptor that ends inside library 3's fields.
    for (const std::size_t cut :
         {std::size_t{2}, std::size_t{8}, std::size_t{20}, std::size_t{40}, std::size_t{70}, raw.size() - at})
    {
        const std::vector<uint8_t> shorter(raw.begin(), raw.end() - static_cast<std::ptrdiff_t>(cut));
        XSFLIB_CHECK(!ArchiveDescriptor::Parse(shorter).has_value());
    }

    const auto refused = [&d](auto change)
    {
        ArchiveDescriptor odd = d;
        change(odd);
        return odd.Check().has_value();
    };
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.music_volume = 0.5f; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.effect_volume = 0.5f; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.category_volumes[2] = 1.2f; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.track_states.push_back({}); }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.sound_volumes.push_back({}); }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library3.sequence_index.clear(); }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library3.effect_level = 0x8000; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library3.cutscene_level = 0x8000; }));
    XSFLIB_CHECK(!refused([](ArchiveDescriptor& o) { o.jaudio.library3.effect_level = 0x7fff; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library3.music_volume = -1.0f; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o)
                         { o.jaudio.library3.pieces[0].volume = std::numeric_limits<float>::infinity(); }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library3.loads[1].set = 2; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library3.pieces.resize(0x10001); }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library3.parts.resize(0x10001); }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library3.effects.resize(0x10001); }));
    XSFLIB_CHECK(!refused([](ArchiveDescriptor& o) { o.jaudio.library3.effects.resize(0x10000); }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library3.effects[1].kind = 8; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library3.effects[0].type = 0; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library3.effects[0].type = 8; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library3.effects[1].type = 1; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library3.effects[2].type = 2; }));
    XSFLIB_CHECK(!refused([](ArchiveDescriptor& o) { o.jaudio.library3.effects[0].type = 1; }));
    XSFLIB_CHECK(!refused([](ArchiveDescriptor& o) { o.jaudio.library3.effects[2].type = 0; }));
    XSFLIB_CHECK(refused([](ArchiveDescriptor& o) { o.jaudio.library3.effects[1].finish = 1; }));
    XSFLIB_CHECK(!refused([](ArchiveDescriptor& o) { o.jaudio.library3.effects[2].finish = 0; }));

    // A player refuses a library past 3, and names the libraries that it supports.
    ArchiveDescriptor later = d;
    later.jaudio.library = 4;
    const std::optional<std::string> refusal = later.Check();
    XSFLIB_CHECK(refusal && refusal->find("library is 4; this player supports libraries 1 to 3") != std::string::npos);
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
    TestRendererEffects();
    TestJaudioDescriptor();
    TestJaudioLibrary2();
    TestJaudioLibrary3();
    TestLoadSet();

    return Summary("format");
}
