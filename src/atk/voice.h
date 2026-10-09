// SPDX-License-Identifier: MIT

// nn::atk's MultiVoice and its manager (driver::MultiVoice and driver::MultiVoiceManager). nw::snd has the same
// MultiVoice and manager. A MultiVoice plays each channel of a wave on a voice of its own: a Voice record and the
// LowLevelVoice it holds. A MultiVoice keeps the parameters that a Channel sets each frame, and works out each voice's
// volume, pitch, mix and filters when they change. Each function's comment gives the address of the routine it follows
// in WarioWare: Get It Together!'s main. The optional packet of output gains that a game can give a voice (+0x260) and
// the side of a stream's channels (+0x1c0) aren't modelled: a sequence's voices have neither.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <vector>

#include "atk/hardware.h"
#include "atk/low_level_voice.h"
#include "atk/util.h"

namespace hsf::atk
{

// A sound's output parameters (OutputParam, 0x50 bytes): its volume, whether the channels mix by pan (0) or by the mix
// parameters (1), each wave channel's mix parameters (front left and right, rear left and right, front centre and LFE),
// the pan, the surround pan, and the main and effect sends. The main send is an offset from 1.
struct OutputParam
{
    float volume = 1.0f;
    uint32_t mix_mode = 0;
    std::array<std::array<float, 6>, 2> mix_parameter{
        {{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}}};
    float pan = 0.0f;
    float span = 0.0f;
    float main_send = 0.0f;
    std::array<float, 3> fx_send{};
};

class MultiVoiceManager;

class MultiVoice
{
public:
    // Why a MultiVoice calls its owner back (VoiceCallbackStatus): the wave has played to its end (0), the MultiVoice
    // was cancelled (1), the manager dropped the MultiVoice for a voice of higher priority (2), or the renderer dropped
    // one of the MultiVoice's voices (3).
    enum class CallbackStatus : uint32_t
    {
        kFinishWave = 0,
        kCancel = 1,
        kDropVoice = 2,
        kDropDsp = 3,
    };

    using Callback = std::function<void(MultiVoice& voice, CallbackStatus status)>;

    // Start sets the voices to start at the next update (0xc24790). Stop stops them now (0xc247b0).
    void Start();
    void Stop();

    // Gives the voices back to the allocator and the MultiVoice back to its manager (0xc24720).
    void Free();

    void SetSampleFormat(SampleFormat format);                         // 0xc24fe0
    void SetSampleRate(uint32_t rate);                                 // 0xc25060
    void SetAdpcmParam(int channel, const AdpcmCoefficients& coefs);   // 0xc26490
    void SetOutputReceiver(const OutputReceiver* receiver);            // 0xc25970
    void AppendWaveBuffer(int channel, WaveBuffer& buffer, bool last); // 0xc26440: the last buffer ends the wave

    // Each channel's interpolation type (SetInterpolationType, 0xc263b0). The voices keep the type, and the renderer
    // never gets it.
    void SetInterpolationType(uint8_t type);

    void SetVolume(float volume);                  // 0xc25090: held to 0 and up
    void SetPitch(float pitch);                    // 0xc250c0
    void SetPanMode(uint32_t mode);                // 0xc250e0
    void SetPanCurve(uint32_t curve);              // 0xc25100
    void SetLpfFreq(float lpf);                    // 0xc25120
    void SetBiquadFilter(int type, float value);   // 0xc25140: the value held to 0 to 1
    void SetPriority(uint32_t priority);           // 0xc251a0
    void SetOutputLine(uint32_t line);             // 0xc25210
    void SetOutputParam(const OutputParam& param); // 0xc258b0

    int ChannelCount() const
    {
        return channel_count_;
    }

    float Volume() const
    {
        return volume_;
    }

    float Pitch() const
    {
        return pitch_;
    }

    // The LowLevelVoice that plays a channel, or nullptr.
    const LowLevelVoice* Voice(int channel) const
    {
        return voices_[static_cast<std::size_t>(channel)].low;
    }

private:
    friend class MultiVoiceManager;

    // A wave channel's voice (Voice, 0xe0 bytes): what a Voice gives the LowLevelVoice it holds.
    struct ChannelVoice
    {
        uint32_t priority = 0;                                    // +0x0
        LowLevelVoice::State state = LowLevelVoice::State::kStop; // +0x4
        VoiceParam param;                                         // +0x8
        uint8_t interpolation = 0;                                // +0x7e
        SampleFormat format = SampleFormat::kPcm16;               // +0x80
        uint32_t sample_rate = 32000;                             // +0x84
        AdpcmCoefficients coefs{};                                // +0x88
        const OutputReceiver* receiver = nullptr;                 // +0xa8
        LowLevelVoice* low = nullptr;                             // +0xd8
    };

    // The changes that wait for the next update (+0x1e8): the start and the pause, and the calculations that a
    // parameter change requires.
    static constexpr uint16_t kSyncStart = 0x01;
    static constexpr uint16_t kSyncPause = 0x02;
    static constexpr uint16_t kSyncSrc = 0x04;
    static constexpr uint16_t kSyncMix = 0x08;
    static constexpr uint16_t kSyncLpf = 0x10;
    static constexpr uint16_t kSyncBiquad = 0x20;
    static constexpr uint16_t kSyncVe = 0x40;

    // A channel's gains: front left and right, rear left and right, front centre and LFE.
    using Gains = std::array<float, 6>;

    bool Alloc(int channels, int32_t priority, Callback callback); // 0xc245e0

    // Each frame: first the calculations that the changes require (Calc, 0xc24960). Then the start, the pause, and the
    // parameters given to the voices (Update, 0xc24cf0).
    void Calc();
    void Update();

    // Each frame before the sound frame's other work: the end of the wave, or the renderer dropping a voice (0xc24820).
    void UpdateVoiceStatus();

    void CalcSrc();
    void CalcVe();
    void CalcMix();
    void CalcLpf();
    void UpdateBiquadFilter(); // 0xc24c40

    // A channel's gains by its pan and surround pan, or its mix parameters (CalcMixVolume, 0xc259f0).
    Gains CalcMixVolume(int channel) const;

    // The gains to each bus of the receiver: the output's volume, the bus's send and the gains (CalcMixVolumeForBuses,
    // 0xc25fc0). The sends are 0 when the voice's output line doesn't reach the console's output.
    OutputMix CalcMixVolumeForBuses(const Gains& gains) const;

    // The Voice's functions.
    bool AllocVoice(ChannelVoice& voice, int32_t priority);              // 0xc27bc0
    void FreeVoice(ChannelVoice& voice);                                 // 0xc27c70
    void SetVoiceState(ChannelVoice& voice, LowLevelVoice::State state); // 0xc27cd0
    static bool IsAvailable(const ChannelVoice& voice);                  // 0xc27bb0

    MultiVoiceManager* manager_ = nullptr;
    std::array<ChannelVoice, 2> voices_;
    int channel_count_ = 0;                      // +0x1c4
    Callback callback_;                          // +0x1c8
    bool active_ = false;                        // +0x1d8
    bool started_ = false;                       // +0x1d9
    bool sdk_started_ = false;                   // +0x1da: the voices have been started on the renderer
    bool pause_ = false;                         // +0x1db
    bool paused_ = false;                        // +0x1dc
    WaveBuffer* last_buffer_ = nullptr;          // +0x1e0
    uint16_t sync_ = 0;                          // +0x1e8
    uint8_t biquad_type_ = 0;                    // +0x1ea
    float volume_ = 1.0f;                        // +0x1ec
    float pitch_ = 1.0f;                         // +0x1f0
    uint32_t pan_mode_ = 0;                      // +0x1f4
    uint32_t pan_curve_ = 0;                     // +0x1f8
    float lpf_freq_ = 1.0f;                      // +0x1fc
    float biquad_value_ = 0.0f;                  // +0x200
    uint32_t priority_ = 0;                      // +0x204
    uint32_t output_line_ = 1;                   // +0x208
    OutputParam output_;                         // +0x20c
    SampleFormat format_ = SampleFormat::kPcm16; // +0x268
    const OutputReceiver* receiver_ = nullptr;   // +0x280
};

// The MultiVoices, a fixed number of them: those in use in a list from the lowest priority up, the newest last among
// equals, and the others in a queue.
class MultiVoiceManager
{
public:
    // `count` is the sound system's voice count.
    MultiVoiceManager(LowLevelVoiceAllocator& allocator, const HardwareSettings& hardware, int count);

    MultiVoiceManager(const MultiVoiceManager&) = delete;
    MultiVoiceManager& operator=(const MultiVoiceManager&) = delete;

    // A MultiVoice for `channels` channels, or nullptr (AllocVoice, 0xc268d0). When no MultiVoice or no LowLevelVoice
    // is left, the voices of the lowest priority are dropped until their channels make room, if they all have a
    // priority of at most `priority` and below 255.
    MultiVoice* AllocVoice(int channels, int32_t priority, MultiVoice::Callback callback);

    void UpdateAllVoiceStatus(); // 0xc26bf0
    void UpdateAllVoices();      // 0xc26cc0

    int ActiveCount() const
    {
        return static_cast<int>(active_.size());
    }

private:
    friend class MultiVoice;

    // Drops the voices of the lowest priority, the oldest first among equals, until they had `channels` channels.
    // Nothing is dropped unless every one of them has a priority of at most `priority` and below 255. Returns whether
    // any were dropped (part of AllocVoice).
    bool DropLowestPriorityVoices(int channels, int32_t priority);

    void AppendVoiceList(MultiVoice& voice);     // part of AllocVoice
    void RemoveVoiceList(MultiVoice& voice);     // 0xc26b50
    void ChangeVoicePriority(MultiVoice& voice); // 0xc26d60

    LowLevelVoiceAllocator* allocator_;
    const HardwareSettings* hardware_;
    std::vector<std::unique_ptr<MultiVoice>> voices_;
    std::list<MultiVoice*> active_;
    std::list<MultiVoice*> free_;
};

} // namespace hsf::atk
