// SPDX-License-Identifier: MIT

// The profiles of the JAudio2 games (see jaudio_profiles.h).

#include "rip/jaudio_profiles.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/jaudio/archive.h"
#include "ntdlib/jaudio/jmap.h"
#include "xsflib/sha1.h"

namespace hsf::rip
{
namespace
{

namespace nj = ntdlib::jaudio;

// The track states as the game's table holds them, two to a byte, the even track in the high nibble.
JaudioTrackStates States(uint32_t sound, const std::array<uint8_t, 8>& packed)
{
    JaudioTrackStates states;
    states.sound = sound;
    for (std::size_t i = 0; i < packed.size(); i++)
    {
        states.states[2 * i] = static_cast<uint8_t>(packed[i] >> 4);
        states.states[2 * i + 1] = static_cast<uint8_t>(packed[i] & 15);
    }

    return states;
}

// The track states of each piece of music in `states`, by the piece's index in group 0 of section 1.
void AddTrackStates(JaudioSettings& s, std::span<const std::pair<uint32_t, std::array<uint8_t, 8>>> states)
{
    for (const auto& [index, packed] : states)
    {
        s.track_states.push_back(States(uint32_t{kMusicSection} << 24 | index, packed));
    }
}

// A later state of a piece of music that has a setting: the piece's index in group 0 of section 1, the states of its
// setting that give it, and its track states as the game's table holds them.
struct LaterState
{
    uint32_t index = 0;
    std::string_view states;
    std::array<uint8_t, 8> packed{};
};

// A variant of each piece in `states`, named after the states that give it.
void AddVariants(JaudioGame& game, std::span<const LaterState> states)
{
    for (const LaterState& state : states)
    {
        game.variants.push_back(
            {States(uint32_t{kMusicSection} << 24 | state.index, state.packed), std::string(state.states)});
    }
}

// Super Mario Galaxy's code in the Switch release's `main`.
JaudioGame Galaxy()
{
    JaudioGame game;
    game.name = "Super Mario Galaxy";
    game.startup_sha1 = "b78389973b94ad6039745dbb3540840b5330251a";

    JaudioSettings& s = game.settings;

    // AudParams' output gain, 0.9 (0x6598f0), through JASDriver::setDSPLevel: 4 times it in 4.12.
    s.dsp_level = 0x3999;

    // AudParams' volumes of the sequence manager and the effect manager (0x6598fc and 0x659900), and the categories'
    // volumes of its first volume set (scCtgVolume, 0x659908). AudSystem::initVolumeSetting sets them.
    s.music_volume = 1.2f;
    s.effect_volume = 1.2f;
    s.category_volumes = {1.2f, 1.2f, 1.2f, 1.3f, 1.2f, 1.2f, 1.2f, 1.2f,
                          1.2f, 1.2f, 1.2f, 1.2f, 1.0f, 1.5f, 1.0f, 1.0f};

    // AudEffector::initFxLine's four lines (0x93f710), each with its two buses looked up in JASDsp's table of sends
    // (0x6511b8). Each line delays by 64 sub-frames.
    constexpr std::array<int16_t, 8> kFilter = {0, 0x1000, 0, 0, 0, 0, 0x3fff, 0};
    s.fx_lines[0] = {1, 64, {0x0d00, 0x0d60}, {0x7000, 0}, kFilter};
    s.fx_lines[1] = {1, 64, {0x0d00, 0x0d60}, {0, 0x7000}, kFilter};
    s.fx_lines[2] = {1, 64, {0x0d00, 0x0f40}, {0x7000, 0x7000}, kFilter};
    s.fx_lines[3] = {1, 64, {0x0ca0, 0x0d60}, {0x7000, 0x7000}, kFilter};

    // AudBgmSetting's first state of each piece of music that has a setting (cBgmSettingInfo at 0x659bc4, cMuteState
    // at 0x659f0c). AudSingleBgm::start gives a piece that state as it starts. Each name says which tracks play. Each
    // state mutes track 0 and fades out the tracks that don't play. kNoTracksMuted1 also mutes track 1 while its
    // volume is 0. The pieces that play no track play beside streams, and the game fades their tracks in later.
    constexpr std::array<uint8_t, 8> kNoTracks = {0x20, 0, 0, 0, 0, 0, 0, 0};
    constexpr std::array<uint8_t, 8> kTracks1To13 = {0x21, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x00};
    constexpr std::array<uint8_t, 8> kTracks1To10 = {0x21, 0x11, 0x11, 0x11, 0x11, 0x10, 0x00, 0x00};
    constexpr std::array<uint8_t, 8> kNoTracksMuted1 = {0x28, 0, 0, 0, 0, 0, 0, 0};
    const std::array<std::pair<uint32_t, std::array<uint8_t, 8>>, 18> kStates = {{
        {5, kNoTracks},
        {8, kTracks1To13},
        {15, kNoTracks},
        {21, kNoTracks},
        {26, kNoTracks},
        {37, kNoTracks},
        {40, kNoTracks},
        {56, kNoTracksMuted1},
        {59, kNoTracks},
        {60, kNoTracks},
        {69, kNoTracks},
        {70, kNoTracks},
        {75, kTracks1To10},
        {79, kNoTracks},
        {83, kNoTracks},
        {84, kNoTracks},
        {96, kNoTracks},
        {99, kTracks1To10},
    }};
    AddTrackStates(s, kStates);

    // The later states of AudBgmSetting: each setting holds 8 states of 10 bytes, from cMuteState. A piece takes other
    // states as the game goes on. The BGM manager then fades each track to the state's level (0x1b27b8). Each state
    // below differs from the piece's first state. A state that fades out every track is left out.
    //
    // A track state of 7 keeps the track's level where the state before left it (0x1b2ba0). States 1 to 4 of piece
    // 0x38 keep track 1, the only track that any state plays. They leave it as the first state or state 6 left it, and
    // add nothing. The Comet Observatory's three pieces (0x25, 0x3b and 0x60) start with no track. Each of their states
    // 3 to 6 sets one of two layers and keeps the other: state 4 plays tracks 1 to 3 and state 3 fades them out. State
    // 6 plays tracks 4 and 5 and state 5 fades them out. States 1 and 2 keep every track. Each layer gets a variant on
    // its own, from the first state. Both layers at once may be a state that the game never plays, as in Pikmin 2.
    const std::array<LaterState, 17> kLaterStates = {{
        {0x05, "state 3", {0x21, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
        {0x0f, "states 4 and 6", {0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
        {0x25, "state 4", {0x21, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
        {0x25, "state 6", {0x20, 0x00, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00}},
        {0x38, "state 6", {0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
        {0x3b, "state 4", {0x21, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
        {0x3b, "state 6", {0x20, 0x00, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00}},
        {0x3c, "state 2", {0x20, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
        {0x3c, "state 3", {0x21, 0x11, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00}},
        {0x3c, "state 4", {0x20, 0x01, 0x00, 0x11, 0x00, 0x00, 0x00, 0x00}},
        {0x4b, "state 1", {0x20, 0x00, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00}},
        {0x4b, "states 3 and 4", {0x21, 0x11, 0x11, 0x11, 0x11, 0x10, 0x00, 0x10}},
        {0x4f, "state 3", {0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
        {0x60, "state 4", {0x21, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
        {0x60, "state 6", {0x20, 0x00, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00}},
        {0x63, "state 1", {0x20, 0x00, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00}},
        {0x63, "states 3 and 4", {0x21, 0x11, 0x11, 0x11, 0x11, 0x10, 0x00, 0x10}},
    }};
    AddVariants(game, kLaterStates);

    // The sounds of group 1 of section 1. AudMultiBgm plays each as a piece of music of group 0 and a stream
    // (cMultiBgmSet).
    game.replaced_groups = {1 << 8 | 1};

    return game;
}

// Super Mario Galaxy 2's code in the Switch release's `main`.
JaudioGame Galaxy2()
{
    JaudioGame game;
    game.name = "Super Mario Galaxy 2";
    game.startup_sha1 = "fb262f1705a6253603055f36002d42dea6b51c87";

    JaudioSettings& s = game.settings;

    // AudParams: the output gain 0.9 (0x745a14), the volumes of the sequence manager and the effect manager
    // (0x745a20 and 0x745a24), and the first volume set of the categories (0x746f5c). All match the first game's.
    s.dsp_level = 0x3999;
    s.music_volume = 1.2f;
    s.effect_volume = 1.2f;
    s.category_volumes = {1.2f, 1.2f, 1.2f, 1.3f, 1.2f, 1.2f, 1.2f, 1.2f,
                          1.2f, 1.2f, 1.2f, 1.2f, 1.0f, 1.5f, 1.0f, 1.0f};

    // AudEffector::initFxLine's four lines (0x1290bc), from the first of its two sets (0xa448a0), each with its two
    // buses looked up in JASDsp's table of sends (0x73fa9c). AudEffector::changeFxLineSet (0x129228) gives a stage the
    // second set when the stage is one of a list (0xb80e0).
    constexpr std::array<int16_t, 8> kFilter = {0x0625, -0x0745, 0x0625, -0x0626, -0x0508, 0x1808, 0x28cd, -0x0f18};
    s.fx_lines[0] = {1, 69, {0x0d00, 0x0d60}, {0x7fff, 0x30a1}, kFilter};
    s.fx_lines[1] = {1, 69, {0x0d00, 0x0d60}, {0x30a1, 0x7fff}, kFilter};
    s.fx_lines[2] = {1, 100, {0x0d00, 0x0f40}, {0x7fff, 0x7fff}, kFilter};
    s.fx_lines[3] = {1, 85, {0x0ca0, 0x0d60}, {0x7fff, 0x7fff}, kFilter};

    // AudBgmSetting's first state of each piece of music that has a setting. A switch on the sound's ID (0x1252d0)
    // finds the piece's setting, and the mute states are at 0x745d9c, 11 to a setting. AudSingleBgm::start (0x1219e8)
    // gives a piece the first state as it starts. The names say which tracks play. The first game's list names its
    // states the same way.
    constexpr std::array<uint8_t, 8> kNoTracks = {0x20, 0, 0, 0, 0, 0, 0, 0};
    constexpr std::array<uint8_t, 8> kTracks4And7And8 = {0x20, 0x00, 0x10, 0x01, 0x10, 0x00, 0x00, 0x00};
    constexpr std::array<uint8_t, 8> kTracks1To5 = {0x21, 0x11, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00};
    const std::array<std::pair<uint32_t, std::array<uint8_t, 8>>, 41> kStates = {{
        {0x08, {0x21, 0x11, 0x11, 0x11, 0x10, 0x10, 0x00, 0x00}},
        {0x09, {0x21, 0x11, 0x11, 0x11, 0x11, 0x10, 0x00, 0x00}},
        {0x0a, {0x21, 0x11, 0x11, 0x00, 0x11, 0x10, 0x00, 0x11}},
        {0x17, {0x21, 0x11, 0x10, 0x01, 0x10, 0x00, 0x00, 0x08}},
        {0x18, kNoTracks},
        {0x1d, kNoTracks},
        {0x1e, {0x21, 0x11, 0x11, 0x11, 0x11, 0x00, 0x00, 0x08}},
        {0x21, kNoTracks},
        {0x23, kTracks4And7And8},
        {0x25, kNoTracks},
        {0x29, kTracks4And7And8},
        {0x2c, {0x21, 0x11, 0x11, 0x11, 0x11, 0x11, 0x10, 0x00}},
        {0x2d, {0x21, 0x10, 0x00, 0x01, 0x00, 0x11, 0x10, 0x00}},
        {0x31, kNoTracks},
        {0x32, kNoTracks},
        {0x35, kNoTracks},
        {0x39, kTracks1To5},
        {0x3a, kTracks4And7And8},
        {0x3c, kNoTracks},
        {0x45, kNoTracks},
        {0x46, kNoTracks},
        {0x47, kNoTracks},
        {0x53, kNoTracks},
        {0x54, kNoTracks},
        {0x58, kNoTracks},
        {0x5a, kNoTracks},
        {0x5b, kNoTracks},
        {0x5d, kNoTracks},
        {0x60, kNoTracks},
        {0x61, kNoTracks},
        {0x62, kNoTracks},
        {0x65, kNoTracks},
        {0x67, kNoTracks},
        {0x70, kTracks1To5},
        {0x72, kNoTracks},
        {0x73, kNoTracks},
        {0x74, kNoTracks},
        {0x76, kNoTracks},
        {0x77, kNoTracks},
        {0x78, kNoTracks},
        {0x7b, kNoTracks},
    }};
    AddTrackStates(s, kStates);

    // The later states of AudBgmSetting (getMuteState, 0x1254f0): each setting holds 11 states of 10 bytes. A piece
    // takes other states as the game goes on. The BGM manager then fades each track to the state's level
    // (changeTrackMuteState, 0x12216c). Each state below differs from the piece's first state. A state that fades out
    // every track is left out. In most of the pieces, states 8 to 10 add track 15. The last 2 bytes of a state are the
    // levels of a piece's stream (0x122fd4). HSF plays no streams.
    const std::array<LaterState, 30> kLaterStates = {{
        {0x08, "states 8 to 10", {0x21, 0x11, 0x11, 0x11, 0x10, 0x10, 0x00, 0x01}},
        {0x09, "state 1", {0x20, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00}},
        {0x09, "states 3 and 4", {0x21, 0x11, 0x11, 0x11, 0x11, 0x10, 0x00, 0x10}},
        {0x0a, "state 1", {0x20, 0x00, 0x00, 0x00, 0x00, 0x01, 0x11, 0x00}},
        {0x17, "state 8", {0x21, 0x11, 0x10, 0x01, 0x10, 0x00, 0x00, 0x01}},
        {0x17, "states 9 and 10", {0x21, 0x11, 0x11, 0x11, 0x10, 0x00, 0x00, 0x01}},
        {0x1e, "state 2", {0x21, 0x11, 0x11, 0x11, 0x00, 0x11, 0x11, 0x11}},
        {0x23, "state 8", {0x20, 0x00, 0x10, 0x01, 0x10, 0x00, 0x00, 0x01}},
        {0x23, "states 9 and 10", {0x20, 0x00, 0x11, 0x11, 0x10, 0x00, 0x00, 0x01}},
        {0x25, "states 8 to 10", {0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}},
        {0x29, "state 8", {0x20, 0x00, 0x10, 0x01, 0x10, 0x00, 0x00, 0x01}},
        {0x29, "states 9 and 10", {0x20, 0x00, 0x11, 0x11, 0x10, 0x00, 0x00, 0x01}},
        {0x2c, "states 8 to 10", {0x21, 0x11, 0x11, 0x11, 0x11, 0x11, 0x10, 0x01}},
        {0x2d, "states 8 to 10", {0x21, 0x10, 0x00, 0x01, 0x00, 0x11, 0x10, 0x01}},
        {0x35, "states 8 to 10", {0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}},
        {0x39, "states 8 to 10", {0x21, 0x11, 0x11, 0x00, 0x00, 0x00, 0x00, 0x01}},
        {0x3a, "state 8", {0x20, 0x00, 0x10, 0x01, 0x10, 0x00, 0x00, 0x01}},
        {0x3a, "states 9 and 10", {0x20, 0x00, 0x11, 0x11, 0x10, 0x00, 0x00, 0x01}},
        {0x45, "state 6", {0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
        {0x47, "states 8 to 10", {0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}},
        {0x58, "states 4 and 6", {0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
        {0x58, "states 8 to 10", {0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}},
        {0x5a, "states 8 to 10", {0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}},
        {0x65, "states 8 to 10", {0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}},
        {0x70, "states 8 to 10", {0x21, 0x11, 0x11, 0x00, 0x00, 0x00, 0x00, 0x01}},
        {0x72, "states 8 to 10", {0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}},
        {0x73, "states 8 to 10", {0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}},
        {0x76, "states 8 to 10", {0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}},
        {0x77, "states 8 to 10", {0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}},
        {0x78, "states 8 to 10", {0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}},
    }};
    AddVariants(game, kLaterStates);

    // As in the first game, AudBgmMgr::start (0x121140) plays a sound with bit 16 of its ID set beside a stream.
    game.replaced_groups = {1 << 8 | 1};

    // The volumes of the music in a table of the game's files (0x128cf0).
    game.bgm_params = "Info/BgmParam.arc";

    return game;
}

const std::vector<JaudioGame>& Games()
{
    static const std::vector<JaudioGame> kGames = {Galaxy(), Galaxy2()};
    return kGames;
}

} // namespace

const JaudioGame* FindJaudioGame(std::span<const uint8_t> startup)
{
    const std::string sha1 = xsflib::Sha1::Hex(xsflib::Sha1::Of(startup));
    for (const JaudioGame& game : Games())
    {
        if (game.startup_sha1 == sha1)
        {
            return &game;
        }
    }

    return nullptr;
}

std::vector<JaudioSoundVolume> ReadBgmVolumes(std::span<const uint8_t> table_archive, const nj::SoundArchive& archive)
{
    std::map<std::string, const nj::SoundEntry*> by_name;
    for (const nj::SoundEntry& sound : archive.Sounds())
    {
        if (!sound.name.empty())
        {
            by_name.emplace(sound.name, &sound);
        }
    }

    std::vector<JaudioSoundVolume> volumes;
    for (const nj::JMapNamedValue& row :
         nj::ReadJMapNamedValues(table_archive, "BgmParam.bcsv", "SoundIdName", "Volume"))
    {
        const auto sound = by_name.find(row.name);
        if (sound != by_name.end() && (sound->second->type & 0x40) != 0)
        {
            volumes.push_back({sound->second->Id(), row.value & 0xff});
        }
    }

    return volumes;
}

} // namespace hsf::rip
