// SPDX-License-Identifier: MIT

#include "rip/sound_states.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>

namespace hsf::rip
{

ntdlib::rip::TrackVolumes TrackVolumesOf(uint16_t muted)
{
    ntdlib::rip::TrackVolumes volumes = ntdlib::rip::kFullTrackVolumes;
    for (std::size_t t = 0; t < volumes.size(); ++t)
    {
        if ((muted >> t & 1) != 0)
        {
            volumes[t] = 0.0f;
        }
    }

    return volumes;
}

std::vector<ntdlib::rip::Job> WithStates(const ntdlib::sar::ArchiveRules& rules,
                                         const std::vector<ntdlib::rip::Job>& selected)
{
    if (rules.Empty())
    {
        return selected;
    }

    std::vector<ntdlib::rip::Job> jobs;
    for (const ntdlib::rip::Job& job : selected)
    {
        for (const ntdlib::sar::SoundState& state : ntdlib::sar::StatesOf(rules, job.label))
        {
            ntdlib::rip::Job in_state = job;
            if (!state.name.empty())
            {
                in_state.label += " ";
                in_state.label += state.name;
            }

            in_state.track_volumes = TrackVolumesOf(state.muted);
            in_state.bus_b_reverb = rules.FindBusBPreset(state.bus_b);
            jobs.push_back(std::move(in_state));
        }
    }

    return jobs;
}

ArchiveDescriptor WithBusBReverb(const ArchiveDescriptor& settings, const ntdlib::sar::ReverbPreset* reverb)
{
    if (!reverb)
    {
        return settings;
    }

    ArchiveDescriptor with = settings;
    std::erase_if(with.effects, [](const AuxEffect& e) { return e.bus == 1; });
    AuxEffect effect;
    effect.bus = 1;
    effect.type = reverb->type == ntdlib::sar::ReverbType::kReverb ? AuxEffect::kReverb : AuxEffect::kI3dl2Reverb;
    effect.parameters.assign(reverb->parameters.begin(), reverb->parameters.end());
    with.effects.push_back(std::move(effect));
    std::ranges::stable_sort(with.effects, {}, &AuxEffect::bus);

    return with;
}

} // namespace hsf::rip
