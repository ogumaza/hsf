// SPDX-License-Identifier: MIT

// nn::atk's MultiVoices on the audio renderer: the renderer that ntdlib's model of the MultiVoices plays on (see
// ntdlib/nwsnd/voice.h). Each wave channel's voice plays on a LowLevelVoice. The renderer holds the wave buffers that
// each MultiVoice queues, and works out each voice's mix, low-pass and biquad filter. Each function's comment gives the
// address of the routine it follows in WarioWare: Get It Together!'s main.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "atk/hardware.h"
#include "atk/low_level_voice.h"
#include "atk/sequence.h"
#include "atk/voice.h"

namespace hsf::atk
{

class VoiceRenderer
{
public:
    using Voice = MultiVoice::Voice;

    // `voice_count` is the sound system's voice count: the MultiVoices whose buffers the renderer holds.
    VoiceRenderer(LowLevelVoiceAllocator& allocator, const HardwareSettings& hardware, int voice_count);

    VoiceRenderer(const VoiceRenderer&) = delete;
    VoiceRenderer& operator=(const VoiceRenderer&) = delete;

    // A LowLevelVoice for a wave channel's voice, at `priority` (Voice::AllocVoice, 0xc27bc0): its place among the
    // sound system's voices, or -1 when every one is allocated.
    int Acquire(uint32_t priority, Voice& voice);

    void Free(int renderer);              // Voice::Free, 0xc27c70
    bool IsAvailable(int renderer) const; // Voice::IsAvailable, 0xc27bb0
    void SetPriority(int renderer, uint32_t priority);

    // Stopping first drops the queued buffers. Playing gives the LowLevelVoice the wave's format, rate and
    // coefficients, and the receiver (Voice::SetState, 0xc27cd0).
    void SetState(int renderer, const Voice& voice);

    // Voice::UpdateParam (0xc27d70).
    void SetParam(int renderer, const VoiceParam& param);

    // Queues each wave channel's buffers on its voice: the wave from its start to its end, and for a wave that loops,
    // the loop after that, for ever (Channel::AppendWaveBuffer, 0xc237e0). Each buffer has all the channel's samples.
    // The game's Channel holds the buffers (+0x1a0) and the decoder's states that they start from (+0x2c0 and +0x340).
    // Here the renderer holds them, for the MultiVoice at `place`.
    void AppendWave(VoiceId place, std::span<Voice> voices, const WaveInfo& wave);

    // Whether the last buffer of the MultiVoice at `place` is done (part of MultiVoice::UpdateVoiceStatus, 0xc24820).
    bool IsPlayFinished(VoiceId place) const;

    // The product of the two volume ramps of GetOutputVolume (0xc06230).
    float OutputVolume() const
    {
        return hardware_->output_volume;
    }

    // Each voice's gains to the buses of the receiver, by its pan and surround pan or its mix parameters.
    void CalcMix(MultiVoice& voice) const;

    // Voice::SetMonoFilter (0xc27e00) for each voice: turning the filter off leaves its cutoff.
    void CalcLpf(MultiVoice& voice) const;

    // MultiVoice::UpdateBiquadFilter (0xc24c40).
    void CalcBiquadFilter(MultiVoice& voice) const;

private:
    // A MultiVoice's buffers: each wave channel's start and loop, the decoder's states that they start from, and the
    // buffer whose end ends the voice (MultiVoice +0x1e0).
    struct Buffers
    {
        std::array<std::array<WaveBuffer, 2>, 2> wave_buffers;
        std::array<AdpcmContext, 2> contexts;
        std::array<AdpcmContext, 2> loop_contexts;
        WaveBuffer* last = nullptr;
    };

    // A channel's gains: front left and right, rear left and right, front centre and LFE.
    using Gains = std::array<float, 6>;

    // Queues a buffer on a channel's voice, if the voice still holds a LowLevelVoice (MultiVoice::AppendWaveBuffer,
    // 0xc26440). The last buffer queued with `last` is the one whose end ends the voice.
    void AppendWaveBuffer(Buffers& buffers, const Voice& voice, WaveBuffer& buffer, bool last);

    // A channel's gains by its pan and surround pan, or its mix parameters (MultiVoice::CalcMixVolume, 0xc259f0).
    Gains CalcMixVolume(const MultiVoice& voice, int channel) const;

    // The gains to each bus of the receiver: the output's volume, the bus's send and the gains
    // (MultiVoice::CalcMixVolumeForBuses, 0xc25fc0). The sends are 0 when the voice's output line doesn't reach the
    // console's output.
    OutputMix CalcMixVolumeForBuses(const MultiVoice& voice, const Gains& gains) const;

    LowLevelVoiceAllocator* allocator_;
    const HardwareSettings* hardware_;
    std::vector<Buffers> buffers_;
};

} // namespace hsf::atk
