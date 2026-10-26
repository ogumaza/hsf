// SPDX-License-Identifier: MIT

// What a library 2 descriptor sets up on the model of JAudio1 (src/jaudio1): the engine's settings, the files and the
// start of a sound. The player and the ripper's length analysis both set the model up with these functions. A rip's
// length then comes from the sound that the player plays.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/jaudio/rarc.h"

#include "hsf/format.h"
#include "hsf/jaudio.h"
#include "jaudio1/engine.h"

namespace hsf
{

// The engine's settings for a library 2 descriptor's JAudio settings. The levels of the channels come with the library
// (jaudio1::Settings).
inline jaudio1::Settings Jaudio1EngineSettings(const JaudioSettings& j)
{
    jaudio1::Settings settings;
    settings.dsp_level = static_cast<uint16_t>(j.dsp_level);
    settings.music_volume = j.music_volume;
    settings.effect_volume = j.effect_volume;

    return settings;
}

// The files of a library 2 set: the sound files, and the conductor archive's files by their names.
struct Jaudio1Files
{
    template <typename Waves>
    Jaudio1Files(std::span<const uint8_t> startup, std::span<const uint8_t> sequences, const Waves& waves)
        : sound(startup, sequences, waves)
    {
    }

    jaudio1::SoundFiles sound;
    std::map<std::string, std::vector<uint8_t>> conductors;
};

// The sound files of a library 2 set, from its FILE chunks: the start-up archive, the sequence archive, the wave
// archives and the conductor archive that the descriptor names. `file(path)` gives a chunk's data, or null. Returns an
// error, or nothing. Throws xsflib::FormatError for a file that can't be read.
template <typename File>
std::optional<std::string> LoadJaudio1Files(const ArchiveDescriptor& d, const File& file,
                                            std::optional<Jaudio1Files>& out)
{
    const std::vector<uint8_t>* startup = file(d.archive_path);
    if (startup == nullptr)
    {
        return "the set has no start-up archive " + d.archive_path;
    }

    const std::vector<uint8_t>* sequences = file(d.group_paths[ArchiveDescriptor::kSequenceArchive]);
    if (sequences == nullptr)
    {
        return "the set has no sequence archive " + d.group_paths[ArchiveDescriptor::kSequenceArchive];
    }

    const std::string& conductor_archive = d.jaudio.library2.conductor_archive;
    const std::vector<uint8_t>* conductors = conductor_archive.empty() ? nullptr : file(conductor_archive);
    if (!conductor_archive.empty() && conductors == nullptr)
    {
        return "the set has no conductor archive " + conductor_archive;
    }

    const std::string& folder = d.group_paths[ArchiveDescriptor::kWaveFolder];
    const auto waves = [&file, &folder](const std::string& name)
    {
        return file(JaudioWavePath(folder, name));
    };
    out.emplace(*startup, *sequences, waves);
    if (conductors != nullptr)
    {
        std::vector<uint8_t> decoded;
        for (const ntdlib::jaudio::ArchiveFile& entry : ntdlib::jaudio::ReadRarc(
                 ntdlib::jaudio::DecodeIfYaz0(*conductors, decoded), ntdlib::jaudio::kWideRarcEntrySize))
        {
            out->conductors.try_emplace(entry.name, entry.data.begin(), entry.data.end());
        }
    }

    return std::nullopt;
}

// Starts the sequence of sound `sound` for StartJaudio1Sound.
inline std::optional<std::string> StartJaudio1Sequence(jaudio1::Engine& engine, const Jaudio1Files& files,
                                                       const JaudioLibrary2& library, uint32_t sound)
{
    if ((sound & 0xc0000000u) != JaudioLibrary2::kPieceSection)
    {
        return engine.StartEffect(library.effect_sequence, library.effect_sequence_volume, sound);
    }

    const uint32_t place = sound & 0xffff;
    if ((sound & 0x3fff0000u) != 0 || place >= library.pieces.size())
    {
        return std::string("the descriptor has no such piece");
    }

    const JaudioPiece& piece = library.pieces[place];
    jaudio1::PieceSettings settings;
    settings.kind = piece.kind;
    settings.volume = piece.volume;
    settings.kept_levels = piece.kept_levels;
    settings.beat_interval = piece.beat_interval;
    settings.muted_tracks = piece.muted_tracks;
    settings.start_point = piece.start_point;
    if (!piece.conductor.empty())
    {
        const auto conductor = files.conductors.find(piece.conductor);
        if (conductor == files.conductors.end())
        {
            return "the conductor archive has no file " + piece.conductor;
        }

        settings.conductor = conductor->second;
    }

    return engine.Start(piece.file, settings);
}

// Starts sound `sound` of a library 2 set on `engine`: a piece of the descriptor's list by its place, with section 2
// in the ID's top bits, or an effect of the sound table on the effect sequence. A sound that the descriptor lists with
// draws starts after the sequences' generator has drawn them. With `draws`, the sound starts after that many draws
// instead. Returns an error, or nothing.
inline std::optional<std::string> StartJaudio1Sound(jaudio1::Engine& engine, const Jaudio1Files& files,
                                                    const JaudioLibrary2& library, uint32_t sound,
                                                    std::optional<uint32_t> draws = std::nullopt)
{
    std::optional<std::string> error = StartJaudio1Sequence(engine, files, library, sound);
    if (!error)
    {
        engine.DrawSequenceNumbers(draws.value_or(library.DrawsBefore(sound)));
    }

    return error;
}

} // namespace hsf
