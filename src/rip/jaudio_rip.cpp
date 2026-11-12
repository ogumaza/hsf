// SPDX-License-Identifier: MIT

// hsfrip's parts for games that play their sound with JAudio (see jaudio_rip.h).

#include "rip/jaudio_rip.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/jaudio/archive.h"
#include "ntdlib/jaudio/rarc.h"
#include "ntdlib/yaz0.h"
#include "xsflib/ascii.h"
#include "xsflib/text.h"

#include "hsf/format.h"
#include "rip/game_paths.h"

namespace hsf::rip
{

namespace nj = ntdlib::jaudio;

bool IsStartupArchive(std::span<const uint8_t> data)
{
    std::vector<uint8_t> decoded;
    std::span<const uint8_t> bytes = data;
    if (ntdlib::IsYaz0(data))
    {
        try
        {
            bytes = ntdlib::DecodeIfYaz0(data, decoded);
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    const std::string_view head(reinterpret_cast<const char*>(bytes.data()), std::min<std::size_t>(bytes.size(), 4));
    return head == "AA_<" || head == "<_AA";
}

std::optional<JaudioPaths> FindJaudioFiles(const std::vector<std::string>& files, const ReadGameFile& read)
{
    for (const std::string& path : files)
    {
        const std::string folder = FolderOf(path);
        if (xsflib::AsciiLower(FileNameOf(folder)) != "audiores" ||
            !(EndsWithLower(path, ".szs") || EndsWithLower(path, ".baa")))
        {
            continue;
        }

        std::vector<uint8_t> data;
        if (!read(path, data) || !IsStartupArchive(data))
        {
            continue;
        }

        JaudioPaths paths;
        paths.startup = path;
        paths.waves = folder + "/Waves";
        const std::string seqs = xsflib::AsciiLower(folder + "/Seqs/");
        for (const std::string& file : files)
        {
            if (xsflib::AsciiLower(file).starts_with(seqs) && EndsWithLower(file, ".arc") &&
                file.find('/', seqs.size()) == std::string::npos)
            {
                paths.sequences = file;
                break;
            }
        }

        return paths;
    }

    return std::nullopt;
}

std::string JaudioLabel(const nj::SoundEntry& sound)
{
    return sound.name.empty() ? "sound " + xsflib::Hex(sound.Id(), 8) : sound.name;
}

std::vector<JaudioJob> SelectJaudioSounds(const jaudio::SoundFiles& files, const JaudioGame* game,
                                          const std::regex& only, bool bgm)
{
    std::vector<JaudioJob> jobs;
    const std::vector<nj::SoundEntry>& sounds = files.Archive().Sounds();
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        const nj::SoundEntry& sound = sounds[i];
        const uint32_t group = uint32_t{sound.section} << 8 | sound.group;
        if (game != nullptr &&
            std::find(game->replaced_groups.begin(), game->replaced_groups.end(), group) != game->replaced_groups.end())
        {
            continue;
        }

        if ((bgm && sound.section != kMusicSection) || files.SequenceOf(sound).sequence == nullptr)
        {
            continue;
        }

        JaudioJob job;
        job.index = static_cast<uint32_t>(i);
        job.label = JaudioLabel(sound);
        job.id = sound.Id();
        if (std::regex_search(job.label, only))
        {
            jobs.push_back(job);
        }

        if (game == nullptr)
        {
            continue;
        }

        for (const JaudioVariant& variant : game->variants)
        {
            JaudioJob variant_job = job;
            variant_job.label = job.label + " " + variant.name;
            variant_job.sound_label = job.label;
            variant_job.track_states = variant.states;
            if (variant.states.sound == job.id && std::regex_search(variant_job.label, only))
            {
                jobs.push_back(std::move(variant_job));
            }
        }
    }

    return jobs;
}

ArchiveDescriptor JobDescriptor(ArchiveDescriptor descriptor, const JaudioJob& job)
{
    std::vector<JaudioPiece>& pieces = descriptor.jaudio.library2.pieces;
    const uint32_t place = job.id & kSoundPlaceMask;
    if (job.muted_tracks && (job.id & kSoundSectionMask) == JaudioLibrary2::kPieceSection && place < pieces.size())
    {
        pieces[place].muted_tracks = *job.muted_tracks;
    }

    if (!job.track_states)
    {
        return descriptor;
    }

    std::vector<JaudioTrackStates>& states = descriptor.jaudio.track_states;
    const auto same_sound = [&job](const JaudioTrackStates& entry)
    {
        return entry.sound == job.track_states->sound;
    };
    const auto entry = std::find_if(states.begin(), states.end(), same_sound);
    if (entry != states.end())
    {
        *entry = *job.track_states;
    }
    else
    {
        states.push_back(*job.track_states);
    }

    return descriptor;
}

std::vector<uint8_t> BuildJaudioLibrary(const FileChunk& startup, const std::optional<FileChunk>& sequences,
                                        const std::vector<FileChunk>& waves, ArchiveDescriptor descriptor,
                                        const xsflib::psf::Tags& tags)
{
    ProgramBuilder program;
    program.AddFileRef(startup.path, startup.data);
    if (sequences)
    {
        program.AddFileRef(sequences->path, sequences->data);
    }

    for (const FileChunk& wave : waves)
    {
        program.AddFileRef(wave.path, wave.data);
    }

    return program.WritePsf(kPsfVersionHsf, descriptor.Serialize(), tags);
}

std::vector<uint8_t> BuildJaudioMini(const JaudioJob& job, const ArchiveDescriptor& descriptor,
                                     const std::string& lib_name, xsflib::psf::Tags tags)
{
    ProgramBuilder program;
    program.AddSound(job.id);
    tags["_lib"] = lib_name;
    const std::vector<uint8_t> reserved =
        IsVariant(job) ? JobDescriptor(descriptor, job).Serialize() : std::vector<uint8_t>();

    return program.WritePsf(kPsfVersionHsf, reserved, tags);
}

} // namespace hsf::rip
