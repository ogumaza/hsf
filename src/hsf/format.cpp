// SPDX-License-Identifier: MIT

// The HSF file format (see format.h and docs/hsf.md).

#include "hsf/format.h"

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

namespace hsf
{
namespace
{

using xsflib::psf::Put32;

} // namespace

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

    return d;
}

std::optional<std::string> ArchiveDescriptor::Check() const
{
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
