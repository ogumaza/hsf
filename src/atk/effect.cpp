// SPDX-License-Identifier: MIT

#include "atk/effect.h"

#include <cstddef>
#include <cstdint>
#include <span>

#include "atk/util.h"
#include "audio/i3dl2.h"

namespace hsf::atk
{

I3dl2AuxEffect::I3dl2AuxEffect(const audio::I3dl2ReverbParameterSet& parameters) : reverb_(48000)
{
    // The constructor takes the reverb's first two blocks of a frame for input and output (0x8ac7f8). The game then
    // sets every parameter, the ratio held with fmin then fmax.
    reverb_.SetInputOutput({0, 1}, {0, 1});
    audio::I3dl2ReverbParameterSet held = parameters;
    held.late_reverb_hf_decay_ratio = util::Fmax(util::Fmin(parameters.late_reverb_hf_decay_ratio, 2.0f), 0.1f);
    reverb_.SetParameters(held);
}

void I3dl2AuxEffect::UpdateBuffer(std::span<int32_t> samples, const BufferInfo& info)
{
    // Whole frames of every channel, no more than the ring holds; the game stops otherwise.
    const int32_t frame = info.sample_count_per_frame * info.channel_count;
    if (frame <= 0 || info.read_sample_count % frame != 0 || info.read_sample_count / frame > info.frame_count)
    {
        return;
    }

    const auto count = static_cast<uint32_t>(info.sample_count_per_frame);
    for (int32_t f = 0; f < info.read_sample_count / frame; ++f)
    {
        int32_t* block = samples.data() + static_cast<std::ptrdiff_t>(f) * frame;
        reverb_.Process(block, block, count);
    }
}

} // namespace hsf::atk
