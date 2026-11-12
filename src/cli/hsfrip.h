// SPDX-License-Identifier: MIT

// The parts of hsfrip that its rips of each kind of input share: the options, what a rip does when its selection holds
// no sound, the drop of the sounds that make none, and the writing of a rip.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rip/archive.h"
#include "xsflib/cli/rip.h"
#include "xsflib/psf/psf.h"
#include "xsflib/rip/tool.h"

#include "hsf/format.h"

namespace hsf::cli
{

// The most voices a set can have. It's also nn::atk's voice count when the game doesn't change it.
constexpr int kMaxVoices = static_cast<int>(hsf::ArchiveDescriptor::kDefaultVoices);

// The rip frame's options, and HSF's.
struct Options : xsflib::cli::RipOptions
{
    bool effects = true;
    int voices = 0; // 0: the game's voice count, or 96
};

// The exit code when a selection of sounds holds none. An --only or --bgm that matches nothing is an error. Otherwise
// the input has nothing to rip. `source` is the file that the sounds come from.
inline int NoSoundsSelected(const std::string& source, const Options& o)
{
    if (!o.only_text.empty() || o.bgm)
    {
        return ntdlib::rip::NoSoundMatches(o.only_text, o.bgm);
    }

    std::fprintf(stderr, "%s: nothing to rip\n", source.c_str());
    return 0;
}

// Drops the jobs that the length analysis found make no sound, each with a message, and calls `on_kept(job)` for each
// job that stays. Returns how many it dropped.
template <class Job, class OnKept>
std::size_t DropSilent(std::vector<Job>& jobs, OnKept on_kept)
{
    std::size_t silent = 0;
    std::vector<Job> kept;
    for (Job& job : jobs)
    {
        if (job.timing && job.timing->silent)
        {
            silent++;
            std::fprintf(stderr, "skipped %s: it makes no sound\n", job.label.c_str());
            continue;
        }

        on_kept(job);
        kept.push_back(std::move(job));
    }

    jobs = std::move(kept);

    return silent;
}

// Writes the rip of the archive or file at `path` into the folder of `out_dir` named after it: the library that
// `build_library(common_tags)` makes, and a mini for each of `jobs` that `build_mini(job, library_name, tags)` makes.
// The messages name `path`, and the last ends with `skipped_text`. Returns the exit code.
template <class Job, class BuildLibrary, class BuildMini>
int WriteRip(const std::string& path, const std::string& game_name, const std::vector<Job>& jobs,
             const std::string& skipped_text, const std::filesystem::path& out_dir, const Options& o,
             xsflib::cli::FileNameRegistries& registries, BuildLibrary build_library, BuildMini build_mini)
{
    const std::string folder = xsflib::rip::ArchiveFolders({path}).front();
    const std::filesystem::path rip_dir = out_dir / folder;
    xsflib::rip::FileNames& names = xsflib::cli::NamesIn(registries, rip_dir, ".hsflib", ".minihsf");
    const xsflib::psf::Tags common = xsflib::cli::RipTags(o, game_name, "hsf", nullptr);
    const std::vector<uint8_t> lib = build_library(common);
    const std::optional<std::string> lib_name =
        xsflib::cli::WriteLibrary(rip_dir, names, game_name + " " + folder, lib);
    if (!lib_name)
    {
        return 1;
    }

    const auto build = [&lib_name, &build_mini](const Job& job, const xsflib::psf::Tags& tags)
    {
        return build_mini(job, *lib_name, tags);
    };
    const int written = xsflib::cli::WriteMinis(rip_dir, names, jobs, common, "hsf_sound", build);
    if (written < 0)
    {
        return 1;
    }

    std::fprintf(stderr, "%s: %d minihsf files%s\n", path.c_str(), written, skipped_text.c_str());
    xsflib::rip::ReportOutput(out_dir);

    return 0;
}

} // namespace hsf::cli
