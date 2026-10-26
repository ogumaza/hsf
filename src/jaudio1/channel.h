// SPDX-License-Identifier: MIT

// JAudio1's channels as the Switch release of Pikmin 2 builds them for AArch64: a note's channel (JASChannel) with its
// four oscillators, and the 64 DSP channels that the notes play on (JASDSPChannel). The CPU side's writes to a DSP
// channel's block (JASDsp's TChannel) are the Galaxy games' (jaudio/channel.h). Each sub-frame, the CPU side updates
// every DSP channel. A note's channel then asks its track for the track's levels, moves its oscillators, and writes its
// mixers' volumes, its pitch and its pause into the block.
//
// The channels and the DSP channels refer to one another by index.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>

#include "jaudio/channel.h"
#include "jaudio/dsp.h"
#include "jaudio1/oscillator.h"

namespace hsf::jaudio1
{

using jaudio::DspBlock;
using jaudio::DspChannelBlock;
using jaudio::DspMemory;
using jaudio::WaveInfo;

// What a channel's oscillators do to its levels (JASChannel::EffectOscParam).
struct EffectParams
{
    float volume = 1.0f;
    float pitch = 1.0f;
    float pan = 0.5f;
    float fxmix = 0.0f;
    float dolby = 0.0f;
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

    // The events that a channel's callback reports to its owner. A timer's event releases the note.
    enum Event : uint32_t
    {
        kEventPlay = 0,
        kEventStart = 1,
        kEventStop = 2,
        kEventTimer = 3,
    };

    // The wave's formats: a wave, or the DSP's oscillator by its waveform's number.
    static constexpr uint8_t kWave = 0;
    static constexpr uint8_t kOsc = 2;

    // A channel is in use from its construction to its deletion.
    bool alive = false;

    uint32_t status = kStop;
    bool pause = false;
    uint32_t dsp_channel = kNone;
    uint32_t owner = kNone; // the track that the callback reports to, or kNone with no callback
    uint32_t update_timer = 0;
    std::array<Oscillator, 4> oscillators{};

    // Each mixer's bus in the high byte, and in each of the low two nibbles a level that scales it, the high nibble
    // first: 1 the pan, 2 the effect send, 3 the surround level, 5 to 7 the same subtracted from 1. 0xffff in the
    // first switches to the auto mixer of Dolby mode.
    std::array<uint16_t, 6> mix_configs = {0x150, 0x210, 0x352, 0x412, 0, 0};
    uint16_t priority = 0x13f; // the low byte while the channel plays, the high byte from its release

    float target_pitch = 1.0f; // the key sweep's target
    uint32_t sweep_steps = 0;
    uint32_t skip_samples = 0;

    // The levels by their sources: the instrument's (sound) and the track's (channel). The oscillators give the third.
    float pan_sound = 0.5f;
    float pan_channel = 0.5f;
    float fxmix_sound = 0.0f;
    float fxmix_channel = 0.0f;
    float dolby_sound = 0.0f;
    float dolby_channel = 0.0f;

    uint8_t wave_format = kWave;
    bool has_wave = false; // whether the channel has a wave's settings
    WaveInfo wave;
    uint32_t wave_address = 0; // the waveform's number for the DSP's oscillator

    float active_pitch = 0.0f; // the wave's rate against the DSP's, times the instrument's pitch
    float volume = 1.0f;       // the instrument's volume
    float modified_pitch = 1.0f;
    float velocity_volume = 1.0f; // the velocity's square, times the instrument's volume
    float volume_channel = 1.0f;
    float pitch_channel = 1.0f;

    // How the three sources of the pan, the effect send and the surround level add up (an index into
    // JASChannel::calc_sw_table), and the weights of the sources: sound, effect, channel.
    uint8_t pan_calc = 26;
    uint8_t fxmix_calc = 1;
    uint8_t dolby_calc = 1;
    std::array<float, 3> pan_power{};

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

    // The sub-frames of a frame (JASDriver's): the DSP renders 80 samples in each, and the CPU side updates the DSP
    // channels once in each.
    static constexpr uint32_t kSubframes = 7;

    // The DSP channels' blocks start at `blocks` in the emulated memory, one every 0x180 bytes.
    explicit Voices(uint32_t blocks);

    // The levels of a channel at full volume for the mixers and for the auto mixer.
    void SetChannelLevel(uint16_t level)
    {
        channel_level_ = level;
    }

    void SetAutoLevel(uint16_t level)
    {
        auto_level_ = level;
    }

    // The DSP's sample rate (JASDriver::getDacRate). The oscillators' steps follow it.
    float DacRate() const
    {
        return dac_rate_;
    }

    // A new channel from the pool (JASChannel's constructor) for `owner`, or for no callback with kNone. Returns its
    // index, or kNone when the pool is empty.
    uint32_t NewChannel(uint32_t owner);

    Channel& ChannelAt(uint32_t index)
    {
        return channels_[index];
    }

    const Channel& ChannelAt(uint32_t index) const
    {
        return channels_[index];
    }

    // JASChannel::play and playForce: a DSP channel for the channel, by the low byte of its priority. Without one, the
    // channel is deleted, and the function returns false.
    bool Play(ChannelOwner* owner, uint32_t channel);
    bool PlayForce(ChannelOwner* owner, uint32_t channel);

    // JASChannel::release: the oscillators' release, with a direct release unless `direct_release` is 0.
    void Release(uint32_t channel, uint16_t direct_release);

    // JASChannel::free: the channel's callback ends.
    void Free(uint32_t channel)
    {
        channels_[channel].owner = Channel::kNone;
    }

    // JASChannel::setPanPower: the weights of the three sources, normalized.
    void SetPanPower(uint32_t channel, float sound, float effect, float track);

    // JASChannel::setKeySweepTarget: the pitch of `key` against the wave's base key, at once with no steps.
    void SetKeySweepTarget(uint32_t channel, uint32_t key, uint32_t steps);

    // A sub-frame's update of every DSP channel (JASDSPChannel::updateAll).
    void UpdateDspChannels(DspMemory& memory, ChannelOwner* owner);

    DspChannel& DspChannelAt(uint32_t index)
    {
        return dsp_channels_[index];
    }

    const DspChannel& DspChannelAt(uint32_t index) const
    {
        return dsp_channels_[index];
    }

    DspBlock Block(DspMemory& memory, uint32_t dsp_channel) const
    {
        return DspBlock(memory, blocks_ + dsp_channel * static_cast<uint32_t>(DspChannelBlock::kBytes));
    }

    // The pool's free channels, the next one first.
    const std::deque<uint32_t>& FreeChannels() const
    {
        return free_channels_;
    }

    // The DSP channel that JASDSPChannel::getLowestChannel picks for a channel of `priority`, or kNone: a free one, or
    // else the one of the lowest priority, at most `priority`, with the most updates.
    uint32_t LowestChannel(uint32_t priority) const;

    // JASChannel::updateEffectorParam: the mixers' volumes from the channel's levels, or the auto mixer's settings.
    // In Dolby mode, the volumes stay 0.
    void UpdateEffectorParam(const Channel& channel, DspBlock& block, std::array<uint16_t, 6>& volumes,
                             const EffectParams& effects) const;

    // The DSP's pitch of the channel, before the setter's limit.
    static int32_t Pitch(const Channel& channel, const EffectParams& effects);

private:
    uint32_t Alloc(ChannelOwner* owner, uint32_t priority, uint32_t channel, bool force);
    void Drop(ChannelOwner* owner, uint32_t dsp_channel);
    bool Start(ChannelOwner* owner, uint32_t channel, bool force);

    // The deletion of a channel: the stop event, then the channel's return to the pool.
    void DeleteChannel(ChannelOwner* owner, uint32_t channel);

    // The channel's end in its update or at its DSP channel's stop or drop. The DSP channel's callback ends first.
    int32_t FreeAndDelete(ChannelOwner* owner, uint32_t channel);

    void UpdateProc(DspMemory& memory, ChannelOwner* owner, uint32_t dsp_channel);
    int32_t Callback(DspMemory& memory, ChannelOwner* owner, uint32_t dsp_channel, uint32_t event);

    // JASChannel::initialUpdateDSPChannel and updateDSPChannel.
    void InitialUpdate(ChannelOwner* owner, uint32_t channel, DspBlock& block);
    int32_t Update(ChannelOwner* owner, uint32_t channel, DspBlock& block);

    void Notify(ChannelOwner* owner, uint32_t channel, Channel::Event event, DspBlock* block);

    // The mixers' volumes (JASChannel::updateMixer). In the game, one variable holds the level for all the mixers. A
    // nibble that names no level (4 or above 7) takes the value that the variable last held. `scale` is its value
    // before the first mixer: what the caller's register held, the oscillators' effect send.
    void UpdateMixer(const Channel& channel, float volume, float pan, float fxmix, float dolby, float scale,
                     std::array<uint16_t, 6>& volumes) const;

    uint32_t blocks_;
    uint32_t output_mode_ = 1; // JASDriver's output mode: 0 mono, 1 stereo (the game's), 2 surround
    uint16_t channel_level_ = 0x2ee0;
    uint16_t auto_level_ = 0x2ee0;
    float dac_rate_ = 32028.5f;
    std::array<Channel, kChannels> channels_{};
    std::deque<uint32_t> free_channels_;
    std::array<DspChannel, kDspChannels> dsp_channels_{};
};

// JASChannel::effectOsc: an oscillator's effect on the channel's levels.
void EffectOsc(const Oscillator& oscillator, EffectParams& effects);

} // namespace hsf::jaudio1
