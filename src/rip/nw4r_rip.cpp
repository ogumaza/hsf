// SPDX-License-Identifier: MIT

// hsfrip's parts for a Wii sound archive (see nw4r_rip.h).

#include "rip/nw4r_rip.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/nwsnd/length_model.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/music.h"
#include "ntdlib/rvl/nx/length_model.h"
#include "ntdlib/sound_build.h"
#include "xsflib/ascii.h"
#include "xsflib/rip/length.h"

namespace hsf::rip
{

bool IsRsar(std::span<const uint8_t> data)
{
    return data.size() >= 4 && std::equal(data.begin(), data.begin() + 4, "RSAR");
}

std::vector<std::string> FindRsarFiles(const std::vector<std::string>& files)
{
    std::vector<std::string> found;
    for (const std::string& file : files)
    {
        if (xsflib::AsciiLower(file).ends_with(".brsar"))
        {
            found.push_back(file);
        }
    }

    return found;
}

std::vector<Nw4rJob> SelectNw4rSounds(const ntdlib::rsar::SoundArchive& archive, const std::regex& only, bool bgm)
{
    std::vector<Nw4rJob> jobs;
    const std::vector<ntdlib::rsar::SoundInfo>& sounds = archive.Sounds();
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        const auto index = static_cast<uint32_t>(i);
        if (sounds[i].type != ntdlib::rsar::SoundType::kSequence || (bgm && !ntdlib::rsar::IsMusic(archive, index)))
        {
            continue;
        }

        const std::string label = sounds[i].name.empty() ? "SEQ_" + std::to_string(i) : sounds[i].name;
        if (std::regex_search(label, only))
        {
            jobs.push_back({index, label, std::nullopt});
        }
    }

    return jobs;
}

std::optional<Timing> AnalyzeNw4rLength(const ntdlib::rsar::SoundArchive& archive, const Nw4rSettings& settings,
                                        uint32_t index)
{
    using LengthConsole = ntdlib::rvl::nx::LengthConsole;
    const auto make_model = [&archive, &settings]
    {
        auto model = std::make_unique<LengthConsole::Model>();
        model->build = ntdlib::SndBuild{settings.snd_build, false};
        model->Start(archive);
        return model;
    };
    const auto measure = [&make_model, index]
    {
        return ntdlib::nwsnd::MeasureSequence<LengthConsole>(make_model, index);
    };

    return xsflib::rip::TryMeasure(measure);
}

std::vector<uint8_t> BuildNw4rLibrary(const FileChunk& archive, const ArchiveDescriptor& descriptor,
                                      const xsflib::psf::Tags& tags)
{
    ProgramBuilder program;
    program.AddFileRef(archive.path, archive.data);

    return program.WritePsf(kPsfVersionHsf, descriptor.Serialize(), tags);
}

std::vector<uint8_t> BuildNw4rMini(const Nw4rJob& job, const std::string& lib_name, xsflib::psf::Tags tags)
{
    ProgramBuilder program;
    program.AddSound(job.index);
    tags["_lib"] = lib_name;

    return program.WritePsf(kPsfVersionHsf, {}, tags);
}

} // namespace hsf::rip
