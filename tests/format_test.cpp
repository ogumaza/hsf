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
#include <optional>
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
    type.effects[0].type = 5;
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

// A reverb (type 4) of 12 parameters makes the round trip. A player accepts it with a pre-delay of whole microseconds
// from 0 to 300 ms, a decay time of whole microseconds from 100 ms to 20 s, modes of 0 to 4, a channel mode of 0 to 3
// and other parameters that are finite. It refuses any other.
void TestReverbEffect()
{
    hsf::AuxEffect reverb;
    reverb.bus = 1;
    reverb.type = hsf::AuxEffect::kReverb;
    reverb.parameters = {2.0f, 0.7f, 15000.0f, 1.0f, 0.7f, 2500000.0f, 0.8f, 0.7f, 0.4f, 1.0f, 0.0f, 2.0f};
    ArchiveDescriptor d;
    d.effects = {Reverb(0), reverb};

    const auto back = ArchiveDescriptor::Parse(d.Serialize());

    XSFLIB_CHECK(!d.Check());
    XSFLIB_CHECK(back && back->effects.size() == 2 && back->effects[1].type == hsf::AuxEffect::kReverb &&
                 back->effects[1].bus == 1 && back->effects[1].parameters == reverb.parameters);

    // The limits of the times and the modes are accepted. Gains outside 0 to 1 are too: nn::atk holds them.
    struct Change
    {
        std::size_t parameter;
        float value;
    };

    const Change kAccepted[] = {{hsf::kReverbPredelay, 0.0f},
                                {hsf::kReverbPredelay, 300000.0f},
                                {hsf::kReverbDecay, 100000.0f},
                                {hsf::kReverbDecay, 20000000.0f},
                                {hsf::kReverbEarlyMode, 4.0f},
                                {hsf::kReverbLateMode, 0.0f},
                                {hsf::kReverbChannelMode, 3.0f},
                                {hsf::kReverbGain, 1.5f},
                                {hsf::kReverbHighFrequencyDecayRatio, 0.05f}};
    for (const Change& change : kAccepted)
    {
        ArchiveDescriptor accepted = d;
        accepted.effects[1].parameters[change.parameter] = change.value;
        Check(!accepted.Check(),
              "parameter " + std::to_string(change.parameter) + " at " + std::to_string(change.value) + " is accepted");
    }

    // Each change makes the descriptor one that a player refuses: a time past its limits or between microseconds, a
    // mode past 4 or between modes, a channel mode past 3, and a number that isn't finite.
    const Change kRefused[] = {{hsf::kReverbPredelay, -1.0f},
                               {hsf::kReverbPredelay, 300001.0f},
                               {hsf::kReverbPredelay, 0.5f},
                               {hsf::kReverbDecay, 99999.0f},
                               {hsf::kReverbDecay, 20000002.0f},
                               {hsf::kReverbEarlyMode, 5.0f},
                               {hsf::kReverbLateMode, 1.5f},
                               {hsf::kReverbLateMode, -1.0f},
                               {hsf::kReverbChannelMode, 4.0f},
                               {hsf::kReverbOutGain, std::numeric_limits<float>::infinity()},
                               {hsf::kReverbDryGain, std::numeric_limits<float>::quiet_NaN()}};
    for (const Change& change : kRefused)
    {
        ArchiveDescriptor refused = d;
        refused.effects[1].parameters[change.parameter] = change.value;
        Check(refused.Check().has_value(), "parameter " + std::to_string(change.parameter) + " at " +
                                               std::to_string(change.value) + " makes the descriptor refused");
    }

    ArchiveDescriptor short_list = d;
    short_list.effects[1].parameters.pop_back();

    XSFLIB_CHECK(short_list.Check() && short_list.Check()->find("a reverb has 11 parameters") != std::string::npos);
}

// Version 1's track volumes (flag bit 0) follow the paths. They make the round trip, and the bytes before them are
// those of the descriptor without them, but for the flags. A player accepts each sound once, with volumes that are
// finite numbers of 0 or more. It refuses other flags, and volumes without the flag. HSF 1.3.0 checked the flags with
// xsflib's check of a version 1 descriptor's start. That check refuses the flag, with the message that docs/hsf.md
// gives.
void TestTrackVolumes()
{
    ArchiveDescriptor plain;
    plain.voice_count = 72;
    plain.effects = {Reverb(0)};
    plain.archive_path = "Archive/Sound.bea/Audio/sound.bfsar";

    ArchiveDescriptor d = plain;
    d.flags = ArchiveDescriptor::kFlagTrackVolumes;
    hsf::SoundTrackVolumes first;
    first.sound = 0x010001aa;
    first.volumes[1] = 0.0f;
    first.volumes[15] = 0.25f;
    hsf::SoundTrackVolumes second;
    second.sound = 0x01000007;
    second.volumes.fill(0.0f);
    second.volumes[3] = 2.0f;
    d.track_volumes = {first, second};

    const std::vector<uint8_t> before = plain.Serialize();
    const std::vector<uint8_t> raw = d.Serialize();
    const auto back = ArchiveDescriptor::Parse(raw);

    XSFLIB_CHECK(!d.Check());
    XSFLIB_CHECK(
        raw.size() == before.size() + 4 + 2 * 68 && raw[8] == 1 &&
        std::equal(raw.begin() + 12, raw.begin() + static_cast<std::ptrdiff_t>(before.size()), before.begin() + 12));
    XSFLIB_CHECK(back && back->flags == d.flags && back->track_volumes == d.track_volumes &&
                 back->archive_path == d.archive_path && back->effects.size() == 1);
    XSFLIB_CHECK(back && back->TrackVolumesOf(0x01000007) && back->TrackVolumesOf(0x01000007)->volumes[3] == 2.0f &&
                 !back->TrackVolumesOf(0x01000008));
    XSFLIB_CHECK(!ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.end() - 4)).has_value());
    XSFLIB_CHECK(!plain.TrackVolumesOf(0x010001aa));

    // A descriptor without the flag is the same as before: nothing follows the paths.
    ArchiveDescriptor unflagged = d;
    unflagged.flags = 0;
    unflagged.track_volumes.clear();
    XSFLIB_CHECK(unflagged.Serialize() == before);

    ArchiveDescriptor other_flag = d;
    other_flag.flags = 0x80000001;
    ArchiveDescriptor no_flag = d;
    no_flag.flags = 0;
    ArchiveDescriptor twice = d;
    twice.track_volumes[1].sound = first.sound;
    ArchiveDescriptor negative = d;
    negative.track_volumes[0].volumes[2] = -0.5f;
    ArchiveDescriptor nan = d;
    nan.track_volumes[1].volumes[0] = std::numeric_limits<float>::quiet_NaN();

    XSFLIB_CHECK(other_flag.Check() && other_flag.Check()->find("version 1 has only bits 0") != std::string::npos);
    XSFLIB_CHECK(no_flag.Check() && no_flag.Check()->find("without its flag") != std::string::npos);
    XSFLIB_CHECK(twice.Check() && twice.Check()->find("more than once") != std::string::npos);
    XSFLIB_CHECK(negative.Check().has_value());
    XSFLIB_CHECK(nan.Check().has_value());

    const std::optional<std::string> old_player = xsflib::psf::CheckArchiveStart(d, ArchiveDescriptor::kDefaultVoices);
    Check(old_player == "the archive descriptor's flags are 0x1; version 1 has none",
          "HSF 1.3.0's check refuses the flag: " + old_player.value_or("nothing"));
}

// Version 1's scripted starts (flag bit 1) follow the paths, or the track volumes when the descriptor has those too.
// They make the round trip, with the track volumes before them. A player accepts each sound once, by a sound's item ID,
// with a script that starts the sound from the set's archive. Its steps come in frame order. A start starts a sound
// once, from the set's archive or a listed add-on archive, and sets nothing. A write sets a variable from 0 to 47 once
// a frame: a global one of no sound, or a sound's or a track's of a sound that an earlier step starts, on a track from
// 0 to 15 for 32 to 47. The sound starts by the end of the lead-in, and the lead-in ends at most 600 s after the last
// step. The add-on archives follow the scripts, each with its paths padded to a multiple of 4 bytes. A write of kind 2
// names a user procedure and a sound that an earlier step starts, a global variable's write too. Its kind and procedure
// take a byte each, where a start or a write of kind 1 has its kind and a 0. A player refuses other scripts, and
// scripts or add-on archives without the flag. HSF 1.3.0's check refuses the flag, with the message that docs/hsf.md
// gives.
void TestScriptedStarts()
{
    ArchiveDescriptor plain;
    plain.voice_count = 96;
    plain.archive_path = "Archive/sound~subarc_mg1801.nx.bea/audio/sounddata/subarc_mg1801/subarc_mg1801.fsst";

    using hsf::ScriptStep;
    ArchiveDescriptor d = plain;
    d.flags = ArchiveDescriptor::kFlagScripts;
    // A piece that waits for the global variables 27 and 28, with its variable 3 and track 0's variable 33 set.
    const hsf::ScriptedStart piece = {
        0x01000002,
        0,
        {ScriptStep::Write(0, 28, 18011), ScriptStep::Write(0, 27, 120), ScriptStep::Start(0, 0x01000002),
         ScriptStep::Write(0, 3, -2, 0x01000002), ScriptStep::Write(0, 33, 7, 0x01000002, 0)}};
    // A piece that starts beside another after a lead-in, with a write at a later frame.
    const hsf::ScriptedStart other = {
        0x01000005,
        400,
        {ScriptStep::Start(0, 0x01000009), ScriptStep::Write(1, 29, 200), ScriptStep::Start(2, 0x01000005),
         ScriptStep::Write(2, 26, 1), ScriptStep::Write(900, 47, -32768, 0x01000009, 15)}};
    d.scripts = {piece, other};

    const std::vector<uint8_t> before = plain.Serialize();
    const std::vector<uint8_t> raw = d.Serialize();
    const auto back = ArchiveDescriptor::Parse(raw);

    XSFLIB_CHECK(!d.Check());
    // 4 for the count, 12 for each sound, 16 for each step, and 4 for the count of add-on archives: 192 bytes.
    const std::size_t at = before.size();
    XSFLIB_CHECK(raw.size() == at + 4 + 2 * 12 + 10 * 16 + 4 && raw[8] == 2 &&
                 std::equal(raw.begin() + 12, raw.begin() + static_cast<std::ptrdiff_t>(at), before.begin() + 12));
    XSFLIB_CHECK(raw[at] == 2 && raw[at + 4] == 0x02 && raw[at + 7] == 0x01 && raw[at + 12] == 5 &&
                 raw[at + 16 + 4] == 1 && raw[at + 16 + 5] == 0 && raw[at + 16 + 6] == 28 && raw[at + 16 + 8] == 0xff &&
                 raw[at + 16 + 10] == 0x5b && raw[at + 16 + 11] == 0x46 && raw[at + 16 + 2 * 16 + 4] == 0 &&
                 raw[at + 16 + 2 * 16 + 12] == 0x02 && raw[at + 16 + 2 * 16 + 15] == 0x01);
    XSFLIB_CHECK(back && back->flags == d.flags && back->scripts == d.scripts && back->track_volumes.empty());
    XSFLIB_CHECK(back && back->ScriptOf(0x01000005) && back->ScriptOf(0x01000005)->lead_in == 400 &&
                 back->ScriptOf(0x01000005)->steps[4].value == -32768 && !back->ScriptOf(0x01000006));
    XSFLIB_CHECK(back && back->addons.empty() &&
                 !ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.end() - 4)).has_value());

    // An add-on archive for the companion to start from. The archive's path of 83 bytes and one group file's path of 9
    // take 2 + 83 + 2 + 2 + 9 bytes, padded to 100.
    ArchiveDescriptor addon = d;
    addon.addons = {
        {"Archive/sound~subarc_rc_cmn.nx.bea/audio/sounddata/subarc_rc_cmn/subarc_rc_cmn.fsst", {"G_A.bfgrp"}}};
    addon.scripts[1].steps[0].variable = 0;
    const std::vector<uint8_t> addon_raw = addon.Serialize();
    const auto addon_back = ArchiveDescriptor::Parse(addon_raw);
    XSFLIB_CHECK(addon.addons[0].archive_path.size() == 83 && !addon.Check());
    XSFLIB_CHECK(addon_raw.size() == raw.size() + 100 && addon_back && addon_back->addons == addon.addons &&
                 addon_back->scripts == addon.scripts);
    XSFLIB_CHECK(!ArchiveDescriptor::Parse(std::vector<uint8_t>(addon_raw.begin(), addon_raw.end() - 12)));
    XSFLIB_CHECK((hsf::ScriptedStart::Plain(0x01000003) ==
                  hsf::ScriptedStart{0x01000003, 0, {ScriptStep::Start(0, 0x01000003)}}));

    // With the track volumes too: the volumes come first, and the scripts after them.
    ArchiveDescriptor both = d;
    both.flags = ArchiveDescriptor::kFlagTrackVolumes | ArchiveDescriptor::kFlagScripts;
    hsf::SoundTrackVolumes volumes;
    volumes.sound = 0x01000002;
    volumes.volumes[4] = 0.0f;
    both.track_volumes = {volumes};
    const std::vector<uint8_t> both_raw = both.Serialize();
    const auto both_back = ArchiveDescriptor::Parse(both_raw);
    XSFLIB_CHECK(!both.Check());
    XSFLIB_CHECK(both_raw.size() == raw.size() + 4 + 68);
    XSFLIB_CHECK(both_back && both_back->track_volumes == both.track_volumes && both_back->scripts == both.scripts);

    // A descriptor without the flag is the same as before.
    ArchiveDescriptor unflagged = d;
    unflagged.flags = 0;
    unflagged.scripts.clear();
    XSFLIB_CHECK(unflagged.Serialize() == before);

    const auto refused = [&d](auto change, std::string_view message)
    {
        ArchiveDescriptor bad = d;
        change(bad);
        const std::optional<std::string> err = bad.Check();
        Check(err && err->find(message) != std::string::npos,
              "refused with \"" + std::string(message) + "\": " + err.value_or("accepted"));
    };

    const auto step = [](ArchiveDescriptor& bad, std::size_t script, std::size_t i) -> ScriptStep&
    {
        return bad.scripts[script].steps[i];
    };

    refused([](ArchiveDescriptor& bad) { bad.flags = 0; }, "without its flag for them (bit 1)");
    refused([](ArchiveDescriptor& bad) { bad.scripts[1].sound = 0x01000002; }, "more than once");
    refused([](ArchiveDescriptor& bad) { bad.scripts[1].sound = 0x02000005; }, "the item isn't a sound");
    refused([](ArchiveDescriptor& bad) { bad.scripts[0].steps.erase(bad.scripts[0].steps.begin() + 2); },
            "no step before it starts that sound");
    refused([](ArchiveDescriptor& bad) { bad.scripts[0].steps = {ScriptStep::Write(0, 28, 1)}; },
            "never starts the sound");
    refused([&step](ArchiveDescriptor& bad) { step(bad, 1, 3).frame = 1; }, "before the step ahead of it");
    refused([&step](ArchiveDescriptor& bad) { step(bad, 1, 0).sound = 0x05000000; }, "the item isn't a sound");
    refused([&step](ArchiveDescriptor& bad) { step(bad, 1, 2).sound = 0x01000009; }, "again");
    refused([&step](ArchiveDescriptor& bad) { step(bad, 1, 2).value = 3; }, "starts a sound and sets a variable");
    refused([&step](ArchiveDescriptor& bad) { step(bad, 1, 0).variable = 0; },
            "starts a sound of add-on archive 0; it lists 0 add-on archives");
    refused(
        [&step](ArchiveDescriptor& bad)
        {
            bad.addons = {{"addon.fsst", {}}};
            step(bad, 1, 2).variable = 0;
        },
        "never starts the sound from the set's archive");
    refused(
        [](ArchiveDescriptor& bad)
        {
            bad.flags = 0;
            bad.scripts.clear();
            bad.addons = {{"addon.fsst", {}}};
        },
        "without its flag for them (bit 1)");
    refused([&step](ArchiveDescriptor& bad) { step(bad, 0, 0).kind = 3; }, "version 1 has only 0");
    refused([&step](ArchiveDescriptor& bad) { step(bad, 0, 0).procedure = 1; }, "only a write of kind 2 names one");
    refused([&step](ArchiveDescriptor& bad) { step(bad, 1, 0).procedure = 2; }, "only a write of kind 2 names one");
    refused([&step](ArchiveDescriptor& bad) { step(bad, 0, 0).variable = 48; }, "are 0 to 47");
    refused([&step](ArchiveDescriptor& bad) { step(bad, 0, 4).track = 16; }, "on track 0 to 15");
    refused([&step](ArchiveDescriptor& bad) { step(bad, 0, 4).track = ScriptStep::kNone; }, "on track 0 to 15");
    refused([&step](ArchiveDescriptor& bad) { step(bad, 0, 3).track = 0; }, "only variables 32 to 47");
    refused([&step](ArchiveDescriptor& bad) { step(bad, 0, 1).sound = 0x01000002; }, "no sound's");
    refused([&step](ArchiveDescriptor& bad) { step(bad, 0, 1).variable = 28; }, "a second time in frame 0");
    refused([](ArchiveDescriptor& bad) { bad.scripts[1].lead_in = 0; }, "at frame 2, after the lead-in of 0 frames");
    refused([](ArchiveDescriptor& bad) { bad.scripts[1].lead_in = 900 + 120001; }, "at most 120000 frames");

    // The same variable on two tracks is two variables, and so is a variable of two frames.
    ArchiveDescriptor two_tracks = d;
    two_tracks.scripts[0].steps.push_back(ScriptStep::Write(0, 33, 8, 0x01000002, 1));
    two_tracks.scripts[0].steps.push_back(ScriptStep::Write(1, 28, 5));
    two_tracks.scripts[1].lead_in = 900 + 120000;
    XSFLIB_CHECK(!two_tracks.Check());

    // Writes of kind 2: two in procedure 2's call, and a global variable's in procedure 4's. A write of kind 1 of the
    // same variable in the same frame is another write, and so is a write of the same variable in another procedure.
    ArchiveDescriptor waits = d;
    std::vector<ScriptStep>& waiting = waits.scripts[1].steps;
    waiting.push_back(ScriptStep::WriteInProcedure(900, 2, 14, -1, 0x01000005));
    waiting.push_back(ScriptStep::WriteInProcedure(900, 2, 13, 3, 0x01000005));
    waiting.push_back(ScriptStep::WriteInProcedure(901, 4, 20, 98, 0x01000005));
    waiting.push_back(ScriptStep::Write(901, 20, 97));
    waiting.push_back(ScriptStep::WriteInProcedure(901, 3, 20, 96, 0x01000005));
    waits.scripts[1].lead_in = 901;
    const std::vector<uint8_t> waits_raw = waits.Serialize();
    const auto waits_back = ArchiveDescriptor::Parse(waits_raw);
    const std::size_t sixth = at + 4 + 2 * 12 + 10 * 16;
    XSFLIB_CHECK(!waits.Check() && waits_raw.size() == raw.size() + 5 * 16);
    XSFLIB_CHECK(waits_raw[sixth] == 0x84 && waits_raw[sixth + 1] == 0x03 && waits_raw[sixth + 4] == 2 &&
                 waits_raw[sixth + 5] == 2 && waits_raw[sixth + 6] == 14 && waits_raw[sixth + 10] == 0xff &&
                 waits_raw[sixth + 11] == 0xff && waits_raw[sixth + 12] == 0x05 && waits_raw[sixth + 15] == 0x01);
    XSFLIB_CHECK(waits_back && waits_back->scripts == waits.scripts);
    const auto refused_wait = [&waits](auto change, std::string_view message)
    {
        ArchiveDescriptor bad = waits;
        change(bad.scripts[1].steps);
        const std::optional<std::string> err = bad.Check();
        Check(err && err->find(message) != std::string::npos,
              "refused with \"" + std::string(message) + "\": " + err.value_or("accepted"));
    };

    refused_wait([](std::vector<ScriptStep>& steps) { steps[5].sound = 0x01000002; },
                 "no step before it starts that sound");
    refused_wait([](std::vector<ScriptStep>& steps) { steps[7].sound = 0; }, "no step before it starts that sound");
    refused_wait([](std::vector<ScriptStep>& steps) { steps[6].variable = 14; },
                 "a second time in frame 900 for user procedure 2");
    refused_wait([](std::vector<ScriptStep>& steps) { steps[5].variable = 48; }, "are 0 to 47");

    const std::optional<std::string> old_player = xsflib::psf::CheckArchiveStart(d, ArchiveDescriptor::kDefaultVoices);
    Check(old_player == "the archive descriptor's flags are 0x2; version 1 has none",
          "HSF 1.3.0's check refuses the flag: " + old_player.value_or("nothing"));
}

// Version 1's user procedures (flag bit 2) follow the blocks of the other flags. They make the round trip: the handler,
// the count of the tables, and each table's number and 128 values. A player accepts handler 1 with tables 0 to 15, each
// once, and handler 2 without tables. It refuses user procedures without the flag. HSF 1.3.0's check refuses the
// flag, with the message that docs/hsf.md gives.
void TestUserProcedures()
{
    ArchiveDescriptor plain;
    plain.voice_count = 96;
    plain.archive_path = "Archive/sound~subarc_mg1801.nx.bea/audio/sounddata/subarc_mg1801/subarc_mg1801.fsst";

    ArchiveDescriptor d = plain;
    d.flags = ArchiveDescriptor::kFlagProcedures;
    d.procedures.handler = hsf::UserProcedureHandler::kJamboree;
    hsf::UserProcedureTable table;
    for (std::size_t i = 0; i < table.values.size(); ++i)
    {
        table.values[i] = static_cast<int16_t>(1000 - static_cast<int>(i) * 20);
    }

    d.procedures.tables = {table};
    const std::vector<uint8_t> before = plain.Serialize();
    const std::vector<uint8_t> raw = d.Serialize();
    const auto back = ArchiveDescriptor::Parse(raw);

    // 4 for the handler, 4 for the count, and 4 + 256 for the table.
    const std::size_t at = before.size();
    XSFLIB_CHECK(!d.Check());
    XSFLIB_CHECK(raw.size() == at + 8 + 260 && raw[8] == 4 && raw[at] == 1 && raw[at + 4] == 1 && raw[at + 8] == 0 &&
                 raw[at + 12] == 0xe8 && raw[at + 13] == 0x03 && raw[at + 12 + 2 * 127] == 0xfc &&
                 raw[at + 13 + 2 * 127] == 0xf9);
    XSFLIB_CHECK(back && back->flags == d.flags && back->procedures == d.procedures);
    XSFLIB_CHECK(!ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.end() - 2)));

    // With the other two flags: the track volumes and the scripts come first.
    ArchiveDescriptor all = d;
    all.flags |= ArchiveDescriptor::kFlagTrackVolumes | ArchiveDescriptor::kFlagScripts;
    hsf::SoundTrackVolumes volumes;
    volumes.sound = 0x01000002;
    volumes.volumes[4] = 0.0f;
    all.track_volumes = {volumes};
    all.scripts = {hsf::ScriptedStart::Plain(0x01000002)};
    all.addons = {{"Archive/sound~subarc_rc_cmn.nx.bea/audio/sounddata/subarc_rc_cmn/subarc_rc_cmn.fsst", {}}};
    const auto all_back = ArchiveDescriptor::Parse(all.Serialize());
    XSFLIB_CHECK(!all.Check());
    XSFLIB_CHECK(all_back && all_back->procedures == all.procedures && all_back->scripts == all.scripts &&
                 all_back->addons == all.addons && all_back->track_volumes == all.track_volumes);

    // Superstars' handler has no tables.
    ArchiveDescriptor superstars = d;
    superstars.procedures = {hsf::UserProcedureHandler::kSuperstars, {}};
    const std::vector<uint8_t> superstars_raw = superstars.Serialize();
    const auto superstars_back = ArchiveDescriptor::Parse(superstars_raw);
    XSFLIB_CHECK(!superstars.Check() && superstars_raw.size() == at + 8);
    XSFLIB_CHECK(superstars_back && superstars_back->procedures == superstars.procedures);

    const auto refused = [&d](auto change, std::string_view message)
    {
        ArchiveDescriptor bad = d;
        change(bad);
        const std::optional<std::string> err = bad.Check();
        Check(err && err->find(message) != std::string::npos,
              "refused with \"" + std::string(message) + "\": " + err.value_or("accepted"));
    };

    refused([](ArchiveDescriptor& bad) { bad.flags = 0; }, "without its flag for them (bit 2)");
    refused([](ArchiveDescriptor& bad) { bad.procedures.handler = 3; }, "names handler 3 of user procedures");
    refused([](ArchiveDescriptor& bad) { bad.procedures.handler = 0; }, "names handler 0 of user procedures");
    refused([](ArchiveDescriptor& bad) { bad.procedures.handler = 2; }, "reads none");
    refused([](ArchiveDescriptor& bad) { bad.procedures.tables[0].number = 16; }, "the tables are 0 to 15");
    refused([](ArchiveDescriptor& bad) { bad.procedures.tables.push_back(bad.procedures.tables[0]); },
            "more than once");
    refused([](ArchiveDescriptor& bad) { bad.flags = 8; }, "version 1 has only bits 0 (track volumes), 1 (scripted");

    // A descriptor without the flag is the same as before.
    ArchiveDescriptor unflagged = d;
    unflagged.flags = 0;
    unflagged.procedures = {};
    XSFLIB_CHECK(unflagged.Serialize() == before && !unflagged.Check());

    const std::optional<std::string> old_player = xsflib::psf::CheckArchiveStart(d, ArchiveDescriptor::kDefaultVoices);
    Check(old_player == "the archive descriptor's flags are 0x4; version 1 has none",
          "HSF 1.3.0's check refuses the flag: " + old_player.value_or("nothing"));
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
    TestReverbEffect();
    TestTrackVolumes();
    TestScriptedStarts();
    TestUserProcedures();
    TestJaudioDescriptor();
    TestJaudioLibrary2();
    TestJaudioLibrary3();
    TestLoadSet();

    return Summary("format");
}
