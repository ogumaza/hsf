// SPDX-License-Identifier: MIT

// hsfrip's rips of Wii sound archives (see rsar_rip.h).

#include "cli/rsar_rip.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rsar/cut.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/music.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "xsflib/binary.h"
#include "xsflib/cli/rip.h"
#include "xsflib/psf/psf.h"

#include "cli/game_settings.h"
#include "game/game.h"
#include "hsf/format.h"
#include "rip/nw4r_rip.h"
#include "rip/rip.h"

namespace hsf::cli
{

namespace
{

namespace fs = std::filesystem;

// The SDK version of Skyward Sword HD's sdk, NintendoSDK 11.4.0, for a version 3 descriptor.
constexpr uint32_t kSkywardSwordSdk = 110400;

// What a Wii sound archive holds, such as "RSAR, 790 sequences (61 music), 2 wave sounds, 3412 streams".
std::string DescribeRsar(const ntdlib::rsar::SoundArchive& archive)
{
    std::size_t sequences = 0;
    std::size_t music = 0;
    std::size_t waves = 0;
    std::size_t streams = 0;
    const std::vector<ntdlib::rsar::SoundInfo>& sounds = archive.Sounds();
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        sequences += sounds[i].type == ntdlib::rsar::SoundType::kSequence ? 1 : 0;
        music += ntdlib::rsar::IsMusic(archive, static_cast<uint32_t>(i)) ? 1 : 0;
        waves += sounds[i].type == ntdlib::rsar::SoundType::kWaveSound ? 1 : 0;
        streams += sounds[i].type == ntdlib::rsar::SoundType::kStream ? 1 : 0;
    }

    return "RSAR, " + std::to_string(sequences) + " sequences (" + std::to_string(music) + " music), " +
           std::to_string(waves) + " wave sounds, " + std::to_string(streams) + " streams";
}

// Drops the sequences whose data `archive` doesn't hold, with a line for each. Returns how many it dropped.
std::size_t DropMissingData(const ntdlib::rsar::SoundArchive& archive, std::vector<hsf::rip::Nw4rJob>& jobs)
{
    std::size_t missing = 0;
    std::vector<hsf::rip::Nw4rJob> complete;
    for (hsf::rip::Nw4rJob& job : jobs)
    {
        const std::vector<std::string> lacks = ntdlib::rsar::MissingData(archive, job.index);
        if (lacks.empty())
        {
            complete.push_back(std::move(job));
            continue;
        }

        missing++;
        std::string what;
        for (const std::string& item : lacks)
        {
            what += (what.empty() ? "" : ", ") + item;
        }
        std::fprintf(stderr, "skipped %s: the archive doesn't hold %s\n", job.label.c_str(), what.c_str());
    }

    jobs = std::move(complete);

    return missing;
}

// The part of `archive` that the sequences of `jobs` play. Nothing after an error message about the archive at
// `source`.
std::optional<std::vector<uint8_t>> SequenceData(const ntdlib::rsar::SoundArchive& archive,
                                                 const std::vector<hsf::rip::Nw4rJob>& jobs, const char* source)
{
    std::vector<uint32_t> indices;
    for (const hsf::rip::Nw4rJob& job : jobs)
    {
        indices.push_back(job.index);
    }

    try
    {
        return ntdlib::rsar::KeepSequenceData(archive, indices, ntdlib::rvl::snd::kRsarOptions);
    }
    catch (const xsflib::FormatError& e)
    {
        std::fprintf(stderr, "error: %s: %s\n", source, e.what());
        return std::nullopt;
    }
}

} // namespace

int RipRsar(FileChunk rsar, const std::string& game_name, const fs::path& out_dir, const Options& o,
            xsflib::cli::FileNameRegistries& registries)
{
    const std::string path = rsar.path;
    const char* source = path.c_str();
    std::optional<ntdlib::rsar::SoundArchive> loaded;
    try
    {
        loaded.emplace(ntdlib::rsar::SoundArchive::Load(std::move(rsar.data), ntdlib::rvl::snd::kRsarOptions));
    }
    catch (const xsflib::FormatError& e)
    {
        std::fprintf(stderr, "error: %s: %s\n", source, e.what());
        return 1;
    }

    const ntdlib::rsar::SoundArchive& archive = *loaded;
    if (o.list)
    {
        std::printf("  %s: %s\n", source, DescribeRsar(archive).c_str());
        return 0;
    }

    std::fprintf(stderr, "%s: %s; NW4R SND on Skyward Sword HD's layer, with the game's reverbs on the aux buses\n",
                 source, DescribeRsar(archive).c_str());
    if (o.voices > 0 || !o.effects)
    {
        std::fprintf(stderr,
                     "warning: %s: --voices and --no-effects don't apply to a Wii sound archive. It plays with the "
                     "game's voices and reverbs.\n",
                     source);
    }

    std::vector<hsf::rip::Nw4rJob> jobs = hsf::rip::SelectNw4rSounds(archive, o.only, o.bgm);
    if (jobs.empty())
    {
        return NoSoundsSelected(path, o);
    }

    const std::size_t missing = DropMissingData(archive, jobs);
    const hsf::ArchiveDescriptor descriptor = hsf::ArchiveDescriptor::Nw4r(hsf::Nw4rSettings{}, path, kSkywardSwordSdk);
    std::size_t silent = 0;
    if (o.lengths)
    {
        const auto analyze = [&archive, &descriptor](hsf::rip::Nw4rJob& job)
        {
            job.timing = hsf::rip::AnalyzeNw4rLength(archive, descriptor.nw4r, job.index);
            return true;
        };
        xsflib::cli::AnalyzeLengths(jobs, o.jobs, analyze);
        silent = DropSilent(jobs, [](const hsf::rip::Nw4rJob&) {});
    }

    const std::string skipped_text = xsflib::cli::SkippedText(missing, "data not there", silent, "sequence");
    if (jobs.empty())
    {
        std::fprintf(stderr, "%s: nothing to rip%s\n", source, skipped_text.c_str());
        return 0;
    }

    // The library holds the part of the archive that the sequences play.
    FileChunk kept;
    kept.path = path;
    std::optional<std::vector<uint8_t>> data = SequenceData(archive, jobs, source);
    if (!data)
    {
        return 1;
    }

    kept.data = std::move(*data);

    const auto build_library = [&kept, &descriptor](const xsflib::psf::Tags& common)
    {
        return hsf::rip::BuildNw4rLibrary(kept, descriptor, common);
    };
    const auto build_mini = [](const hsf::rip::Nw4rJob& job, const std::string& lib_name, const xsflib::psf::Tags& tags)
    {
        return hsf::rip::BuildNw4rMini(job, lib_name, tags);
    };

    return WriteRip(path, game_name, jobs, skipped_text, out_dir, o, registries, build_library, build_mini);
}

int RipRsarGame(const std::string& input, const hsf::game::GameFiles& game, const std::vector<std::string>& archives,
                const fs::path& out_dir, const Options& o, xsflib::cli::FileNameRegistries& registries)
{
    if (RefuseMissingArchive(archives, o))
    {
        return 1;
    }

    if (o.list)
    {
        std::printf("%s: %zu files, %zu Wii sound archive%s\n", input.c_str(), game.files.size(), archives.size(),
                    archives.size() == 1 ? "" : "s");
    }

    const std::string game_name = o.game_name.empty() ? game.name : o.game_name;
    int result = 0;
    for (const std::string& path : archives)
    {
        if (!o.only_archive.empty() && path != o.only_archive)
        {
            continue;
        }

        FileChunk rsar;
        rsar.path = path;
        if (!game.read_file(path, rsar.data))
        {
            std::fprintf(stderr, "error: can't read %s\n", path.c_str());
            result = 1;
            continue;
        }

        if (!hsf::rip::IsRsar(rsar.data))
        {
            std::fprintf(stderr, "skipped %s: it isn't a Wii sound archive\n", path.c_str());
            continue;
        }

        if (RipRsar(std::move(rsar), game_name, out_dir, o, registries) != 0)
        {
            result = 1;
        }
    }

    return result;
}

} // namespace hsf::cli
