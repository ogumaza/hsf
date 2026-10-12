// SPDX-License-Identifier: MIT

// nn::atk's voices on the audio renderer (LowLevelVoice and LowLevelVoiceAllocator). They stand where nw::snd's
// AxVoices stand on the Wii U. A LowLevelVoice plays a queue of wave buffers on a voice of the renderer. The
// LowLevelVoice acquires the renderer's voice when it starts to play and releases that voice as soon as it stops. Each
// frame, the LowLevelVoice gives the renderer the parameters that the layers above work out, starts, pauses and stops
// the renderer's voice, and keeps a buffer queued ahead of the one that plays. Each function's comment gives the
// address of the routine it follows in WarioWare: Get It Together!'s main.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "atk/hardware.h"
#include "atk/util.h"
#include "audio/dsp.h"
#include "audio/renderer.h"

namespace hsf::atk
{

// nn::atk's sample formats.
enum class SampleFormat : uint32_t
{
    kPcm8 = 0,
    kPcm16 = 1,
    kDspAdpcm = 2,
};

// The DSP-ADPCM decoder's state at a buffer's start, and the coefficients.
using AdpcmContext = audio::AdpcmContext;
using AdpcmCoefficients = audio::AdpcmCoefficients;

// A block of samples a voice plays (WaveBuffer): the samples of a wave's channel from `sample_offset` up to
// `sample_length`. A voice plays its queue in order, and a buffer that loops plays until the voice stops. The voice
// marks each buffer done once the renderer has finished with it.
struct WaveBuffer
{
    enum class Status : uint32_t
    {
        kFree = 0,
        kWait = 1,
        kPlay = 2,
        kDone = 3,
    };

    std::span<const uint8_t> data;               // +0x0 and +0x8: all the channel's samples
    int64_t sample_length = 0;                   // +0x10: where the buffer ends
    int64_t sample_offset = 0;                   // +0x18: where it starts
    const AdpcmContext* adpcm_context = nullptr; // +0x20
    bool loop = false;                           // +0x30
    Status status = Status::kFree;               // +0x34
    WaveBuffer* next = nullptr;                  // +0x38
};

// A voice's gains to the buffers of the mix it outputs to. For a mix of c channels, gain i is to channel i % c of bus i
// / c. A mix has 24 buffers at most.
using OutputMix = std::array<float, 24>;

// What a voice plays with this frame (the parameters at a Voice's +0x8 and at a LowLevelVoice's +0x2c). The renderer
// never reads the interpolation type.
struct VoiceParam
{
    float volume = 1.0f;
    float pitch = 1.0f;
    OutputMix mix{};
    bool lpf_on = false;
    bool biquad_on = false;
    std::array<int16_t, 5> biquad{}; // b0, b1, b2, a1 and a2 in Q14
    uint16_t lpf_freq = 0;           // the cutoff in Hz
    uint8_t interpolation = 0;       // +0x76
};

class LowLevelVoiceAllocator;

class LowLevelVoice
{
public:
    enum class State : uint32_t
    {
        kPlay = 0,
        kStop = 1,
        kPause = 2,
    };

    // Queues a buffer at the queue's end, waiting (AppendWaveBuffer, 0xc26e70).
    void AppendWaveBuffer(WaveBuffer& buffer);

    // Marks every queued buffer done and empties the queue (FreeAllWaveBuffer, 0xc26e30).
    void FreeAllWaveBuffer();

    // nn::atk's priority (SetPriority, 0xc26ea0). The renderer takes 255 minus that priority.
    void SetPriority(uint32_t priority);

    // The state the voice should be in (SetState, 0xc27040). Playing acquires a voice of the renderer if the voice has
    // none, and stopping releases it at once.
    void SetState(State state);

    // What the voice plays when it next acquires a voice of the renderer: the wave's format, rate and DSP-ADPCM
    // coefficients, and the mix its gains go to (Voice::SetState gives them to it, 0xc27cd0).
    void SetWave(SampleFormat format, uint32_t sample_rate, const AdpcmCoefficients& coefficients,
                 const OutputReceiver* receiver);

    // The parameters the renderer gets each frame.
    VoiceParam& Param()
    {
        return param_;
    }

    // Whether the allocator still holds the voice for its owner (IsAvailable, 0xc26e60). The renderer may drop it.
    bool IsAvailable() const
    {
        return allocated_;
    }

    // The sample of its wave that the voice played, by the renderer's last report (+0xb8).
    int64_t PlayPosition() const
    {
        return play_position_;
    }

    // The renderer's voice, or -1 if the voice has none.
    int Slot() const
    {
        return slot_;
    }

    // Whether the voice is wanted playing and has a voice of the renderer.
    bool IsPlaying() const
    {
        return slot_ >= 0 && state_ == State::kPlay;
    }

    // Whether a buffer the voice has queued loops: its wave loops.
    bool Loops() const;

private:
    friend class LowLevelVoiceAllocator;

    bool IsVoiceValid() const
    {
        return slot_ >= 0;
    }

    bool Acquire();                        // 0xc26f00
    void UpdateParam(OutputMode mode);     // 0xc270b0
    void UpdateLowPass();                  // 0xc27300
    void UpdateMixVolume(OutputMode mode); // 0xc273c0
    void UpdateStatePlay(OutputMode mode); // 0xc271c0
    void UpdateWaveBuffer();               // 0xc275f0

    // Gives a buffer to the renderer. The buffer ends a stream.
    bool AppendToRenderer(WaveBuffer& buffer);

    LowLevelVoiceAllocator* owner_ = nullptr;
    AdpcmCoefficients coefficients_{};           // +0x0
    int slot_ = -1;                              // +0x20: the renderer's voice
    bool allocated_ = false;                     // +0x28
    bool acquired_ = false;                      // +0x29
    VoiceParam param_;                           // +0x2c
    uint32_t priority_ = 0;                      // +0xa4
    State state_ = State::kStop;                 // +0xa8
    uint32_t sample_rate_ = 32000;               // +0xac
    SampleFormat format_ = SampleFormat::kPcm16; // +0xb0
    int64_t play_position_ = 0;                  // +0xb8
    const OutputReceiver* receiver_ = nullptr;   // +0xc0
    WaveBuffer* head_ = nullptr;                 // +0xc8
    WaveBuffer* tail_ = nullptr;                 // +0xd0
    WaveBuffer* last_ = nullptr;                 // +0xd8: the last buffer given to the renderer
};

// The LowLevelVoices, one for each of the sound system's voices, at the hardware manager's +0x530. The allocator keeps
// them in a list with the allocated voices first. A freed voice swaps places with the last allocated one.
class LowLevelVoiceAllocator
{
public:
    LowLevelVoiceAllocator(audio::Renderer& renderer, const HardwareSettings& hardware, int count);

    LowLevelVoiceAllocator(const LowLevelVoiceAllocator&) = delete;
    LowLevelVoiceAllocator& operator=(const LowLevelVoiceAllocator&) = delete;

    // A voice, stopped, for PCM16 at 32 kHz, or nullptr when every voice is allocated (AllocVoice, 0xc27a50).
    LowLevelVoice* AllocVoice();

    // Empties the voice's queue, releases its voice of the renderer, and gives it back (FreeVoice, 0xc27ac0).
    void FreeVoice(LowLevelVoice& voice);

    // Before each sound frame: voices the renderer dropped, the buffers it has finished with, and the play positions
    // (UpdateAllVoiceStatus, 0xc27830).
    void UpdateAllVoiceStatus();

    // After each sound frame: each voice's parameters, then its state, to the renderer (UpdateAllVoice, 0xc27950).
    void UpdateAllVoice(OutputMode mode);

    int AllocatedCount() const
    {
        return count_;
    }

    // Every voice, by its place among the sound system's voices. A voice's place doesn't change.
    std::span<const LowLevelVoice> All() const
    {
        return voices_;
    }

    // The voice at a place among the sound system's voices, and a voice's place there.
    LowLevelVoice& At(int place)
    {
        return voices_[static_cast<std::size_t>(place)];
    }

    const LowLevelVoice& At(int place) const
    {
        return voices_[static_cast<std::size_t>(place)];
    }

    int PlaceOf(const LowLevelVoice& voice) const
    {
        return static_cast<int>(&voice - voices_.data());
    }

private:
    friend class LowLevelVoice;

    audio::Renderer* renderer_;
    const HardwareSettings* hardware_;
    std::vector<LowLevelVoice> voices_;
    std::vector<LowLevelVoice*> list_; // the allocated voices first
    std::vector<int> places_;          // each voice's place in the list while it's allocated, or -1
    int count_ = 0;                    // +0x10
};

} // namespace hsf::atk
