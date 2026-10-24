// SPDX-License-Identifier: MIT

// What a version 2 archive descriptor sets up on the model of JAudio (src/jaudio): the engine's settings, the track
// states of a sound, and the files. The player and the ripper's length analysis both set the model up with these
// functions. A rip's length then comes from the sound that the player plays.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "hsf/format.h"
#include "jaudio/engine.h"

namespace hsf
{

// The engine's settings for a version 2 descriptor's JAudio settings.
inline jaudio::Settings JaudioEngineSettings(const JaudioSettings& j)
{
    jaudio::Settings settings;
    settings.dsp_level = static_cast<uint16_t>(j.dsp_level);
    settings.music_volume = j.music_volume;
    settings.effect_volume = j.effect_volume;
    settings.category_volumes = j.category_volumes;
    for (std::size_t i = 0; i < settings.fx_lines.size(); ++i)
    {
        const JaudioFxLine& line = j.fx_lines[i];
        settings.fx_lines[i] = {line.flags, line.subframes, line.buses, line.levels, line.filter};
    }
    for (const JaudioSoundVolume& sound : j.sound_volumes)
    {
        settings.sound_volumes[sound.sound] = static_cast<uint8_t>(sound.volume);
    }

    return settings;
}

// The track states that the settings give sound `sound`, or none.
inline jaudio::TrackStates TrackStatesFor(const JaudioSettings& j, uint32_t sound)
{
    for (const JaudioTrackStates& entry : j.track_states)
    {
        if (entry.sound == sound)
        {
            return jaudio::TrackStates(entry.states.begin(), entry.states.end());
        }
    }

    return {};
}

// The path of a wave archive's FILE chunk: the folder of the wave archives, a slash and the archive's name. An empty
// folder gives the name alone.
inline std::string JaudioWavePath(const std::string& folder, const std::string& name)
{
    return folder.empty() ? name : folder + "/" + name;
}

// The sound files of a version 2 set, from its FILE chunks: the start-up archive, the sequence archive and the wave
// archives that the descriptor names. `file(path)` gives a chunk's data, or null. The sequence archive may be missing.
// Returns an error, or nothing. Throws xsflib::FormatError for a file that can't be read.
template <typename File>
std::optional<std::string> LoadJaudioFiles(const ArchiveDescriptor& d, const File& file,
                                           std::optional<jaudio::SoundFiles>& out)
{
    const std::vector<uint8_t>* startup = file(d.archive_path);
    if (startup == nullptr)
    {
        return "the set has no start-up archive " + d.archive_path;
    }

    const std::vector<uint8_t>* sequences = file(d.group_paths[ArchiveDescriptor::kSequenceArchive]);
    const std::string& folder = d.group_paths[ArchiveDescriptor::kWaveFolder];
    const auto waves = [&file, &folder](const std::string& name)
    {
        return file(JaudioWavePath(folder, name));
    };
    out.emplace(*startup, sequences ? std::span<const uint8_t>(*sequences) : std::span<const uint8_t>(), waves);

    return std::nullopt;
}

} // namespace hsf
