// SPDX-License-Identifier: MIT

// What an archive descriptor sets up on ntdlib's model of nn::atk: the engine's options, and the effects on the aux
// buses. The player and the ripper's length analysis both set the model up with these functions. A rip's length then
// comes from the sound that the player plays.

#pragma once

#include <memory>
#include <vector>

#include "ntdlib/nx/atk/effect.h"
#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/nx/audio/i3dl2.h"

#include "hsf/format.h"
#include "hsf/nx.h"

namespace hsf
{

// The engine's options for a set with `descriptor`: nn::atk's voice count.
inline atk::EngineOptions EngineOptionsFor(const ArchiveDescriptor& descriptor)
{
    atk::EngineOptions options;
    options.voice_count = static_cast<int>(descriptor.voice_count);

    return options;
}

// An I3DL2 reverb's parameters as an AuxEffect holds them: the 12 fields of nn::audio's I3dl2ReverbParameterSet, in
// their order.
inline std::vector<float> I3dl2Values(const audio::I3dl2ReverbParameterSet& p)
{
    return {p.room_gain,
            p.room_hf_gain,
            p.late_reverb_decay_time,
            p.late_reverb_hf_decay_ratio,
            p.reflections_gain,
            p.reflections_delay,
            p.reverb_gain,
            p.late_reverb_delay,
            p.late_reverb_diffusion,
            p.late_reverb_density,
            p.hf_reference,
            p.dry_gain};
}

// The parameter set that an AuxEffect's 12 values give (I3dl2Values).
inline audio::I3dl2ReverbParameterSet I3dl2Parameters(const std::vector<float>& v)
{
    return {v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]};
}

// Puts `effects` on the aux buses of `engine` in their order, before the sound starts. Each effect is an I3DL2 reverb
// with 12 parameters, as ArchiveDescriptor::Check requires.
inline void AppendEffects(atk::Engine& engine, const std::vector<AuxEffect>& effects)
{
    for (const AuxEffect& effect : effects)
    {
        engine.AppendEffect(static_cast<int>(effect.bus),
                            std::make_unique<atk::I3dl2AuxEffect>(I3dl2Parameters(effect.parameters)));
    }
}

} // namespace hsf
