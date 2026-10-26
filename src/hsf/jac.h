// SPDX-License-Identifier: MIT

// What a library 3 descriptor sets up on the model of the C JAudio (src/jac): the engine's settings, the files, the
// game's start-up and the start of a sound. The player and the ripper's length analysis both set the model up with
// these functions. A rip's length then comes from the sound that the player plays.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "hsf/format.h"
#include "hsf/jaudio.h"
#include "jac/engine.h"

namespace hsf
{

// The engine's settings for a library 3 descriptor's JAudio settings. The mixers' level comes with the library
// (jac::Settings).
inline jac::Settings JacEngineSettings(const JaudioSettings& j)
{
    jac::Settings settings;
    settings.dsp_level = static_cast<uint16_t>(j.dsp_level);
    for (std::size_t i = 0; i < settings.fx_lines.size(); ++i)
    {
        const JaudioFxLine& line = j.fx_lines[i];
        settings.fx_lines[i] = {line.flags, line.subframes, line.buses, line.levels, line.filter};
    }

    return settings;
}

// The game's start-up for a library 3 descriptor.
inline jac::Startup JacStartup(const JaudioLibrary3& library)
{
    jac::Startup startup;
    for (const JacLoad& load : library.loads)
    {
        startup.loads.push_back({{load.scene.system, load.scene.scene}, load.set != 0});
    }
    startup.files = library.startup_files;
    startup.effect_level = library.effect_level;
    startup.cutscene_level = library.cutscene_level;

    return startup;
}

// The sound files of a library 3 set, from its FILE chunks: the bank file, the sequence archive, the sequence index
// that the descriptor names and the wave archives. `file(path)` gives a chunk's data, or null. Returns an error, or
// nothing. Throws xsflib::FormatError for a file that can't be read.
template <typename File>
std::optional<std::string> LoadJacFiles(const ArchiveDescriptor& d, const File& file,
                                        std::optional<jac::SoundFiles>& out)
{
    const std::vector<uint8_t>* bank_file = file(d.archive_path);
    if (bank_file == nullptr)
    {
        return "the set has no bank file " + d.archive_path;
    }

    const std::vector<uint8_t>* sequences = file(d.group_paths[ArchiveDescriptor::kSequenceArchive]);
    if (sequences == nullptr)
    {
        return "the set has no sequence archive " + d.group_paths[ArchiveDescriptor::kSequenceArchive];
    }

    const std::vector<uint8_t>* index = file(d.jaudio.library3.sequence_index);
    if (index == nullptr)
    {
        return "the set has no sequence index " + d.jaudio.library3.sequence_index;
    }

    const std::string folder = d.group_paths[ArchiveDescriptor::kWaveFolder];
    const auto waves = [file, folder](const std::string& name)
    {
        return file(JaudioWavePath(folder, name));
    };
    out.emplace(*bank_file, *sequences, *index, waves);

    return std::nullopt;
}

// The files of a library 3 set as the player keeps them: the set's FILE chunks by their paths, and the sound files.
// The sound files read the wave archives from the chunks as the game's code loads its scenes. The chunks therefore stay
// in place for as long as the sound files do.
struct JacFiles
{
    std::map<std::string, std::vector<uint8_t>> chunks;
    std::optional<jac::SoundFiles> sound;
};

// A library 3 descriptor gives an effect's kind, a cutscene's type and a value that the effect doesn't write as the
// model takes them.
static_assert(JacEffect::kSystem == jac::Effect::kSystem && JacEffect::kCaptain == jac::Effect::kCaptain &&
              JacEffect::kPikmin == jac::Effect::kPikmin && JacEffect::kEvent == jac::Effect::kEvent &&
              JacEffect::kFormation == jac::Effect::kFormation && JacEffect::kCrowd == jac::Effect::kCrowd &&
              JacEffect::kWalk == jac::Effect::kWalk && JacEffect::kCutscene == jac::Effect::kCutscene &&
              JacEffect::kCutscenePart == jac::Effect::kCutscenePart && JacEffect::kNone == jac::Effect::kNone);

// Starts sound `sound` of a library 3 set on `engine`, after the game's start-up: a piece of music, a piece of the
// cutscenes' music or an effect of the descriptor's lists, by the section in the ID's top 2 bits and the place in the
// low 16. The start-up's frames run up to the game's first frame. The sound starts in that frame, and its output
// starts with the next frame. `saved` is true when the game found a save (jac::Startup::saved). Rips play with a save.
// Returns an error, or nothing.
inline std::optional<std::string> StartJacSound(jac::Engine& engine, const JaudioLibrary3& library, uint32_t sound,
                                                bool saved = true)
{
    jac::Startup startup = JacStartup(library);
    startup.saved = saved;
    if (std::optional<std::string> error = engine.StartUp(startup))
    {
        return error;
    }

    // The game's first frame falls 14 sub-frames after the start-up. A start-up whose tracks never register the
    // events' track never gets there.
    constexpr int kStartupLimit = 1000;
    for (int i = 0; engine.GameFrames() == 0; i++)
    {
        if (i == kStartupLimit)
        {
            return std::string("the start-up's sequences never start the game's frames");
        }

        engine.RunFrame();
    }

    const uint32_t place = sound & 0xffff;
    if ((sound & 0x3fff0000u) != 0)
    {
        return std::string("the descriptor has no such sound");
    }

    switch (sound & 0xc0000000u)
    {
    case JaudioLibrary3::kPieceSection:
        {
            if (place >= library.pieces.size())
            {
                return std::string("the descriptor has no such piece");
            }

            const JacPiece& p = library.pieces[place];
            const float music_volume = saved ? library.music_volume : jac::kDataMusicVolume;
            return engine.StartPiece(
                {p.file, p.playing_tracks, p.volume, {p.scene.system, p.scene.scene}, music_volume});
        }

    case JaudioLibrary3::kPartSection:
        {
            if (place >= library.parts.size())
            {
                return std::string("the descriptor has no such piece of the cutscenes' music");
            }

            const JacPart& p = library.parts[place];
            return engine.StartPart(p.number, {p.scene.system, p.scene.scene});
        }

    case JaudioLibrary3::kEffectSection:
        {
            if (place >= library.effects.size())
            {
                return std::string("the descriptor has no such effect");
            }

            const JacEffect& e = library.effects[place];
            return engine.StartEffect({e.kind, e.type, e.value, e.first, e.finish});
        }

    default:
        return std::string("the descriptor has no such sound");
    }
}

} // namespace hsf
