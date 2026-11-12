// SPDX-License-Identifier: MIT

// The HSF file format (see format.h and docs/hsf.md).

#include "hsf/format.h"

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

namespace hsf
{
namespace
{

using xsflib::psf::Put16;
using xsflib::psf::Put32;

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
    pos = detail::AlignTo4(pos);
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
    return xsflib::psf::FindTrackVolumes(track_volumes, sound);
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
        detail::PutJaudio(v, jaudio);
    }
    else if (version == kNw4rVersion)
    {
        PutNw4r(v, nw4r);
    }
    else if (version == kVersion)
    {
        if ((flags & kFlagTrackVolumes) != 0)
        {
            xsflib::psf::PutTrackVolumes(v, track_volumes);
        }

        if ((flags & kFlagScripts) != 0)
        {
            detail::PutScripts(v, scripts, addons);
        }

        if ((flags & kFlagProcedures) != 0)
        {
            detail::PutProcedures(v, procedures);
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

    if (d.version == kJaudioVersion && !detail::ReadJaudio(data, pos, d.jaudio))
    {
        return std::nullopt;
    }

    if (d.version == kNw4rVersion && !ReadNw4r(data, pos, d.nw4r))
    {
        return std::nullopt;
    }

    // Version 1's track volumes follow the paths' padding.
    if (d.version == kVersion && (d.flags & kFlagTrackVolumes) != 0)
    {
        pos = detail::AlignTo4(pos);
        if (!xsflib::psf::ReadTrackVolumes(data, pos, d.track_volumes))
        {
            return std::nullopt;
        }
    }

    if (d.version == kVersion && (d.flags & kFlagScripts) != 0 && !detail::ReadScripts(data, pos, d.scripts, d.addons))
    {
        return std::nullopt;
    }

    if (d.version == kVersion && (d.flags & kFlagProcedures) != 0 && !detail::ReadProcedures(data, pos, d.procedures))
    {
        return std::nullopt;
    }

    return d;
}

std::optional<std::string> ArchiveDescriptor::Check() const
{
    if (version == kJaudioVersion)
    {
        return detail::CheckJaudio(*this);
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

    if (auto err = detail::CheckTrackVolumes(*this))
    {
        return err;
    }

    if (auto err = detail::CheckScripts(*this))
    {
        return err;
    }

    if (auto err = detail::CheckProcedures(*this))
    {
        return err;
    }

    return detail::CheckEffects(*this);
}

} // namespace hsf
