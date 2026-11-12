// SPDX-License-Identifier: MIT

// Version 2 of the archive descriptor: the JAudio settings, with the fields of libraries 2 and 3 (see format.h and
// docs/hsf.md).

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/psf/descriptor.h"
#include "xsflib/psf/psf.h"

#include "hsf/descriptor_parts.h"
#include "hsf/format.h"

namespace hsf::detail
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
        pos = AlignTo4(pos);
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
    if (!std::all_of(j.category_volumes.begin(), j.category_volumes.end(), IsUnity))
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
    pos = AlignTo4(pos);
    if (!index || pos > data.size() || data.size() - pos < kLevelsBytes)
    {
        return false;
    }

    LeCursor in(data, pos);
    const auto scene = [&in]
    {
        JacScene s;
        s.system = in.U16();
        s.scene = in.U16();
        return s;
    };

    // The count of a list whose entries take `bytes` each, or no value if the data ends first.
    const auto count = [&data, &pos, &in](std::size_t bytes) -> std::optional<uint32_t>
    {
        if (data.size() - pos < 4)
        {
            return std::nullopt;
        }

        const uint32_t n = in.U32();
        return n <= (data.size() - pos) / bytes ? std::optional<uint32_t>(n) : std::nullopt;
    };

    l.sequence_index = std::move(*index);
    for (uint16_t& file : l.startup_files)
    {
        file = in.U16();
    }
    l.effect_level = in.U16();
    l.cutscene_level = in.U16();
    l.music_volume = in.F32();

    const std::optional<uint32_t> loads = count(kLoadBytes);
    if (!loads)
    {
        return false;
    }

    l.loads.resize(*loads);
    for (JacLoad& load : l.loads)
    {
        load.scene = scene();
        load.set = in.U32();
    }

    const std::optional<uint32_t> pieces = count(kJacPieceBytes);
    if (!pieces)
    {
        return false;
    }

    l.pieces.resize(*pieces);
    for (JacPiece& piece : l.pieces)
    {
        piece.file = in.U16();
        piece.playing_tracks = in.U16();
        piece.volume = in.F32();
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
        part.number = in.U16();
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
        effect.value = in.U16();
        effect.first = in.U16();
        effect.finish = in.U16();
    }

    return true;
}

// Why a version 2 player can't play a library 3 descriptor's settings, or nothing. Library 3 has no managers' or
// categories' volumes, no track states and no sounds' volumes. Its FX lines are the game's.
std::optional<std::string> CheckLibrary3(const JaudioSettings& j)
{
    if (!IsUnity(j.music_volume) || !IsUnity(j.effect_volume) ||
        !std::all_of(j.category_volumes.begin(), j.category_volumes.end(), IsUnity))
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

    if (!IsVolume(l.music_volume) ||
        !std::all_of(l.pieces.begin(), l.pieces.end(), [&](const JacPiece& p) { return IsVolume(p.volume); }))
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

} // namespace

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

bool ReadJaudio(const std::vector<uint8_t>& data, std::size_t pos, JaudioSettings& j)
{
    pos = AlignTo4(pos);
    if (pos > data.size() || data.size() - pos < kJaudioBytes)
    {
        return false;
    }

    LeCursor in(data, pos);
    j.library = in.U32();
    j.dsp_level = in.U32();
    j.music_volume = in.F32();
    j.effect_volume = in.F32();
    for (float& volume : j.category_volumes)
    {
        volume = in.F32();
    }

    for (JaudioFxLine& line : j.fx_lines)
    {
        line.flags = in.U16();
        line.subframes = in.U16();
        for (std::size_t i = 0; i < 2; ++i)
        {
            line.buses[i] = in.U16();
            line.levels[i] = in.U16();
        }
        for (int16_t& coefficient : line.filter)
        {
            coefficient = static_cast<int16_t>(in.U16());
        }
    }

    const uint32_t count = in.U32();
    if (count > (data.size() - pos) / kTrackStatesBytes)
    {
        return false;
    }

    j.track_states.resize(count);
    for (JaudioTrackStates& sound : j.track_states)
    {
        sound.sound = in.U32();
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

    const uint32_t volumes = in.U32();
    if (volumes > (data.size() - pos) / kSoundVolumeBytes)
    {
        return false;
    }

    j.sound_volumes.resize(volumes);
    for (JaudioSoundVolume& sound : j.sound_volumes)
    {
        sound.sound = in.U32();
        sound.volume = in.U32();
    }

    if (j.library == JaudioSettings::kPikmin2Library)
    {
        return ReadLibrary2(data, pos, j.library2);
    }

    return j.library != JaudioSettings::kPikminLibrary || ReadLibrary3(data, pos, j.library3);
}

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

    if (!IsVolume(j.music_volume) || !IsVolume(j.effect_volume) ||
        !std::all_of(j.category_volumes.begin(), j.category_volumes.end(), IsVolume))
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

} // namespace hsf::detail
