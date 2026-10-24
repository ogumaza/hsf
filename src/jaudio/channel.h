// SPDX-License-Identifier: MIT

// JAudio2's channels as the Super Mario Galaxy games' `main` builds them for AArch64: a note's channel (JASChannel),
// the 64 DSP channels that the notes play on (JASDSPChannel), and the CPU side's writes to a DSP channel's block
// (JASDsp's TChannel). Each sub-frame, the CPU side updates every DSP channel. A note's channel then writes its
// mixers' volumes, its pitch and its pause into the block, and the DSP renders the block.
//
// The channels and the DSP channels refer to one another by index. The state holds no pointer to the emulated memory.
// A channel's oscillators point to their settings: an instrument's in its bank, or a track's (track.h).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "jaudio/dsp.h"
#include "jaudio/oscillator.h"

namespace hsf::jaudio
{

// A wave as a channel plays it (JASWaveInfo).
struct WaveInfo
{
    uint8_t format = 0;    // 0 AFC of 9 bytes a block, 1 AFC of 5, 2 PCM8, 3 PCM16
    uint8_t base_key = 60; // the key that plays the wave at its rate
    float sample_rate = 0.0f;
    uint32_t loop_flags = 0; // nonzero for a wave that loops
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
    uint32_t sample_count = 0;
    int16_t last = 0; // the decoder's history at the loop's start
    int16_t penult = 0;
};

// A view of a channel's block in the emulated memory, for the CPU side's writes (JASDsp::TChannel). The CPU side
// keeps the blocks in the host's byte order, little-endian.
class DspBlock
{
public:
    DspBlock(DspMemory& memory, uint32_t address) : memory_(memory), address_(address)
    {
    }

    uint32_t Address() const
    {
        return address_;
    }

    uint16_t U16(std::size_t offset) const
    {
        return memory_.Native16(address_ + static_cast<uint32_t>(offset));
    }

    void SetU16(std::size_t offset, uint32_t value)
    {
        memory_.SetNative16(address_ + static_cast<uint32_t>(offset), static_cast<uint16_t>(value));
    }

    uint32_t U32(std::size_t offset) const
    {
        return memory_.Native32(address_ + static_cast<uint32_t>(offset));
    }

    void SetU32(std::size_t offset, uint32_t value)
    {
        memory_.SetNative32(address_ + static_cast<uint32_t>(offset), value);
    }

    bool IsFinish() const
    {
        return U16(DspChannelBlock::kFinished) != 0;
    }

    // The block's set-up before its channel starts (init), the start of the playback (playStart), its stop by the
    // CPU side (playStop), the answer to the DSP's end of the wave (replyFinishRequest) and the forced stop of a
    // dropped channel (forceStop).
    void Init();
    void PlayStart();
    void PlayStop();
    void ReplyFinishRequest();
    void ForceStop();

    // The wave, from its address, with `skip` samples skipped at its start (1 skips to the loop's start). Format 2
    // and 3 skip by the position, and AFC skips whole blocks from the wave's address.
    void SetWaveInfo(const WaveInfo& wave, uint32_t wave_address, uint32_t skip);

    // The oscillator's waveform for a channel of the oscillator type.
    void SetOscInfo(uint32_t program);

    // The auto mixer of Dolby mode. The DSP model doesn't render it.
    void InitAutoMixer();
    void SetAutoMixer(int32_t volume, int32_t pan, int32_t dolby, int32_t fxmix);

    // The low 16 bits of `pitch`, at most 0x7fff: the step through the wave per output sample, in 4.12.
    void SetPitch(int32_t pitch);

    // A mixer's volume at the channel's start, and its target volume after that. A forced stop keeps the volume.
    void SetMixerInitVolume(uint32_t mixer, uint16_t volume);
    void SetMixerVolume(uint32_t mixer, uint16_t volume);

    // A mixer's bus by its number (0 for none, 1 and 2 for the left and right outputs, 3 to 6 for the FX lines).
    void SetBusConnect(uint32_t mixer, uint32_t bus);

    void SetPauseFlag(uint8_t flag);

    // The filters' settings (setIIRFilterParam, setFIR8FilterParam, setFilterMode and setDistFilter). The IIR
    // filter's setter writes 8 values: the 4 coefficients, then 4 more over the distortion's amount and the 6 bytes
    // after it. The filter mode holds the FIR filter's taps in its low 5 bits and the IIR filter's switch in bit 5.
    void SetIirFilterParam(const std::array<uint16_t, 8>& values);
    void SetFir8FilterParam(const std::array<uint16_t, 8>& values);
    void SetFilterMode(uint32_t mode);
    void SetDistFilter(int32_t amount);

private:
    DspMemory& memory_;
    uint32_t address_;
};

// The levels that a channel's track gives it each sub-frame (JASChannelParams). `octaves` moves the pitch.
struct ChannelParams
{
    float volume = 1.0f;
    float pitch = 1.0f;
    float octaves = 0.0f;
    float pan = 0.5f;
    float fxmix = 0.0f;
    float dolby = 0.0f;
};

// The levels that a channel's instrument gives it when it starts (JASSoundParams), in the game's order of fields.
struct SoundParams
{
    float volume = 1.0f;
    float fxmix = 0.0f;
    float pitch = 1.0f;
    float pan = 0.5f;
    float dolby = 0.0f;
};

// What a channel's oscillators do to its levels each sub-frame (JASOscillator::EffectParams).
struct EffectParams
{
    float volume = 1.0f;
    float pitch = 1.0f;
    float pan = 0.5f;
    float fxmix = 0.0f;
    float dolby = 0.0f;
    float vibrato = 1.0f; // the vibrato's depth
    float tremolo = 1.0f; // the tremolo's depth
};

// A note's channel (JASChannel).
struct Channel
{
    static constexpr uint32_t kNone = 0xffffffff;

    enum Status : uint32_t
    {
        kStop = 0,
        kPlay = 1,
        kRelease = 2,
    };

    enum Type : uint32_t
    {
        kWave = 0,
        kOsc = 2,
    };

    // The events that a channel's callback reports to its owner (JASChannel's callback types).
    enum Event : uint32_t
    {
        kEventPlay = 0,
        kEventStart = 1,
        kEventStop = 2,
        kEventTimer = 3,
    };

    // A channel is in use from its construction to its deletion.
    bool alive = false;

    uint32_t status = kStop;
    bool pause = false;
    uint32_t dsp_channel = kNone; // the DSP channel that the channel plays on
    uint32_t owner = kNone;       // what the callback reports to, or kNone with no callback
    uint32_t update_timer = 0;    // sub-frames to the timer's event
    std::array<Oscillator, 2> oscillators;
    Lfo vibrato;
    Lfo tremolo;

    // Each mixer's bus in the high byte, and in each of the low two nibbles a level that scales it: 1 the pan, 2
    // the effect send, 3 the surround level, 5 to 7 the same subtracted from 1. 0xffff in the first switches to the
    // auto mixer of Dolby mode.
    std::array<uint16_t, 6> mix_configs = {0x150, 0x210, 0x352, 0x412, 0, 0};
    uint16_t priority = 0x13f; // the low byte while the channel plays, the high byte from its release
    ChannelParams params;
    SoundParams sound_params;
    int16_t key = 0; // in semitones from the wave's base key
    uint16_t velocity = 0x7f;
    float key_sweep = 0.0f;
    float key_sweep_target = 0.0f;
    uint32_t key_sweep_count = 0;
    uint32_t skip_samples = 0;
    uint32_t type = kWave;
    WaveInfo wave;
    uint32_t wave_address = 0; // the waveform's number for a channel of the oscillator type

    bool DolbyMode() const
    {
        return mix_configs[0] == 0xffff;
    }
};

// A DSP channel (JASDSPChannel): one of the 64 that the DSP renders.
struct DspChannel
{
    enum Status : uint32_t
    {
        kActive = 0,
        kInactive = 1,
        kDrop = 2,
    };

    // The callback's types (JASDSPChannel's).
    enum Event : uint32_t
    {
        kEventPlay = 0,
        kEventStart = 1,
        kEventStop = 2,
        kEventDrop = 3,
    };

    uint32_t status = kInactive;
    int16_t priority = -1;
    uint32_t flags = 0;                // bit 0: a start is pending, bit 1: a forced stop is pending
    uint32_t updates = 0;              // sub-frames since the allocation: a new note takes the oldest channel first
    uint32_t channel = Channel::kNone; // the channel that the callback reports to, or kNone with no callback
};

// The tracks' side of the channels' callbacks (JASTrack::channelUpdateCallback). `block` is null for kEventStop.
class ChannelOwner
{
public:
    virtual ~ChannelOwner() = default;

    virtual void OnChannel(uint32_t owner, Channel::Event event, uint32_t channel, DspBlock* block) = 0;
};

// The channels and the DSP channels, and the CPU side's update of the DSP channels each sub-frame. A function that
// can report to the tracks takes them as `owner`. `owner` may be null.
class Voices
{
public:
    static constexpr std::size_t kDspChannels = 64;

    // JASChannel's pool, from JASAudioThread's start-up.
    static constexpr std::size_t kChannels = 72;

    // The DSP channels' blocks start at `blocks` in the emulated memory, one every 0x180 bytes.
    explicit Voices(uint32_t blocks);

    // The output mode (JASDriver's: 0 mono, 1 stereo, 2 surround) and the DSP's level of a channel at full volume.
    void SetOutputMode(uint32_t mode)
    {
        output_mode_ = mode;
    }

    void SetChannelLevel(uint16_t level)
    {
        channel_level_ = level;
    }

    // The DSP's sample rate. The steps of the oscillators and the LFOs follow it.
    void SetDacRate(float rate)
    {
        dac_rate_ = rate;
    }

    float DacRate() const
    {
        return dac_rate_;
    }

    // A new channel (JASChannel's constructor) for `owner`, or for no callback with kNone. Returns its index, or
    // kNone when the pool is empty.
    uint32_t NewChannel(uint32_t owner);

    Channel& ChannelAt(uint32_t index)
    {
        return channels_[index];
    }

    const Channel& ChannelAt(uint32_t index) const
    {
        return channels_[index];
    }

    // JASChannel::play and playForce: a DSP channel for the channel, by the low byte of its priority. Without one,
    // the channel is deleted, and the function returns false. playForce takes a DSP channel of the same priority
    // even while that one is starting.
    bool Play(ChannelOwner* owner, uint32_t channel);
    bool PlayForce(ChannelOwner* owner, uint32_t channel);

    // JASChannel::release: the oscillators' release, with a direct release unless `direct_release` is 0.
    void Release(uint32_t channel, uint16_t direct_release);

    // JASChannel::free: the channel's callback ends.
    void Free(uint32_t channel)
    {
        channels_[channel].owner = Channel::kNone;
    }

    // A sub-frame's update of every DSP channel (JASDSPChannel::updateAll).
    void UpdateDspChannels(DspMemory& memory, ChannelOwner* owner);

    // The step of the free-running LFO at a sub-frame's end, and its phase. An LFO of mode 0 starts from that phase.
    void UpdateFreeRun();

    float FreeRunPhase() const
    {
        return free_run_.Phase();
    }

    const DspChannel& DspChannelAt(uint32_t index) const
    {
        return dsp_channels_[index];
    }

    DspBlock Block(DspMemory& memory, uint32_t dsp_channel) const
    {
        return DspBlock(memory, blocks_ + dsp_channel * static_cast<uint32_t>(DspChannelBlock::kBytes));
    }

    // The DSP channel that JASDSPChannel::getLowestChannel picks for a channel of `priority`, or kNone: a free one,
    // or else the one of the lowest priority, at most `priority`, with the most updates.
    uint32_t LowestChannel(uint32_t priority) const;

    // JASChannel::updateEffectorParam: the mixers' volumes from the channel's levels, or the auto mixer's settings.
    // In Dolby mode, the game passes the DSP mixers' volumes that it never wrote. The model gives 0.
    void UpdateEffectorParam(const Channel& channel, DspBlock& block, std::array<uint16_t, 6>& volumes,
                             const EffectParams& effects) const;

    // The DSP's pitch of the channel, before the setter's limit.
    int32_t Pitch(const Channel& channel, const EffectParams& effects) const;

private:
    // JASDSPChannel::alloc and allocForce. A DSP channel in use goes to the new channel, after a drop event for the
    // old one.
    uint32_t Alloc(ChannelOwner* owner, uint32_t priority, uint32_t channel, bool force);

    // JASDSPChannel::drop's report to the channel on it.
    void ReportDrop(ChannelOwner* owner, uint32_t dsp_channel);

    bool Start(ChannelOwner* owner, uint32_t channel, bool force);

    // The end of a channel that its DSP channel has dropped: the stop event, then the channel's return to the pool.
    void DeleteChannel(ChannelOwner* owner, uint32_t channel);

    // A channel's end in its update or at a DSP channel's stop. The DSP channel's callback ends first.
    int32_t FreeAndDelete(ChannelOwner* owner, uint32_t channel);

    // JASDSPChannel::updateProc, and its callback: the channel's dspUpdateCallback, or the result without one.
    void UpdateProc(DspMemory& memory, ChannelOwner* owner, uint32_t dsp_channel);
    int32_t Callback(DspMemory& memory, ChannelOwner* owner, uint32_t dsp_channel, DspChannel::Event event);

    // JASChannel::initialUpdateDSPChannel and updateDSPChannel.
    int32_t InitialUpdate(ChannelOwner* owner, uint32_t channel, DspBlock& block);
    int32_t Update(ChannelOwner* owner, uint32_t channel, DspBlock& block);

    void Notify(ChannelOwner* owner, uint32_t channel, Channel::Event event, DspBlock* block);

    uint32_t blocks_;
    uint32_t output_mode_ = 1;
    uint16_t channel_level_ = 0x2ee0;
    float dac_rate_ = 32028.46875f;
    std::array<Channel, kChannels> channels_{};
    std::array<uint32_t, kChannels> free_channels_{}; // the pool's free channels, the next one last
    uint32_t free_count_ = 0;
    std::array<DspChannel, kDspChannels> dsp_channels_{};
    Lfo free_run_;
};

// JASChannel::effectOsc: an oscillator's effect on the channel's levels.
void EffectOsc(const Oscillator& oscillator, EffectParams& effects);

} // namespace hsf::jaudio
