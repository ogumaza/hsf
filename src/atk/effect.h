// SPDX-License-Identifier: MIT

// nn::atk's aux effects (EffectAux), which run in the game's process on what the renderer sends them from a bus, and
// the I3DL2 reverb that the reference game runs on its aux bus A that way. Each function's comment gives the address of
// the routine it follows in WarioWare: Get It Together!'s main.

#pragma once

#include <cstdint>
#include <span>

#include "atk/util.h"
#include "audio/i3dl2.h"

namespace hsf::atk
{

// An effect on an aux bus (nn::atk::EffectAux, whose constructor is 0xc2b0d0). Each frame, the sound thread reads what
// the renderer has sent from the bus since the last frame, gives it to UpdateBuffer and writes it back. What the sound
// thread writes back comes out of the bus when the renderer comes round the ring again, 4 frames after it went in
// (0xc2b580).
class EffectAux
{
public:
    // The ring holds 4 frames of the bus's first 4 channels (+0x28 and +0x2c). A game can change both counts.
    static constexpr int kFrameCount = 4;
    static constexpr int kChannelCount = 4;

    // What UpdateBuffer takes (EffectAux::BufferUpdateInfo).
    struct BufferInfo
    {
        int32_t sample_count_per_frame = 0; // a channel's samples in a frame
        int32_t sample_rate = 0;
        int32_t frame_count = 0; // the most frames the samples hold
        int32_t channel_count = 0;
        int32_t read_sample_count = 0; // whole frames of every channel, channel after channel within each
        OutputMode output_mode = OutputMode::kStereo;
    };

    virtual ~EffectAux() = default;

    // Processes the samples that were read, in place (EffectAux's pure virtual function).
    virtual void UpdateBuffer(std::span<int32_t> samples, const BufferInfo& info) = 0;
};

// The I3DL2 reverb that the reference game runs on an aux bus (its class at 0x8ac6b0, whose UpdateBuffer is 0x8ac8d0):
// Nintendo's reverb (audio::AuxI3dl2Reverb) at 48 kHz on the bus's front left and right, a frame at a time, with the
// parameters a scene gives it. The rear channels come back as they went in.
class I3dl2AuxEffect : public EffectAux
{
public:
    // The game's setters (0x8ac990 on) store each parameter as it is, but hold the high frequencies' decay ratio to 0.1
    // to 2 (0x8aca70).
    explicit I3dl2AuxEffect(const audio::I3dl2ReverbParameterSet& parameters);

    void UpdateBuffer(std::span<int32_t> samples, const BufferInfo& info) override;

private:
    audio::AuxI3dl2Reverb reverb_;
};

} // namespace hsf::atk
