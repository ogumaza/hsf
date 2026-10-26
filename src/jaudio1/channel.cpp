// SPDX-License-Identifier: MIT

#include "jaudio1/channel.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "jaudio1/calc.h"

namespace hsf::jaudio1
{

namespace
{

// main's constants, by their bits: a surround level's curve in a mixer's second level.
const float kSurroundScale = std::bit_cast<float>(0x3eb20d99u); // 0.34776
const float kSurroundBase = std::bit_cast<float>(0x3ea6f933u);  // 0.32612

// JASChannel::calc_sw_table: how each source of a level adds up, for the sound, the oscillators' effect and the
// track's channel: 0 not at all, 1 as it is, 2 by its weight. The game reads the bytes after the table for a type
// above 26. None of them is 1 or 2.
constexpr std::array<std::array<uint8_t, 3>, 27> kCalcTable = {{
    {0, 0, 0}, {0, 0, 1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 1}, {0, 1, 2}, {0, 1, 0}, {0, 2, 1}, {0, 2, 2},
    {1, 0, 0}, {1, 0, 1}, {1, 0, 2}, {1, 1, 0}, {1, 1, 1}, {1, 1, 2}, {1, 2, 0}, {1, 2, 1}, {1, 2, 2},
    {1, 0, 0}, {2, 0, 1}, {2, 0, 2}, {2, 1, 0}, {2, 1, 1}, {2, 1, 2}, {2, 2, 0}, {2, 2, 1}, {2, 2, 2},
}};

std::array<uint8_t, 3> CalcTypes(uint8_t type)
{
    return type < kCalcTable.size() ? kCalcTable[type] : std::array<uint8_t, 3>{};
}

// JASChannel::calcEffect: the effect send or the surround level from its three sources.
float CalcEffect(const std::array<float, 3>& sources, const std::array<float, 3>& power, uint8_t type)
{
    const std::array<uint8_t, 3> types = CalcTypes(type);
    float value = 0.0f;
    for (std::size_t i = 0; i < 3; i++)
    {
        if (types[i] == 1)
        {
            value = value + sources[i];
        }
        else if (types[i] == 2)
        {
            value = value + power[i] * sources[i];
        }
    }

    return value;
}

// JASChannel::calcPan: the pan from its three sources, each as its distance from the centre.
float CalcPan(const std::array<float, 3>& sources, const std::array<float, 3>& power, uint8_t type)
{
    const std::array<uint8_t, 3> types = CalcTypes(type);
    float value = 0.0f;
    for (std::size_t i = 0; i < 3; i++)
    {
        if (types[i] == 1)
        {
            value = value + (sources[i] + -0.5f);
        }
        else if (types[i] == 2)
        {
            value = value + (sources[i] + -0.5f) * power[i];
        }
    }

    return value + 0.5f;
}

// The game's inline clamp of a level to 0 to 1. A NaN gives 0.
float ClampLevel(float value)
{
    return value > 0.0f ? std::fmin(value, 1.0f) : 0.0f;
}

} // namespace

void EffectOsc(const Oscillator& oscillator, EffectParams& effects)
{
    const float value = oscillator.Value();
    switch (oscillator.Target())
    {
    case 0:
        effects.volume = value * effects.volume;
        break;
    case 1:
        effects.pitch = value * effects.pitch;
        break;
    case 2:
        effects.pan = (value + -0.5f) + effects.pan;
        break;
    case 3:
        effects.fxmix = value + effects.fxmix;
        break;
    case 4:
        effects.dolby = value + effects.dolby;
        break;
    default:
        break;
    }
}

Voices::Voices(uint32_t blocks) : blocks_(blocks)
{
    // The pool's start-up puts each new channel at the front: the last one is the first to go.
    for (std::size_t i = 0; i < kChannels; i++)
    {
        free_channels_.push_back(static_cast<uint32_t>(kChannels - 1 - i));
    }
}

uint32_t Voices::NewChannel(uint32_t owner)
{
    if (free_channels_.empty())
    {
        return Channel::kNone;
    }

    const uint32_t index = free_channels_.front();
    free_channels_.pop_front();
    channels_[index] = Channel{};
    channels_[index].alive = true;
    channels_[index].owner = owner;

    return index;
}

uint32_t Voices::LowestChannel(uint32_t priority) const
{
    uint32_t best = Channel::kNone;
    int32_t best_priority = 0xff;
    uint32_t best_updates = 0;
    for (uint32_t i = 0; i < kDspChannels; i++)
    {
        const DspChannel& dsp = dsp_channels_[i];
        const int32_t level = dsp.priority;
        if (level < 0)
        {
            return i;
        }
        if (level > best_priority || level > static_cast<int32_t>(priority & 0xff))
        {
            continue;
        }
        if (level != best_priority || dsp.updates > best_updates)
        {
            best = i;
            best_priority = level;
            best_updates = dsp.updates;
        }
    }

    return best;
}

void Voices::Drop(ChannelOwner* owner, uint32_t index)
{
    // JASDSPChannel::drop: the channel on the DSP channel ends. A DSP channel that plays stops at its next update.
    DspChannel& dsp = dsp_channels_[index];
    if (dsp.channel != Channel::kNone)
    {
        FreeAndDelete(owner, dsp.channel);
    }
    dsp.channel = Channel::kNone;
    dsp.priority = -1;
    dsp.flags &= ~1u;
    if (dsp.status == DspChannel::kActive)
    {
        dsp.flags |= 2;
        dsp.status = DspChannel::kDrop;
    }
}

uint32_t Voices::Alloc(ChannelOwner* owner, uint32_t priority, uint32_t channel, bool force)
{
    const uint32_t index = LowestChannel(priority);
    if (index == Channel::kNone)
    {
        return Channel::kNone;
    }

    // allocForce marks the DSP channel inactive first. The new channel then starts at the next update, with no stop
    // before it.
    DspChannel& dsp = dsp_channels_[index];
    if (force)
    {
        dsp.status = DspChannel::kInactive;
    }
    Drop(owner, index);
    dsp.priority = static_cast<int16_t>(priority & 0xff);
    dsp.updates = 0;
    dsp.channel = channel;

    return index;
}

bool Voices::Start(ChannelOwner* owner, uint32_t channel, bool force)
{
    const uint32_t dsp_channel = Alloc(owner, channels_[channel].priority & 0xff, channel, force);
    if (dsp_channel == Channel::kNone)
    {
        DeleteChannel(owner, channel);
        return false;
    }

    channels_[channel].dsp_channel = dsp_channel;
    dsp_channels_[dsp_channel].flags |= 1;
    channels_[channel].status = Channel::kPlay;

    return true;
}

bool Voices::Play(ChannelOwner* owner, uint32_t channel)
{
    return Start(owner, channel, false);
}

bool Voices::PlayForce(ChannelOwner* owner, uint32_t channel)
{
    return Start(owner, channel, true);
}

void Voices::Release(uint32_t index, uint16_t direct_release)
{
    Channel& channel = channels_[index];
    if (channel.status != Channel::kPlay)
    {
        return;
    }

    if (direct_release != 0)
    {
        channel.oscillators[0].SetDirectRelease(direct_release);
    }
    for (Oscillator& oscillator : channel.oscillators)
    {
        if (oscillator.Valid())
        {
            oscillator.Release(dac_rate_);
        }
    }
    dsp_channels_[channel.dsp_channel].priority = static_cast<int16_t>((channel.priority >> 8) & 0xff);
    channel.status = Channel::kRelease;
}

void Voices::SetPanPower(uint32_t index, float sound, float effect, float track)
{
    const float sum = (sound + effect) + track;
    channels_[index].pan_power = {sound / sum, effect / sum, track / sum};
}

void Voices::SetKeySweepTarget(uint32_t index, uint32_t key, uint32_t steps)
{
    Channel& channel = channels_[index];
    int32_t semitones = static_cast<int32_t>(key & 0xff);
    if (channel.wave_format != Channel::kOsc && channel.has_wave)
    {
        semitones = semitones - channel.wave.base_key + 60;
    }
    const float pitch = Key2Pitch(semitones) * channel.active_pitch;
    if (steps == 0)
    {
        channel.modified_pitch = pitch;
    }
    else
    {
        channel.target_pitch = pitch;
    }
    channel.sweep_steps = steps;
}

void Voices::DeleteChannel(ChannelOwner* owner, uint32_t channel)
{
    // JASChannel's destructor. In every path that deletes a channel, the channel no longer holds a DSP channel at
    // this point. The destructor's drop of a DSP channel therefore never runs.
    Notify(owner, channel, Channel::kEventStop, nullptr);
    channels_[channel].alive = false;
    free_channels_.push_back(channel);
}

int32_t Voices::FreeAndDelete(ChannelOwner* owner, uint32_t channel)
{
    dsp_channels_[channels_[channel].dsp_channel].channel = Channel::kNone;
    channels_[channel].dsp_channel = Channel::kNone;
    DeleteChannel(owner, channel);

    return -1;
}

int32_t Voices::Callback(DspMemory& memory, ChannelOwner* owner, uint32_t dsp_channel, uint32_t event)
{
    // JASChannel::dspUpdateCallback.
    const uint32_t channel = dsp_channels_[dsp_channel].channel;
    DspBlock block = Block(memory, dsp_channel);
    switch (event)
    {
    case 0:
        return Update(owner, channel, block);

    case 1:
        InitialUpdate(owner, channel, block);
        return 0;

    case 2:
    case 3:
        return FreeAndDelete(owner, channel);

    default:
        return 0;
    }
}

void Voices::UpdateProc(DspMemory& memory, ChannelOwner* owner, uint32_t index)
{
    DspChannel& dsp = dsp_channels_[index];
    DspBlock block = Block(memory, index);

    // Without a callback, a stop reports -1.
    const auto stop = [&]
    {
        const int32_t result = dsp.channel != Channel::kNone ? Callback(memory, owner, index, 2) : -1;
        if (result < 0)
        {
            dsp.priority = -1;
        }
    };

    if (block.IsFinish())
    {
        dsp.flags &= ~2u;
        if (dsp.status == DspChannel::kActive)
        {
            stop();
        }
        dsp.status = DspChannel::kInactive;
        block.ReplyFinishRequest();
        return;
    }
    if ((dsp.flags & 2) != 0)
    {
        dsp.flags &= ~2u;
        block.ForceStop();
        return;
    }
    if (dsp.status == DspChannel::kDrop)
    {
        return;
    }

    if ((dsp.flags & 1) != 0 && dsp.status == DspChannel::kInactive)
    {
        dsp.flags &= ~1u;
        dsp.status = DspChannel::kActive;
        block.Init();
        if (dsp.channel != Channel::kNone)
        {
            Callback(memory, owner, index, 1);
        }
        block.PlayStart();
        return;
    }
    if (dsp.status == DspChannel::kInactive)
    {
        return;
    }

    if (dsp.channel != Channel::kNone && Callback(memory, owner, index, 0) < 0)
    {
        dsp.status = DspChannel::kInactive;
        stop();
        block.PlayStop();
        return;
    }
    dsp.updates++;
}

void Voices::UpdateDspChannels(DspMemory& memory, ChannelOwner* owner)
{
    for (uint32_t i = 0; i < kDspChannels; i++)
    {
        UpdateProc(memory, owner, i);
    }
}

void Voices::Notify(ChannelOwner* owner, uint32_t channel, Channel::Event event, DspBlock* block)
{
    const uint32_t target = channels_[channel].owner;
    if (owner != nullptr && target != Channel::kNone)
    {
        owner->OnChannel(target, event, channel, block);
    }
}

void Voices::UpdateEffectorParam(const Channel& channel, DspBlock& block, std::array<uint16_t, 6>& volumes,
                                 const EffectParams& effects) const
{
    const std::array<float, 3> pans = {channel.pan_sound, effects.pan, channel.pan_channel};
    const std::array<float, 3> fxmixes = {channel.fxmix_sound, effects.fxmix, channel.fxmix_channel};
    const std::array<float, 3> dolbies = {channel.dolby_sound, effects.dolby, channel.dolby_channel};

    // In mono, the pan stays in the centre. Only surround has a surround level.
    float pan = 0.5f;
    float fxmix = 0.0f;
    float dolby = 0.0f;
    if (output_mode_ <= 2)
    {
        fxmix = CalcEffect(fxmixes, channel.pan_power, channel.fxmix_calc);
    }
    if ((output_mode_ == 1 || output_mode_ == 2) && channel.pan_calc != 0)
    {
        pan = CalcPan(pans, channel.pan_power, channel.pan_calc);
    }
    if (output_mode_ == 2)
    {
        dolby = CalcEffect(dolbies, channel.pan_power, channel.dolby_calc);
    }

    const float volume = (channel.velocity_volume * effects.volume) * channel.volume_channel;
    const float clamped_pan = ClampLevel(pan);
    const float clamped_fxmix = ClampLevel(fxmix);
    const float clamped_dolby = ClampLevel(dolby);

    if (!channel.DolbyMode())
    {
        UpdateMixer(channel, volume, clamped_pan, clamped_fxmix, clamped_dolby, effects.fxmix, volumes);
        return;
    }

    // The auto mixer. The game then writes mixer volumes that it never set. The model writes 0.
    const float level = ClampLevel(volume);
    block.SetAutoMixer(ToInt32(level * static_cast<float>(auto_level_)), ToInt32(clamped_pan * 127.5f),
                       ToInt32(clamped_dolby * 127.5f), ToInt32(clamped_fxmix * 127.5f));
    volumes.fill(0);
}

void Voices::UpdateMixer(const Channel& channel, float volume, float pan, float fxmix, float dolby, float scale,
                         std::array<uint16_t, 6>& volumes) const
{
    const float far_dolby = 1.0f - dolby;
    const float far_fxmix = 1.0f - fxmix;
    const float far_pan = 1.0f - pan;
    const auto pick = [&](uint32_t nibble)
    {
        switch (nibble)
        {
        case 1:
            scale = pan;
            break;
        case 2:
            scale = fxmix;
            break;
        case 3:
            scale = dolby;
            break;
        case 5:
            scale = far_pan;
            break;
        case 6:
            scale = far_fxmix;
            break;
        case 7:
            scale = far_dolby;
            break;
        default:
            break;
        }
    };

    for (std::size_t i = 0; i < volumes.size(); i++)
    {
        const uint32_t config = channel.mix_configs[i];
        if (config < 0x100)
        {
            volumes[i] = 0;
            continue;
        }

        float level = volume;
        const uint32_t first = (config >> 4) & 0xf;
        if (first != 0)
        {
            pick(first);
            level = first == 2 || first == 6 ? scale * volume : SinRadian(scale * kHalfPi) * volume;
        }

        const uint32_t second = config & 0xf;
        if (second != 0)
        {
            pick(second);
            if (second == 3 || second == 7)
            {
                level = level * SinRadian((scale * kSurroundScale + kSurroundBase) * kHalfPi);
            }
            else if (second == 2 || second == 6)
            {
                level = level * scale;
            }
            else
            {
                level = level * SinRadian(scale * kHalfPi);
            }
        }

        volumes[i] = static_cast<uint16_t>(ToInt32(ClampLevel(level) * static_cast<float>(channel_level_)));
    }
}

int32_t Voices::Pitch(const Channel& channel, const EffectParams& effects)
{
    return ToInt32(((channel.modified_pitch * effects.pitch) * channel.pitch_channel) * 4096.0f);
}

void Voices::InitialUpdate(ChannelOwner* owner, uint32_t index, DspBlock& block)
{
    Notify(owner, index, Channel::kEventStart, &block);
    Channel& channel = channels_[index];

    if (channel.wave_format == Channel::kWave)
    {
        block.SetWaveInfo(channel.wave, channel.wave_address, channel.skip_samples);
    }
    else if (channel.wave_format == Channel::kOsc)
    {
        block.SetOscInfo(channel.wave_address);
    }

    // Buses 8 and 9 serve surround. In stereo, bus 8 goes to bus 11. In mono, bus 9 goes to the right output too.
    for (uint32_t i = 0; i < channel.mix_configs.size(); i++)
    {
        uint32_t bus = channel.mix_configs[i] >> 8;
        if (output_mode_ == 0)
        {
            bus = bus == 8 ? 11 : bus == 9 ? 2 : bus;
        }
        else if (output_mode_ == 1 && bus == 8)
        {
            bus = 11;
        }
        block.SetBusConnect(i, bus);
    }

    EffectParams effects;
    for (const Oscillator& oscillator : channel.oscillators)
    {
        if (oscillator.Valid())
        {
            EffectOsc(oscillator, effects);
        }
    }

    std::array<uint16_t, 6> volumes{};
    UpdateEffectorParam(channel, block, volumes, effects);
    for (uint32_t i = 0; i < volumes.size(); i++)
    {
        block.SetMixerInitVolume(i, volumes[i]);
    }
    block.SetPitch(Pitch(channel, effects));
    block.SetPauseFlag(channel.pause ? 1 : 0);
}

int32_t Voices::Update(ChannelOwner* owner, uint32_t index, DspBlock& block)
{
    // The game ends a channel here whose wave's group is no longer loaded. A rip loads every wave for as long as it
    // plays.
    Notify(owner, index, Channel::kEventPlay, &block);
    Channel& channel = channels_[index];
    if (channel.update_timer != 0)
    {
        channel.update_timer--;
        if (channel.update_timer == 0)
        {
            Notify(owner, index, Channel::kEventTimer, &block);
        }
    }

    // The channel ends when its first oscillator stops.
    EffectParams effects;
    for (std::size_t i = 0; i < channel.oscillators.size(); i++)
    {
        Oscillator& oscillator = channel.oscillators[i];
        if (!oscillator.Valid())
        {
            continue;
        }

        oscillator.IncCounter(dac_rate_);
        EffectOsc(oscillator, effects);
        if (i == 0 && oscillator.Stopped())
        {
            return FreeAndDelete(owner, index);
        }
    }

    std::array<uint16_t, 6> volumes{};
    UpdateEffectorParam(channel, block, volumes, effects);
    if (channel.sweep_steps != 0)
    {
        const float steps = static_cast<float>(channel.sweep_steps);
        channel.sweep_steps--;
        channel.modified_pitch = channel.modified_pitch + (channel.target_pitch - channel.modified_pitch) / steps;
    }
    for (uint32_t i = 0; i < volumes.size(); i++)
    {
        block.SetMixerVolume(i, volumes[i]);
    }
    block.SetPitch(Pitch(channel, effects));
    block.SetPauseFlag(channel.pause ? 1 : 0);

    return 0;
}

} // namespace hsf::jaudio1
