// SPDX-License-Identifier: MIT

// The HSF file format (see format.h and docs/hsf.md).

#include "hsf/format.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/psf/descriptor.h"
#include "xsflib/psf/psf.h"
#include "xsflib/text.h"

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

// Version 1's track volumes, after the paths: the count of the sounds, then each sound's item ID and its 16 volumes.
constexpr std::size_t kSoundTrackVolumesBytes = 4 + 4 * SoundTrackVolumes::kTracks;

// Version 1's scripted starts, after the track volumes: the count of the sounds, then each sound's item ID, its lead-in
// and the count of its steps, then each step's frame, kind, procedure, variable, track, value and item ID.
constexpr std::size_t kScriptBytes = 12;
constexpr std::size_t kScriptStepBytes = 16;

// Version 1's user procedures, after the scripted starts: the handler and the count of its tables, then each table's
// number and its 128 values.
constexpr std::size_t kProceduresBytes = 8;
constexpr std::size_t kProcedureTableBytes = 4 + 2 * UserProcedureTable::kValues;

// The longest a sound plays after its lead-in's last step: the length analysis's limit of 600 s, in frames of 5 ms.
constexpr uint64_t kMaxScriptFrames = 120000;

// The type of a sound in the top byte of its nn::atk item ID (ntdlib's sar::kSoundItem).
constexpr uint32_t kSoundItemType = 0x01;

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

        for (std::size_t k = 0; k < i; k++)
        {
            if (l.draws[k].sound == d.sound)
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

// Version 3's fields after the paths: the layer and NW4R SND's build.
constexpr std::size_t kNw4rBytes = 8;

void PutNw4r(std::vector<uint8_t>& v, const Nw4rSettings& n)
{
    Put32(v, n.layer);
    Put32(v, n.snd_build);
}

// Reads version 3's fields at `pos`, after the paths and their padding. False if `data` ends first.
bool ReadNw4r(const std::vector<uint8_t>& data, std::size_t pos, Nw4rSettings& n)
{
    pos = (pos + 3) & ~std::size_t{3};
    if (pos > data.size() || data.size() - pos < kNw4rBytes)
    {
        return false;
    }

    n.layer = xsflib::LoadLe32(&data[pos]);
    n.snd_build = xsflib::LoadLe32(&data[pos + 4]);

    return true;
}

// Why a version 3 player can't play a version 3 descriptor, or nothing.
std::optional<std::string> CheckNw4r(const ArchiveDescriptor& d)
{
    if (auto err = xsflib::psf::CheckDescriptorFlags("archive", d.flags, 0))
    {
        return err;
    }

    if (auto err = xsflib::psf::CheckDescriptorOutputMode("archive", d.output_mode))
    {
        return err;
    }

    if (d.voice_count != ArchiveDescriptor::kNw4rVoices)
    {
        return "the archive descriptor's voice count is " + std::to_string(d.voice_count) + "; version 3 has 64";
    }

    if (!d.effects.empty())
    {
        return std::string("the archive descriptor has aux effects; version 3 has none");
    }

    if (!d.group_paths.empty())
    {
        return "the archive descriptor has " + std::to_string(d.group_paths.size()) +
               " group paths; version 3 has none";
    }

    if (d.nw4r.layer != Nw4rSettings::kSkywardSwordLayer)
    {
        return "the archive descriptor's layer is " + std::to_string(d.nw4r.layer) +
               "; this player supports layer 1 (Skyward Sword HD's)";
    }

    return std::nullopt;
}

// Writes version 1's track volumes (kFlagTrackVolumes).
void PutTrackVolumes(std::vector<uint8_t>& v, const std::vector<SoundTrackVolumes>& sounds)
{
    Put32(v, static_cast<uint32_t>(sounds.size()));
    for (const SoundTrackVolumes& sound : sounds)
    {
        Put32(v, sound.sound);
        for (const float volume : sound.volumes)
        {
            Put32(v, std::bit_cast<uint32_t>(volume));
        }
    }
}

// Reads version 1's track volumes after the paths' padding, from `pos`, the end of the last path, and moves `pos` past
// them. False if `data` ends first.
bool ReadTrackVolumes(const std::vector<uint8_t>& data, std::size_t& pos, std::vector<SoundTrackVolumes>& sounds)
{
    pos = (pos + 3) & ~std::size_t{3};
    if (pos > data.size() || data.size() - pos < 4)
    {
        return false;
    }

    const uint32_t count = xsflib::LoadLe32(&data[pos]);
    pos += 4;
    if (count > (data.size() - pos) / kSoundTrackVolumesBytes)
    {
        return false;
    }

    sounds.resize(count);
    for (SoundTrackVolumes& sound : sounds)
    {
        sound.sound = xsflib::LoadLe32(&data[pos]);
        for (std::size_t t = 0; t < SoundTrackVolumes::kTracks; ++t)
        {
            sound.volumes[t] = std::bit_cast<float>(xsflib::LoadLe32(&data[pos + 4 + 4 * t]));
        }
        pos += kSoundTrackVolumesBytes;
    }

    return true;
}

// Writes version 1's scripted starts and add-on archives (kFlagScripts).
void PutScripts(std::vector<uint8_t>& v, const std::vector<ScriptedStart>& scripts,
                const std::vector<AddonArchive>& addons)
{
    Put32(v, static_cast<uint32_t>(scripts.size()));
    for (const ScriptedStart& script : scripts)
    {
        Put32(v, script.sound);
        Put32(v, script.lead_in);
        Put32(v, static_cast<uint32_t>(script.steps.size()));
        for (const ScriptStep& step : script.steps)
        {
            Put32(v, step.frame);
            v.push_back(step.kind);
            v.push_back(step.procedure);
            Put16(v, step.variable);
            Put16(v, step.track);
            Put16(v, static_cast<uint16_t>(step.value));
            Put32(v, step.sound);
        }
    }

    Put32(v, static_cast<uint32_t>(addons.size()));
    for (const AddonArchive& addon : addons)
    {
        xsflib::psf::PutArchivePaths(v, addon);
    }
}

// Reads version 1's scripted starts and add-on archives from `pos`, the end of the paths or of the track volumes, after
// the padding, and moves `pos` past them. False if `data` ends first.
bool ReadScripts(const std::vector<uint8_t>& data, std::size_t& pos, std::vector<ScriptedStart>& scripts,
                 std::vector<AddonArchive>& addons)
{
    pos = (pos + 3) & ~std::size_t{3};
    if (pos > data.size() || data.size() - pos < 4)
    {
        return false;
    }

    const uint32_t count = xsflib::LoadLe32(&data[pos]);
    pos += 4;
    if (count > (data.size() - pos) / kScriptBytes)
    {
        return false;
    }

    scripts.resize(count);
    for (ScriptedStart& script : scripts)
    {
        if (data.size() - pos < kScriptBytes)
        {
            return false;
        }

        script.sound = xsflib::LoadLe32(&data[pos]);
        script.lead_in = xsflib::LoadLe32(&data[pos + 4]);
        const uint32_t steps = xsflib::LoadLe32(&data[pos + 8]);
        pos += kScriptBytes;
        if (steps > (data.size() - pos) / kScriptStepBytes)
        {
            return false;
        }

        script.steps.resize(steps);
        for (ScriptStep& step : script.steps)
        {
            step.frame = xsflib::LoadLe32(&data[pos]);
            step.kind = data[pos + 4];
            step.procedure = data[pos + 5];
            step.variable = xsflib::LoadLe16(&data[pos + 6]);
            step.track = xsflib::LoadLe16(&data[pos + 8]);
            step.value = static_cast<int16_t>(xsflib::LoadLe16(&data[pos + 10]));
            step.sound = xsflib::LoadLe32(&data[pos + 12]);
            pos += kScriptStepBytes;
        }
    }

    if (data.size() - pos < 4)
    {
        return false;
    }

    const uint32_t addon_count = xsflib::LoadLe32(&data[pos]);
    pos += 4;
    if (addon_count > (data.size() - pos) / 4)
    {
        return false;
    }

    addons.resize(addon_count);
    for (AddonArchive& addon : addons)
    {
        pos = (pos + 3) & ~std::size_t{3};
        if (pos > data.size() || !xsflib::psf::ReadArchivePaths(data, pos, addon))
        {
            return false;
        }
    }

    return true;
}

// Writes version 1's user procedures (kFlagProcedures).
void PutProcedures(std::vector<uint8_t>& v, const UserProcedureHandler& procedures)
{
    Put32(v, procedures.handler);
    Put32(v, static_cast<uint32_t>(procedures.tables.size()));
    for (const UserProcedureTable& table : procedures.tables)
    {
        Put32(v, table.number);
        for (const int16_t value : table.values)
        {
            Put16(v, static_cast<uint16_t>(value));
        }
    }
}

// Reads version 1's user procedures from `pos`, the end of the blocks before them, after the padding. False if `data`
// ends first.
bool ReadProcedures(const std::vector<uint8_t>& data, std::size_t pos, UserProcedureHandler& procedures)
{
    pos = (pos + 3) & ~std::size_t{3};
    if (pos > data.size() || data.size() - pos < kProceduresBytes)
    {
        return false;
    }

    procedures.handler = xsflib::LoadLe32(&data[pos]);
    const uint32_t count = xsflib::LoadLe32(&data[pos + 4]);
    pos += kProceduresBytes;
    if (count > (data.size() - pos) / kProcedureTableBytes)
    {
        return false;
    }

    procedures.tables.resize(count);
    for (UserProcedureTable& table : procedures.tables)
    {
        table.number = xsflib::LoadLe32(&data[pos]);
        for (std::size_t i = 0; i < UserProcedureTable::kValues; ++i)
        {
            table.values[i] = static_cast<int16_t>(xsflib::LoadLe16(&data[pos + 4 + 2 * i]));
        }
        pos += kProcedureTableBytes;
    }

    return true;
}

// Why a version 1 player can't play a descriptor's flags and track volumes, or nothing. The flags are
// kFlagTrackVolumes, kFlagScripts and kFlagProcedures. With the first, each sound is listed once, with 16 volumes that
// are finite numbers of 0 or more.
std::optional<std::string> CheckTrackVolumes(const ArchiveDescriptor& d)
{
    constexpr uint32_t kFlags =
        ArchiveDescriptor::kFlagTrackVolumes | ArchiveDescriptor::kFlagScripts | ArchiveDescriptor::kFlagProcedures;
    if ((d.flags & ~kFlags) != 0)
    {
        return "the archive descriptor's flags are " + xsflib::Hex(d.flags) +
               "; version 1 has only bits 0 (track volumes), 1 (scripted starts) and 2 (user procedures)";
    }

    if ((d.flags & ArchiveDescriptor::kFlagTrackVolumes) == 0 && !d.track_volumes.empty())
    {
        return std::string("the archive descriptor lists track volumes without its flag for them (bit 0)");
    }

    for (std::size_t i = 0; i < d.track_volumes.size(); ++i)
    {
        const SoundTrackVolumes& sound = d.track_volumes[i];
        const auto same_sound = [&sound](const SoundTrackVolumes& other)
        {
            return other.sound == sound.sound;
        };
        if (std::any_of(d.track_volumes.begin() + static_cast<std::ptrdiff_t>(i) + 1, d.track_volumes.end(),
                        same_sound))
        {
            return "the archive descriptor lists the track volumes of sound " + xsflib::Hex(sound.sound, 8) +
                   " more than once";
        }

        const auto volume_ok = [](float volume)
        {
            return std::isfinite(volume) && volume >= 0.0f;
        };
        if (!std::all_of(sound.volumes.begin(), sound.volumes.end(), volume_ok))
        {
            return "a track volume of sound " + xsflib::Hex(sound.sound, 8) +
                   " in the archive descriptor isn't a finite number of 0 or more";
        }
    }

    return std::nullopt;
}

// Why a version 1 player can't play a step of `script`, the step at `i`, or nothing. A start starts a sound, once, from
// the set's archive or from one of `addons` add-on archives, and sets no variable. A write sets a variable below 48, of
// a sound that an earlier step starts, of the global variables or of a track from 0 to 15 of such a sound. A write of
// kind 2 waits for a procedure call of a sound that an earlier step starts. No other write of the same kind and
// procedure in the same frame sets the same variable. Only a write of kind 2 names a procedure.
std::optional<std::string> CheckStep(const ScriptedStart& script, std::size_t i, std::size_t addons)
{
    const ScriptStep& step = script.steps[i];
    const std::string where = "step " + std::to_string(i) + " of sound " + xsflib::Hex(script.sound, 8) + "'s script";
    const auto before = std::span(script.steps).first(i);
    const auto starts = [&before](uint32_t sound)
    {
        return std::any_of(before.begin(), before.end(),
                           [sound](const ScriptStep& s) { return s.kind == ScriptStep::kStart && s.sound == sound; });
    };

    if (i > 0 && step.frame < script.steps[i - 1].frame)
    {
        return "the archive descriptor's " + where + " comes at frame " + std::to_string(step.frame) +
               ", before the step ahead of it";
    }

    if (step.kind > ScriptStep::kWriteInProcedure)
    {
        return "the archive descriptor's " + where + " is of kind " + std::to_string(step.kind) +
               "; version 1 has only 0 (a start), 1 (a write) and 2 (a write in a user procedure)";
    }

    const bool in_procedure = step.kind == ScriptStep::kWriteInProcedure;
    if (!in_procedure && step.procedure != 0)
    {
        return "the archive descriptor's " + where + " names user procedure " + std::to_string(step.procedure) +
               "; only a write of kind 2 names one";
    }

    if (step.kind == ScriptStep::kStart)
    {
        if (step.sound >> 24 != kSoundItemType)
        {
            return "the archive descriptor's " + where + " starts item " + xsflib::Hex(step.sound, 8) +
                   "; the item isn't a sound";
        }

        if (step.variable != ScriptStep::kNone && step.variable >= addons)
        {
            return "the archive descriptor's " + where + " starts a sound of add-on archive " +
                   std::to_string(step.variable) + "; it lists " + std::to_string(addons) + " add-on archives";
        }

        if (step.track != ScriptStep::kNone || step.value != 0)
        {
            return "the archive descriptor's " + where + " starts a sound and sets a variable";
        }

        if (starts(step.sound))
        {
            return "the archive descriptor's " + where + " starts sound " + xsflib::Hex(step.sound, 8) + " again";
        }

        return std::nullopt;
    }

    const std::string name = "variable " + std::to_string(step.variable);
    if (step.variable >= ScriptStep::kTrackEnd)
    {
        return "the archive descriptor's " + where + " sets " + name + "; the sequence variables are 0 to 47";
    }

    const bool global = step.variable >= ScriptStep::kLocalEnd && step.variable < ScriptStep::kGlobalEnd;
    const bool track_variable = step.variable >= ScriptStep::kGlobalEnd;
    if (track_variable && step.track >= SoundTrackVolumes::kTracks)
    {
        return "the archive descriptor's " + where + " sets " + name + " on track " + std::to_string(step.track) +
               "; a track's variable is on track 0 to 15";
    }

    if (!track_variable && step.track != ScriptStep::kNone)
    {
        return "the archive descriptor's " + where + " sets " + name + " on track " + std::to_string(step.track) +
               "; only variables 32 to 47 are a track's";
    }

    if (global && !in_procedure && step.sound != 0)
    {
        return "the archive descriptor's " + where + " sets the global " + name + " of sound " +
               xsflib::Hex(step.sound, 8) + "; a global variable is no sound's";
    }

    if ((!global || in_procedure) && !starts(step.sound))
    {
        return "the archive descriptor's " + where + " sets " + name + " of sound " + xsflib::Hex(step.sound, 8) +
               "; no step before it starts that sound";
    }

    const auto same = [&step](const ScriptStep& other)
    {
        return other.kind == step.kind && other.procedure == step.procedure && other.frame == step.frame &&
               other.variable == step.variable && other.track == step.track && other.sound == step.sound;
    };
    if (std::any_of(before.begin(), before.end(), same))
    {
        return "the archive descriptor's " + where + " sets " + name + " a second time in frame " +
               std::to_string(step.frame) +
               (in_procedure ? " for user procedure " + std::to_string(step.procedure) : std::string());
    }

    return std::nullopt;
}

// Why a version 1 player can't play a descriptor's scripted starts, or nothing. Each sound is listed once, by a sound's
// item ID. Its steps come in frame order, and one of them starts the sound from the set's archive by the end of the
// lead-in. The lead-in ends at most 600 s after the last step.
std::optional<std::string> CheckScripts(const ArchiveDescriptor& d)
{
    if ((d.flags & ArchiveDescriptor::kFlagScripts) == 0 && (!d.scripts.empty() || !d.addons.empty()))
    {
        return std::string("the archive descriptor lists scripted starts without its flag for them (bit 1)");
    }

    for (std::size_t i = 0; i < d.scripts.size(); ++i)
    {
        const ScriptedStart& script = d.scripts[i];
        const std::string name = "sound " + xsflib::Hex(script.sound, 8);
        const auto same_sound = [&script](const ScriptedStart& other)
        {
            return other.sound == script.sound;
        };
        if (std::any_of(d.scripts.begin() + static_cast<std::ptrdiff_t>(i) + 1, d.scripts.end(), same_sound))
        {
            return "the archive descriptor lists the scripted start of " + name + " more than once";
        }

        if (script.sound >> 24 != kSoundItemType)
        {
            return "the archive descriptor lists a scripted start for item " + xsflib::Hex(script.sound, 8) +
                   "; the item isn't a sound";
        }

        for (std::size_t k = 0; k < script.steps.size(); ++k)
        {
            if (auto err = CheckStep(script, k, d.addons.size()))
            {
                return err;
            }
        }

        const auto starts_sound = [&script](const ScriptStep& s)
        {
            return s.kind == ScriptStep::kStart && s.sound == script.sound && s.variable == ScriptStep::kNone;
        };
        const auto start = std::find_if(script.steps.begin(), script.steps.end(), starts_sound);
        if (start == script.steps.end())
        {
            return "the archive descriptor's script of " + name + " never starts the sound from the set's archive";
        }

        if (start->frame > script.lead_in)
        {
            return "the archive descriptor's script of " + name + " starts the sound at frame " +
                   std::to_string(start->frame) + ", after the lead-in of " + std::to_string(script.lead_in) +
                   " frames";
        }

        const uint64_t last = script.steps.back().frame;
        if (script.lead_in > last + kMaxScriptFrames)
        {
            return "the archive descriptor's script of " + name + " has a lead-in of " +
                   std::to_string(script.lead_in) + " frames; it ends at most 120000 frames after the last step";
        }
    }

    return std::nullopt;
}

// Why a version 1 player can't play a descriptor's user procedures, or nothing. The handler is Jamboree's or
// Superstars'. Only Jamboree's handler reads tables. Each table's number is below 16, and the descriptor lists it once.
std::optional<std::string> CheckProcedures(const ArchiveDescriptor& d)
{
    const UserProcedureHandler& procedures = d.procedures;
    if ((d.flags & ArchiveDescriptor::kFlagProcedures) == 0)
    {
        if (procedures != UserProcedureHandler{})
        {
            return std::string("the archive descriptor names user procedures without its flag for them (bit 2)");
        }

        return std::nullopt;
    }

    if (procedures.handler != UserProcedureHandler::kJamboree &&
        procedures.handler != UserProcedureHandler::kSuperstars)
    {
        return "the archive descriptor names handler " + std::to_string(procedures.handler) +
               " of user procedures; version 1 has only 1 (Super Mario Party Jamboree's) and 2 (Mario Party "
               "Superstars')";
    }

    if (procedures.handler != UserProcedureHandler::kJamboree && !procedures.tables.empty())
    {
        return std::string("the archive descriptor gives tables to a handler of user procedures that reads none");
    }

    for (std::size_t i = 0; i < procedures.tables.size(); ++i)
    {
        const uint32_t number = procedures.tables[i].number;
        if (number >= UserProcedureTable::kTables)
        {
            return "the archive descriptor lists table " + std::to_string(number) +
                   " of its user procedures; the tables are 0 to 15";
        }

        const auto same_number = [number](const UserProcedureTable& other)
        {
            return other.number == number;
        };
        if (std::any_of(procedures.tables.begin() + static_cast<std::ptrdiff_t>(i) + 1, procedures.tables.end(),
                        same_number))
        {
            return "the archive descriptor lists table " + std::to_string(number) +
                   " of its user procedures more than once";
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

// Why a version 1 player can't play a reverb, or nothing. A reverb has 12 finite parameters. Its pre-delay is a whole
// number of microseconds from 0 to 300 ms, and its decay time one from 100 ms to 20 s. Its modes are 0 to 4, and its
// channel mode 0 to 3.
std::optional<std::string> CheckReverb(const AuxEffect& effect)
{
    const std::vector<float>& p = effect.parameters;
    if (p.size() != AuxEffect::kReverbParameters)
    {
        return "a reverb has " + std::to_string(p.size()) + " parameters; it has 12";
    }

    if (!std::all_of(p.begin(), p.end(), [](float parameter) { return std::isfinite(parameter); }))
    {
        return std::string("a reverb's parameter isn't a finite number");
    }

    const auto whole = [](float value, float least, float most)
    {
        return value >= least && value <= most && value == std::trunc(value);
    };
    if (!whole(p[kReverbPredelay], 0.0f, AuxEffect::kMostPredelay))
    {
        return std::string("a reverb's pre-delay isn't a whole number of microseconds from 0 to 300 ms");
    }

    if (!whole(p[kReverbDecay], AuxEffect::kLeastDecay, AuxEffect::kMostDecay))
    {
        return std::string("a reverb's decay time isn't a whole number of microseconds from 100 ms to 20 s");
    }

    if (!whole(p[kReverbEarlyMode], 0.0f, 4.0f) || !whole(p[kReverbLateMode], 0.0f, 4.0f))
    {
        return std::string("a reverb's early or late mode isn't 0, 1, 2, 3 or 4");
    }

    if (!whole(p[kReverbChannelMode], 0.0f, 3.0f))
    {
        return std::string("a reverb's channel mode isn't 0, 1, 2 or 3");
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

ArchiveDescriptor ArchiveDescriptor::Nw4r(const Nw4rSettings& settings, std::string archive_path, uint32_t sdk_version)
{
    ArchiveDescriptor d;
    d.version = kNw4rVersion;
    d.voice_count = kNw4rVoices;
    d.sdk_version = sdk_version;
    d.archive_path = std::move(archive_path);
    d.nw4r = settings;

    return d;
}

ScriptStep ScriptStep::Start(uint32_t frame, uint32_t sound, uint16_t archive)
{
    ScriptStep step;
    step.frame = frame;
    step.kind = kStart;
    step.variable = archive;
    step.sound = sound;

    return step;
}

ScriptStep ScriptStep::Write(uint32_t frame, uint16_t variable, int16_t value, uint32_t sound, uint16_t track)
{
    ScriptStep step;
    step.frame = frame;
    step.kind = kWrite;
    step.variable = variable;
    step.track = track;
    step.value = value;
    step.sound = sound;

    return step;
}

ScriptStep ScriptStep::WriteInProcedure(uint32_t frame, uint8_t procedure, uint16_t variable, int16_t value,
                                        uint32_t sound, uint16_t track)
{
    ScriptStep step = Write(frame, variable, value, sound, track);
    step.kind = kWriteInProcedure;
    step.procedure = procedure;

    return step;
}

ScriptedStart ScriptedStart::Plain(uint32_t sound)
{
    return {sound, 0, {ScriptStep::Start(0, sound)}};
}

const ScriptedStart* ArchiveDescriptor::ScriptOf(uint32_t sound) const
{
    for (const ScriptedStart& entry : scripts)
    {
        if (entry.sound == sound)
        {
            return &entry;
        }
    }

    return nullptr;
}

const SoundTrackVolumes* ArchiveDescriptor::TrackVolumesOf(uint32_t sound) const
{
    for (const SoundTrackVolumes& entry : track_volumes)
    {
        if (entry.sound == sound)
        {
            return &entry;
        }
    }

    return nullptr;
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

    // Versions 2 and 3 keep version 1's layout up to here. An older player then reads the version, and refuses it. A
    // version 1 descriptor's track volumes follow the paths, with their flag, and its scripted starts follow those,
    // with theirs. Its user procedures come last, with their flag. A player that doesn't know a flag refuses the
    // descriptor.
    if (version == kJaudioVersion)
    {
        PutJaudio(v, jaudio);
    }
    else if (version == kNw4rVersion)
    {
        PutNw4r(v, nw4r);
    }
    else if (version == kVersion)
    {
        if ((flags & kFlagTrackVolumes) != 0)
        {
            PutTrackVolumes(v, track_volumes);
        }

        if ((flags & kFlagScripts) != 0)
        {
            PutScripts(v, scripts, addons);
        }

        if ((flags & kFlagProcedures) != 0)
        {
            PutProcedures(v, procedures);
        }
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

    if (d.version == kNw4rVersion && !ReadNw4r(data, pos, d.nw4r))
    {
        return std::nullopt;
    }

    if (d.version == kVersion && (d.flags & kFlagTrackVolumes) != 0 && !ReadTrackVolumes(data, pos, d.track_volumes))
    {
        return std::nullopt;
    }

    if (d.version == kVersion && (d.flags & kFlagScripts) != 0 && !ReadScripts(data, pos, d.scripts, d.addons))
    {
        return std::nullopt;
    }

    if (d.version == kVersion && (d.flags & kFlagProcedures) != 0 && !ReadProcedures(data, pos, d.procedures))
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

    if (version == kNw4rVersion)
    {
        return CheckNw4r(*this);
    }

    if (version != kVersion)
    {
        return "archive descriptor version " + std::to_string(version) + "; this player supports versions 1 to 3";
    }

    // The archive's start, with the flags that version 1 has (CheckTrackVolumes) left out of xsflib's check of them.
    ArchiveDescriptor start = *this;
    start.flags = 0;
    if (auto err = xsflib::psf::CheckArchiveStart(start, kDefaultVoices))
    {
        return err;
    }

    if (auto err = CheckTrackVolumes(*this))
    {
        return err;
    }

    if (auto err = CheckScripts(*this))
    {
        return err;
    }

    if (auto err = CheckProcedures(*this))
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

        if (effect.type == AuxEffect::kReverb)
        {
            if (auto err = CheckReverb(effect))
            {
                return err;
            }

            continue;
        }

        if (effect.type != AuxEffect::kI3dl2Reverb)
        {
            return "an aux effect's type is " + std::to_string(effect.type) +
                   "; version 1 supports the I3DL2 reverb (1), the delay (2), the light limiter (3) and the reverb (4)";
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
