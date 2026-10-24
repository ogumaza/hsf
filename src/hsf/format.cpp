// SPDX-License-Identifier: MIT

// The HSF file format (see format.h and docs/hsf.md).

#include "hsf/format.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/psf/descriptor.h"
#include "xsflib/psf/psf.h"

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

    return true;
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
    if (j.library != JaudioSettings::kGalaxyLibrary)
    {
        return "the archive descriptor's library is " + std::to_string(j.library) +
               "; this player supports library 1 (the Super Mario Galaxy games' JAudio2)";
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

} // namespace

ArchiveDescriptor ArchiveDescriptor::Jaudio(const JaudioSettings& settings)
{
    ArchiveDescriptor d;
    d.version = kJaudioVersion;
    d.voice_count = kJaudioVoices;
    d.jaudio = settings;

    return d;
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

    // Version 2 keeps version 1's layout up to here. A version 1 player then reads the version, and refuses it.
    if (version == kJaudioVersion)
    {
        PutJaudio(v, jaudio);
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

    return d;
}

std::optional<std::string> ArchiveDescriptor::Check() const
{
    if (version == kJaudioVersion)
    {
        return CheckJaudio(*this);
    }

    if (version != kVersion)
    {
        return "archive descriptor version " + std::to_string(version) + "; this player supports versions 1 and 2";
    }

    if (auto err = xsflib::psf::CheckArchiveStart(*this, kDefaultVoices))
    {
        return err;
    }

    for (const AuxEffect& effect : effects)
    {
        if (effect.bus > 2)
        {
            return "an aux effect is on bus " + std::to_string(effect.bus) + "; the aux buses are 0 to 2";
        }

        if (effect.type != AuxEffect::kI3dl2Reverb)
        {
            return "an aux effect's type is " + std::to_string(effect.type) +
                   "; version 1 supports only the I3DL2 reverb (1)";
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
