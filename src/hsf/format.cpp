// SPDX-License-Identifier: MIT

// The HSF file format (see format.h and docs/hsf.md).

#include "hsf/format.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/psf/descriptor.h"
#include "xsflib/psf/psf.h"

namespace hsf
{
namespace
{

using xsflib::psf::Put16;
using xsflib::psf::Put32;

// Version 2's fields after the paths: the library, the DSP's level, the managers' and the categories' volumes, the FX
// lines, and the count of the sounds with track states. Each FX line takes 28 bytes and each sound 12. The count of the
// sounds with volumes follows the track states, and each of those sounds takes 8 bytes.
constexpr std::size_t kJaudioBytes = 4 + 4 + 8 + 4 * JaudioSettings::kCategories + 28 * JaudioSettings::kFxLines + 4;
constexpr std::size_t kTrackStatesBytes = 12;
constexpr std::size_t kSoundVolumeBytes = 8;

// Library 2's fields after version 2's: the effect sequence's file and volume, the conductor archive's path, the
// pieces of music and the sounds that start after draws. Each piece takes 12 bytes before its conductor's name. Its
// name and the archive's path end with zeros to a multiple of 4 bytes. Each sound with draws takes 8 bytes.
constexpr std::size_t kPieceBytes = 12;
constexpr std::size_t kDrawsBytes = 8;

// Library 3's fields after version 2's: the sequence index's path, the start-up files and the levels, then the wave
// scenes that the game loads, the pieces of music, the pieces of the cutscenes' music and the effects. The path ends
// with zeros to a multiple of 4 bytes. The start-up files, the two levels and the music's volume take 16 bytes. Each
// load and each part takes 8 bytes, each piece 12 and each effect 8.
constexpr std::size_t kLevelsBytes = 2 * JaudioLibrary3::kStartupHandles + 2 * 2 + 4;
constexpr std::size_t kLoadBytes = 8;
constexpr std::size_t kJacPieceBytes = 12;
constexpr std::size_t kPartBytes = 8;
constexpr std::size_t kEffectBytes = 8;

void PutLibrary2(std::vector<uint8_t>& v, const JaudioLibrary2& l)
{
    Put16(v, l.effect_sequence);
    v.push_back(l.effect_sequence_volume);
    v.push_back(0);
    xsflib::psf::PutPath(v, l.conductor_archive);
    xsflib::psf::PadTo4(v);

    Put32(v, static_cast<uint32_t>(l.pieces.size()));
    for (const JaudioPiece& piece : l.pieces)
    {
        Put16(v, piece.file);
        v.push_back(piece.kind);
        v.push_back(piece.volume);
        v.push_back(piece.kept_levels);
        v.push_back(0);
        Put16(v, piece.beat_interval);
        Put16(v, piece.muted_tracks);
        Put16(v, piece.start_point);
        xsflib::psf::PutPath(v, piece.conductor);
        xsflib::psf::PadTo4(v);
    }

    Put32(v, static_cast<uint32_t>(l.draws.size()));
    for (const JaudioDraws& d : l.draws)
    {
        Put32(v, d.sound);
        Put32(v, d.draws);
    }
}

// Reads library 2's fields at `pos`. False if `data` ends first.
bool ReadLibrary2(const std::vector<uint8_t>& data, std::size_t pos, JaudioLibrary2& l)
{
    const auto padded = [&pos]
    {
        pos = (pos + 3) & ~std::size_t{3};
    };
    const auto u32 = [&data, &pos]() -> std::optional<uint32_t>
    {
        if (pos > data.size() || data.size() - pos < 4)
        {
            return std::nullopt;
        }

        const uint32_t value = xsflib::LoadLe32(&data[pos]);
        pos += 4;
        return value;
    };

    const std::optional<uint32_t> effect_sequence = u32();
    std::optional<std::string> archive = effect_sequence ? xsflib::psf::ReadPath(data, pos) : std::nullopt;
    if (!archive)
    {
        return false;
    }

    l.effect_sequence = static_cast<uint16_t>(*effect_sequence & 0xffff);
    l.effect_sequence_volume = static_cast<uint8_t>(*effect_sequence >> 16);

    padded();
    const std::optional<uint32_t> count = u32();
    if (!count || *count > (data.size() - pos) / kPieceBytes)
    {
        return false;
    }

    l.conductor_archive = std::move(*archive);
    l.pieces.resize(*count);
    for (JaudioPiece& piece : l.pieces)
    {
        if (pos > data.size() || data.size() - pos < kPieceBytes)
        {
            return false;
        }

        const uint8_t* p = &data[pos];
        piece.file = static_cast<uint16_t>(xsflib::LoadLe16(p));
        piece.kind = p[2];
        piece.volume = p[3];
        piece.kept_levels = p[4];
        piece.beat_interval = static_cast<uint16_t>(xsflib::LoadLe16(p + 6));
        piece.muted_tracks = static_cast<uint16_t>(xsflib::LoadLe16(p + 8));
        piece.start_point = static_cast<uint16_t>(xsflib::LoadLe16(p + 10));
        pos += kPieceBytes;
        std::optional<std::string> conductor = xsflib::psf::ReadPath(data, pos);
        if (!conductor)
        {
            return false;
        }

        piece.conductor = std::move(*conductor);
        padded();
    }

    const std::optional<uint32_t> draws = u32();
    if (!draws || *draws > (data.size() - pos) / kDrawsBytes)
    {
        return false;
    }

    l.draws.resize(*draws);
    for (JaudioDraws& d : l.draws)
    {
        d.sound = *u32();
        d.draws = *u32();
    }

    return true;
}

// Why a version 2 player can't play a library 2 descriptor's settings, or nothing. Library 2 reads its FX lines from
// the start-up archive. It has no categories' volumes, no track states and no sounds' volumes.
std::optional<std::string> CheckLibrary2(const JaudioSettings& j)
{
    const auto unity = [](float volume)
    {
        return volume == 1.0f;
    };
    if (!std::all_of(j.category_volumes.begin(), j.category_volumes.end(), unity))
    {
        return std::string("a library 2 descriptor has categories' volumes other than 1; library 2 has none");
    }

    if (!std::all_of(j.fx_lines.begin(), j.fx_lines.end(),
                     [](const JaudioFxLine& line) { return line == JaudioFxLine{}; }))
    {
        return std::string("a library 2 descriptor has FX lines; library 2 takes them from the start-up archive");
    }

    if (!j.track_states.empty() || !j.sound_volumes.empty())
    {
        return std::string("a library 2 descriptor has track states or sounds' volumes; library 2 has neither");
    }

    const JaudioLibrary2& l = j.library2;
    if (l.effect_sequence_volume > 127)
    {
        return "the effect sequence's volume is " + std::to_string(l.effect_sequence_volume) + "; it's at most 127";
    }

    if (l.pieces.size() > JaudioLibrary2::kMaxPieces)
    {
        return "the archive descriptor has " + std::to_string(l.pieces.size()) + " pieces; library 2 has at most 65536";
    }

    for (const JaudioPiece& piece : l.pieces)
    {
        if (piece.kind >= JaudioPiece::kClasses)
        {
            return "a piece's class is " + std::to_string(piece.kind) + "; library 2's classes are 0 to 3";
        }

        if (piece.volume > 127)
        {
            return "a piece's volume is " + std::to_string(piece.volume) + "; it's at most 127";
        }

        if (piece.kept_levels > 0x1f)
        {
            return "a piece's kept levels are " + std::to_string(piece.kept_levels) + "; they're bits 0 to 4";
        }

        if ((piece.kind == JaudioPiece::kConducted) != !piece.conductor.empty())
        {
            return std::string("a piece of class 3 has no conductor, or a piece of another class has one");
        }

        if (!piece.conductor.empty() && l.conductor_archive.empty())
        {
            return std::string("a piece has a conductor, and the archive descriptor names no conductor archive");
        }
    }

    for (std::size_t i = 0; i < l.draws.size(); i++)
    {
        const JaudioDraws& d = l.draws[i];
        if (d.draws == 0 || d.draws > JaudioDraws::kMaxDraws)
        {
            return "a sound starts after " + std::to_string(d.draws) + " draws; library 2 takes 1 to 65535";
        }

        for (std::size_t j = 0; j < i; j++)
        {
            if (l.draws[j].sound == d.sound)
            {
                return std::string("the archive descriptor lists a sound's draws twice");
            }
        }
    }

    return std::nullopt;
}

void PutScene(std::vector<uint8_t>& v, const JacScene& scene)
{
    Put16(v, scene.system);
    Put16(v, scene.scene);
}

void PutLibrary3(std::vector<uint8_t>& v, const JaudioLibrary3& l)
{
    xsflib::psf::PutPath(v, l.sequence_index);
    xsflib::psf::PadTo4(v);

    for (const uint16_t file : l.startup_files)
    {
        Put16(v, file);
    }
    Put16(v, l.effect_level);
    Put16(v, l.cutscene_level);
    Put32(v, std::bit_cast<uint32_t>(l.music_volume));

    Put32(v, static_cast<uint32_t>(l.loads.size()));
    for (const JacLoad& load : l.loads)
    {
        PutScene(v, load.scene);
        Put32(v, load.set);
    }

    Put32(v, static_cast<uint32_t>(l.pieces.size()));
    for (const JacPiece& piece : l.pieces)
    {
        Put16(v, piece.file);
        Put16(v, piece.playing_tracks);
        Put32(v, std::bit_cast<uint32_t>(piece.volume));
        PutScene(v, piece.scene);
    }

    Put32(v, static_cast<uint32_t>(l.parts.size()));
    for (const JacPart& part : l.parts)
    {
        Put16(v, part.number);
        Put16(v, 0);
        PutScene(v, part.scene);
    }

    Put32(v, static_cast<uint32_t>(l.effects.size()));
    for (const JacEffect& effect : l.effects)
    {
        v.push_back(effect.kind);
        v.push_back(effect.type);
        Put16(v, effect.value);
        Put16(v, effect.first);
        Put16(v, effect.finish);
    }
}

// Reads library 3's fields at `pos`. False if `data` ends first.
bool ReadLibrary3(const std::vector<uint8_t>& data, std::size_t pos, JaudioLibrary3& l)
{
    std::optional<std::string> index = xsflib::psf::ReadPath(data, pos);
    pos = (pos + 3) & ~std::size_t{3};
    if (!index || pos > data.size() || data.size() - pos < kLevelsBytes)
    {
        return false;
    }

    const auto u16 = [&data, &pos]
    {
        const uint32_t value = xsflib::LoadLe16(&data[pos]);
        pos += 2;
        return static_cast<uint16_t>(value);
    };
    const auto u32 = [&data, &pos]
    {
        const uint32_t value = xsflib::LoadLe32(&data[pos]);
        pos += 4;
        return value;
    };
    const auto scene = [&u16]
    {
        JacScene s;
        s.system = u16();
        s.scene = u16();
        return s;
    };

    // The count of a list whose entries take `bytes` each, or no value if the data ends first.
    const auto count = [&data, &pos, &u32](std::size_t bytes) -> std::optional<uint32_t>
    {
        if (data.size() - pos < 4)
        {
            return std::nullopt;
        }

        const uint32_t n = u32();
        return n <= (data.size() - pos) / bytes ? std::optional<uint32_t>(n) : std::nullopt;
    };

    l.sequence_index = std::move(*index);
    for (uint16_t& file : l.startup_files)
    {
        file = u16();
    }
    l.effect_level = u16();
    l.cutscene_level = u16();
    l.music_volume = std::bit_cast<float>(u32());

    const std::optional<uint32_t> loads = count(kLoadBytes);
    if (!loads)
    {
        return false;
    }

    l.loads.resize(*loads);
    for (JacLoad& load : l.loads)
    {
        load.scene = scene();
        load.set = u32();
    }

    const std::optional<uint32_t> pieces = count(kJacPieceBytes);
    if (!pieces)
    {
        return false;
    }

    l.pieces.resize(*pieces);
    for (JacPiece& piece : l.pieces)
    {
        piece.file = u16();
        piece.playing_tracks = u16();
        piece.volume = std::bit_cast<float>(u32());
        piece.scene = scene();
    }

    const std::optional<uint32_t> parts = count(kPartBytes);
    if (!parts)
    {
        return false;
    }

    l.parts.resize(*parts);
    for (JacPart& part : l.parts)
    {
        part.number = u16();
        pos += 2;
        part.scene = scene();
    }

    const std::optional<uint32_t> effects = count(kEffectBytes);
    if (!effects)
    {
        return false;
    }

    l.effects.resize(*effects);
    for (JacEffect& effect : l.effects)
    {
        effect.kind = data[pos];
        effect.type = data[pos + 1];
        pos += 2;
        effect.value = u16();
        effect.first = u16();
        effect.finish = u16();
    }

    return true;
}

// Why a version 2 player can't play a library 3 descriptor's settings, or nothing. Library 3 has no managers' or
// categories' volumes, no track states and no sounds' volumes. Its FX lines are the game's.
std::optional<std::string> CheckLibrary3(const JaudioSettings& j)
{
    const auto unity = [](float volume)
    {
        return volume == 1.0f;
    };
    if (!unity(j.music_volume) || !unity(j.effect_volume) ||
        !std::all_of(j.category_volumes.begin(), j.category_volumes.end(), unity))
    {
        return std::string(
            "a library 3 descriptor has managers' or categories' volumes other than 1; library 3 has none");
    }

    if (!j.track_states.empty() || !j.sound_volumes.empty())
    {
        return std::string("a library 3 descriptor has track states or sounds' volumes; library 3 has neither");
    }

    const JaudioLibrary3& l = j.library3;
    if (l.sequence_index.empty())
    {
        return std::string("the archive descriptor names no sequence index; library 3 requires one");
    }

    if (l.effect_level > JaudioLibrary3::kMaxLevel || l.cutscene_level > JaudioLibrary3::kMaxLevel)
    {
        return std::string("a level of the archive descriptor is past 32767");
    }

    const auto volume_ok = [](float volume)
    {
        return std::isfinite(volume) && volume >= 0.0f;
    };
    if (!volume_ok(l.music_volume) ||
        !std::all_of(l.pieces.begin(), l.pieces.end(), [&](const JacPiece& p) { return volume_ok(p.volume); }))
    {
        return std::string("a volume of the archive descriptor isn't a finite number of 0 or more");
    }

    if (l.pieces.size() > JaudioLibrary3::kMaxEntries || l.parts.size() > JaudioLibrary3::kMaxEntries ||
        l.effects.size() > JaudioLibrary3::kMaxEntries)
    {
        return std::string("the archive descriptor lists more than 65536 pieces, parts or effects");
    }

    for (const JacLoad& load : l.loads)
    {
        if (load.set > 1)
        {
            return "a wave scene's flag is " + std::to_string(load.set) + "; it's 0 or 1";
        }
    }

    for (const JacEffect& effect : l.effects)
    {
        if (effect.kind >= JacEffect::kKinds)
        {
            return "an effect's kind is " + std::to_string(effect.kind) + "; library 3's kinds are 0 to 7";
        }

        // An event's effect has a type of 1 to 7, a cutscene's a type of 0 or 1, and another effect a type of 0.
        const bool type_ok =
            effect.kind == JacEffect::kEvent
                ? effect.type != 0 && effect.type <= JacEffect::kEventTypes
                : effect.type == 0 || (effect.kind == JacEffect::kCutscene && effect.type == JacEffect::kCutscenePart);
        if (!type_ok)
        {
            return "an effect of kind " + std::to_string(effect.kind) + " has a type of " +
                   std::to_string(effect.type) + "; kind 3 takes 1 to 7, kind 7 takes 0 or 1, and the others 0";
        }

        if (effect.finish != 0 && effect.kind != JacEffect::kCutscene)
        {
            return "an effect of kind " + std::to_string(effect.kind) +
                   " has a cutscene's finish; only kind 7 takes one";
        }
    }

    return std::nullopt;
}

void PutJaudio(std::vector<uint8_t>& v, const JaudioSettings& j)
{
    Put32(v, j.library);
    Put32(v, j.dsp_level);
    Put32(v, std::bit_cast<uint32_t>(j.music_volume));
    Put32(v, std::bit_cast<uint32_t>(j.effect_volume));
    for (const float volume : j.category_volumes)
    {
        Put32(v, std::bit_cast<uint32_t>(volume));
    }
    for (const JaudioFxLine& line : j.fx_lines)
    {
        Put16(v, line.flags);
        Put16(v, line.subframes);
        for (std::size_t i = 0; i < 2; ++i)
        {
            Put16(v, line.buses[i]);
            Put16(v, line.levels[i]);
        }
        for (const int16_t coefficient : line.filter)
        {
            Put16(v, static_cast<uint16_t>(coefficient));
        }
    }

    // Each sound's states go 2 to a byte, the even track in the high nibble. The game's table holds them that way.
    Put32(v, static_cast<uint32_t>(j.track_states.size()));
    for (const JaudioTrackStates& sound : j.track_states)
    {
        Put32(v, sound.sound);
        for (std::size_t i = 0; i < sound.states.size(); i += 2)
        {
            v.push_back(static_cast<uint8_t>((sound.states[i] & 15) << 4 | (sound.states[i + 1] & 15)));
        }
    }

    Put32(v, static_cast<uint32_t>(j.sound_volumes.size()));
    for (const JaudioSoundVolume& sound : j.sound_volumes)
    {
        Put32(v, sound.sound);
        Put32(v, sound.volume);
    }

    // A player that supports only library 1 reads up to here, and refuses the library.
    if (j.library == JaudioSettings::kPikmin2Library)
    {
        PutLibrary2(v, j.library2);
    }
    else if (j.library == JaudioSettings::kPikminLibrary)
    {
        PutLibrary3(v, j.library3);
    }
}

// Reads version 2's fields at `pos`, after the paths and their padding. False if `data` ends first.
bool ReadJaudio(const std::vector<uint8_t>& data, std::size_t pos, JaudioSettings& j)
{
    pos = (pos + 3) & ~std::size_t{3};
    if (pos > data.size() || data.size() - pos < kJaudioBytes)
    {
        return false;
    }

    const auto u16 = [&data, &pos]
    {
        const uint32_t value = xsflib::LoadLe16(&data[pos]);
        pos += 2;
        return static_cast<uint16_t>(value);
    };
    const auto u32 = [&data, &pos]
    {
        const uint32_t value = xsflib::LoadLe32(&data[pos]);
        pos += 4;
        return value;
    };

    j.library = u32();
    j.dsp_level = u32();
    j.music_volume = std::bit_cast<float>(u32());
    j.effect_volume = std::bit_cast<float>(u32());
    for (float& volume : j.category_volumes)
    {
        volume = std::bit_cast<float>(u32());
    }
    for (JaudioFxLine& line : j.fx_lines)
    {
        line.flags = u16();
        line.subframes = u16();
        for (std::size_t i = 0; i < 2; ++i)
        {
            line.buses[i] = u16();
            line.levels[i] = u16();
        }
        for (int16_t& coefficient : line.filter)
        {
            coefficient = static_cast<int16_t>(u16());
        }
    }

    const uint32_t count = u32();
    if (count > (data.size() - pos) / kTrackStatesBytes)
    {
        return false;
    }

    j.track_states.resize(count);
    for (JaudioTrackStates& sound : j.track_states)
    {
        sound.sound = u32();
        for (std::size_t i = 0; i < sound.states.size(); i += 2)
        {
            sound.states[i] = static_cast<uint8_t>(data[pos] >> 4);
            sound.states[i + 1] = static_cast<uint8_t>(data[pos] & 15);
            ++pos;
        }
    }

    if (data.size() - pos < 4)
    {
        return false;
    }

    const uint32_t volumes = u32();
    if (volumes > (data.size() - pos) / kSoundVolumeBytes)
    {
        return false;
    }

    j.sound_volumes.resize(volumes);
    for (JaudioSoundVolume& sound : j.sound_volumes)
    {
        sound.sound = u32();
        sound.volume = u32();
    }

    if (j.library == JaudioSettings::kPikmin2Library)
    {
        return ReadLibrary2(data, pos, j.library2);
    }

    return j.library != JaudioSettings::kPikminLibrary || ReadLibrary3(data, pos, j.library3);
}

// Why a version 2 player can't play a version 2 descriptor, or nothing.
std::optional<std::string> CheckJaudio(const ArchiveDescriptor& d)
{
    if (auto err = xsflib::psf::CheckDescriptorFlags("archive", d.flags, 0))
    {
        return err;
    }

    if (auto err = xsflib::psf::CheckDescriptorOutputMode("archive", d.output_mode))
    {
        return err;
    }

    if (d.voice_count != ArchiveDescriptor::kJaudioVoices)
    {
        return "the archive descriptor's voice count is " + std::to_string(d.voice_count) + "; version 2 has 64";
    }

    if (!d.effects.empty())
    {
        return std::string("the archive descriptor has aux effects; version 2 has none");
    }

    if (d.group_paths.size() != 2)
    {
        return "the archive descriptor has " + std::to_string(d.group_paths.size()) +
               " group paths; version 2 has 2: the sequence archive and the folder of the wave archives";
    }

    const JaudioSettings& j = d.jaudio;
    if (j.library != JaudioSettings::kGalaxyLibrary && j.library != JaudioSettings::kPikmin2Library &&
        j.library != JaudioSettings::kPikminLibrary)
    {
        return "the archive descriptor's library is " + std::to_string(j.library) +
               "; this player supports libraries 1 to 3 (the Super Mario Galaxy games' JAudio2, Pikmin 2's JAudio1 "
               "and Pikmin's C JAudio)";
    }

    if (j.library == JaudioSettings::kPikmin2Library)
    {
        if (auto err = CheckLibrary2(j))
        {
            return err;
        }
    }

    if (j.library == JaudioSettings::kPikminLibrary)
    {
        if (auto err = CheckLibrary3(j))
        {
            return err;
        }
    }

    if (j.dsp_level > 0xffff)
    {
        return "the archive descriptor's DSP level is " + std::to_string(j.dsp_level) + "; it's at most 65535";
    }

    const auto volume_ok = [](float volume)
    {
        return std::isfinite(volume) && volume >= 0.0f;
    };
    if (!volume_ok(j.music_volume) || !volume_ok(j.effect_volume) ||
        !std::all_of(j.category_volumes.begin(), j.category_volumes.end(), volume_ok))
    {
        return std::string("a volume of the archive descriptor isn't a finite number of 0 or more");
    }

    for (const JaudioFxLine& line : j.fx_lines)
    {
        if (line.subframes > JaudioSettings::kMaxSubframes)
        {
            return "an FX line's delay is " + std::to_string(line.subframes) + " sub-frames; it's at most 128";
        }
    }

    for (const JaudioSoundVolume& sound : j.sound_volumes)
    {
        if (sound.volume > 255)
        {
            return "a sound's volume in the archive descriptor is " + std::to_string(sound.volume) +
                   "; it's at most 255";
        }
    }

    return std::nullopt;
}

// Why a version 1 player can't play a delay or a light limiter, or nothing. Each has 8 finite parameters. A delay's
// first 2 and a light limiter's first 4 are times, each a whole number of microseconds from 0 to 10 s. The last is the
// channel mode, 0 to 3.
std::optional<std::string> CheckRendererEffect(const AuxEffect& effect)
{
    const std::string name = effect.type == AuxEffect::kDelay ? "a delay" : "a light limiter";
    const std::vector<float>& p = effect.parameters;
    if (p.size() != AuxEffect::kRendererEffectParameters)
    {
        return name + " has " + std::to_string(p.size()) + " parameters; it has 8";
    }

    if (!std::all_of(p.begin(), p.end(), [](float parameter) { return std::isfinite(parameter); }))
    {
        return name + "'s parameter isn't a finite number";
    }

    const std::size_t times = effect.type == AuxEffect::kDelay ? 2 : 4;
    for (std::size_t i = 0; i < times; ++i)
    {
        if (p[i] < 0.0f || p[i] > AuxEffect::kMostTime || p[i] != std::trunc(p[i]))
        {
            return name + "'s time isn't a whole number of microseconds from 0 to 10 s";
        }
    }

    const float mode = p.back();
    if (mode != 0.0f && mode != 1.0f && mode != 2.0f && mode != 3.0f)
    {
        return name + "'s channel mode isn't 0, 1, 2 or 3";
    }

    return std::nullopt;
}

} // namespace

ArchiveDescriptor ArchiveDescriptor::Jaudio(const JaudioSettings& settings)
{
    ArchiveDescriptor d;
    d.version = kJaudioVersion;
    d.voice_count = kJaudioVoices;
    d.jaudio = settings;

    return d;
}

std::vector<uint8_t> ArchiveDescriptor::Serialize() const
{
    std::vector<uint8_t> v;
    xsflib::psf::PutArchiveStart(v, *this);
    Put32(v, sdk_version);
    Put32(v, static_cast<uint32_t>(effects.size()));
    for (const AuxEffect& effect : effects)
    {
        Put32(v, effect.bus);
        Put32(v, effect.type);
        Put32(v, static_cast<uint32_t>(effect.parameters.size()));
        for (const float parameter : effect.parameters)
        {
            Put32(v, std::bit_cast<uint32_t>(parameter));
        }
    }
    xsflib::psf::PutArchivePaths(v, *this);

    // Version 2 keeps version 1's layout up to here. A version 1 player then reads the version, and refuses it.
    if (version == kJaudioVersion)
    {
        PutJaudio(v, jaudio);
    }

    return v;
}

std::optional<ArchiveDescriptor> ArchiveDescriptor::Parse(const std::vector<uint8_t>& data)
{
    ArchiveDescriptor d;
    if (!xsflib::psf::ReadArchiveStart(data, 28, d))
    {
        return std::nullopt;
    }

    d.sdk_version = xsflib::LoadLe32(&data[20]);
    const uint32_t effects = xsflib::LoadLe32(&data[24]);
    std::size_t pos = 28;
    for (uint32_t i = 0; i < effects; ++i)
    {
        if (data.size() - pos < 12)
        {
            return std::nullopt;
        }

        AuxEffect effect;
        effect.bus = xsflib::LoadLe32(&data[pos]);
        effect.type = xsflib::LoadLe32(&data[pos + 4]);
        const uint32_t count = xsflib::LoadLe32(&data[pos + 8]);
        pos += 12;
        if (count > (data.size() - pos) / 4)
        {
            return std::nullopt;
        }

        for (uint32_t k = 0; k < count; ++k)
        {
            effect.parameters.push_back(std::bit_cast<float>(xsflib::LoadLe32(&data[pos + 4 * k])));
        }
        pos += std::size_t{4} * count;
        d.effects.push_back(std::move(effect));
    }

    if (!xsflib::psf::ReadArchivePaths(data, pos, d))
    {
        return std::nullopt;
    }

    if (d.version == kJaudioVersion && !ReadJaudio(data, pos, d.jaudio))
    {
        return std::nullopt;
    }

    return d;
}

std::optional<std::string> ArchiveDescriptor::Check() const
{
    if (version == kJaudioVersion)
    {
        return CheckJaudio(*this);
    }

    if (version != kVersion)
    {
        return "archive descriptor version " + std::to_string(version) + "; this player supports versions 1 and 2";
    }

    if (auto err = xsflib::psf::CheckArchiveStart(*this, kDefaultVoices))
    {
        return err;
    }

    for (const AuxEffect& effect : effects)
    {
        if (effect.bus > 2)
        {
            return "an aux effect is on bus " + std::to_string(effect.bus) + "; the aux buses are 0 to 2";
        }

        if (effect.type == AuxEffect::kDelay || effect.type == AuxEffect::kLightLimiter)
        {
            if (auto err = CheckRendererEffect(effect))
            {
                return err;
            }

            continue;
        }

        if (effect.type != AuxEffect::kI3dl2Reverb)
        {
            return "an aux effect's type is " + std::to_string(effect.type) +
                   "; version 1 supports the I3DL2 reverb (1), the delay (2) and the light limiter (3)";
        }

        if (effect.parameters.size() != AuxEffect::kI3dl2Parameters)
        {
            return "an I3DL2 reverb has " + std::to_string(effect.parameters.size()) + " parameters; it has 12";
        }

        for (const float parameter : effect.parameters)
        {
            if (!std::isfinite(parameter))
            {
                return std::string("an I3DL2 reverb's parameter isn't a finite number");
            }
        }
    }

    return std::nullopt;
}

} // namespace hsf
