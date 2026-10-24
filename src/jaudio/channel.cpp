// SPDX-License-Identifier: MIT

#include "jaudio/channel.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "jaudio/calc.h"

namespace hsf::jaudio
{

namespace
{

// main's constants, by their bits.
const float kPi = std::bit_cast<float>(0x40490fdbu);            // JGeometry's π in single precision
const float kSurroundScale = std::bit_cast<float>(0x3eb20d99u); // 0.34776: a surround level's curve
const float kSurroundBase = std::bit_cast<float>(0x3ea6f933u);  // 0.32612
const float kMonoLevel = std::bit_cast<float>(0x3f34fdf4u);     // 0.707: the auto mixer's level in mono

// The LFOs' and the oscillators' steps per sub-frame at a DSP rate of 32028.5 Hz and 48000 Hz.
constexpr float kLfoRate = 32028.5f;
constexpr float kOscillatorRate = 48000.0f;

// The DSP's buses by a mixer's bus number (JASDsp's connect_table). Number 10 names 0x0b00. The Galaxy games' DSP
// program has no bus there, and the DSP model leaves such a mixer out.
constexpr std::array<uint16_t, 12> kBusAddresses = {0x0000, 0x0d00, 0x0d60, 0x0dc0, 0x0e20, 0x0e80,
                                                    0x0ee0, 0x0ca0, 0x0f40, 0x0fa0, 0x0b00, 0x09a0};

// By a wave's format (0 to 7): the samples that the DSP's first block holds, and the bytes of a block of 16 samples.
// For a format above 7, the game reads both tables past their ends. No game's wave has such a format. The model
// writes 0.
constexpr std::array<uint8_t, 8> kFirstBlockSamples = {0x10, 0x10, 1, 1, 1, 0x10, 0x10, 1};
constexpr std::array<uint8_t, 8> kBlockBytes = {9, 5, 8, 16, 1, 1, 1, 1};

constexpr std::size_t kMixerBytes = 8;
constexpr std::size_t kMixerBus = 0;
constexpr std::size_t kMixerTarget = 2;
constexpr std::size_t kMixerVolume = 4;
constexpr std::size_t kMixerDelays = 6;

constexpr std::size_t kSampleCount = 0x11c;
constexpr std::size_t kStartFlags = 0x10c;

std::size_t MixerOffset(uint32_t mixer)
{
    return DspChannelBlock::kMixers + (mixer & 0xff) * kMixerBytes;
}

void ClearBytes(DspBlock& block, std::size_t start, std::size_t end)
{
    for (std::size_t offset = start; offset < end; offset += 2)
    {
        block.SetU16(offset, 0);
    }
}

} // namespace

void DspBlock::Init()
{
    SetU16(DspChannelBlock::kPause, 0);
    SetU16(DspChannelBlock::kForcedStop, 0);
    SetU32(DspChannelBlock::kActive, 0);
    SetU16(DspChannelBlock::kAutoMixer, 0);
    SetU32(DspChannelBlock::kPcmPosition, 0);
    SetU16(DspChannelBlock::kDist, 0);

    // The FIR filter and the IIR filter each pass the signal through: their first coefficient is 0x7fff.
    ClearBytes(*this, DspChannelBlock::kFirCoefficients, DspChannelBlock::kDist);
    SetU16(DspChannelBlock::kFirCoefficients, 0x7fff);
    SetU16(DspChannelBlock::kIirCoefficients, 0x7fff);
}

void DspBlock::PlayStart()
{
    SetU32(kStartFlags, 0);
    SetU16(DspChannelBlock::kFraction, 0);
    SetU16(DspChannelBlock::kReset, 1);
    SetU16(DspChannelBlock::kLastSample, 0);
    ClearBytes(*this, DspChannelBlock::kHistory, DspChannelBlock::kSavedBlock);
    SetU16(DspChannelBlock::kActive, 1);
}

void DspBlock::PlayStop()
{
    SetU16(DspChannelBlock::kActive, 0);
}

void DspBlock::ReplyFinishRequest()
{
    SetU32(DspChannelBlock::kActive, 0);
}

void DspBlock::ForceStop()
{
    SetU16(DspChannelBlock::kForcedStop, 1);
}

void DspBlock::SetWaveInfo(const WaveInfo& wave, uint32_t wave_address, uint32_t skip)
{
    const bool known = wave.format < kBlockBytes.size();
    const uint32_t block_bytes = known ? kBlockBytes[wave.format] : 0;
    SetU32(DspChannelBlock::kWaveAddress, wave_address);
    SetU16(DspChannelBlock::kLeftover, known ? kFirstBlockSamples[wave.format] : 0);
    SetU32(DspChannelBlock::kPcmPosition, 0);
    SetU16(DspChannelBlock::kFormat, block_bytes);
    if (wave.format > 3)
    {
        return;
    }

    SetU32(kSampleCount, wave.sample_count);
    const uint16_t loops = static_cast<uint16_t>(wave.loop_flags);
    SetU16(DspChannelBlock::kLoops, loops);
    uint32_t end = wave.sample_count;
    if (loops == 0)
    {
        SetU32(DspChannelBlock::kLoopEnd, end);
    }
    else
    {
        if (skip == 1)
        {
            skip = wave.loop_start;
        }
        end = wave.loop_end;
        SetU32(DspChannelBlock::kLoopStart, wave.loop_start);
        SetU32(DspChannelBlock::kLoopEnd, end);
        SetU16(DspChannelBlock::kLoopLast, static_cast<uint16_t>(wave.last));
        SetU16(DspChannelBlock::kLoopPenult, static_cast<uint16_t>(wave.penult));
    }

    // A skip that reaches the end plays from the start. A PCM wave starts at the position. An AFC wave starts at the
    // block that holds it, and the loop's start and the end move back by the skip, in the loop's start's case even
    // for a wave that doesn't loop.
    if (skip != 0 && end > skip)
    {
        if (wave.format >= 2)
        {
            SetU32(DspChannelBlock::kPcmPosition, skip);
        }
        else
        {
            SetU32(DspChannelBlock::kPcmPosition, skip);
            SetU32(DspChannelBlock::kLoopEnd, end - skip);
            SetU32(DspChannelBlock::kWaveAddress, wave_address + ((skip * block_bytes) >> 4));
            SetU32(DspChannelBlock::kLoopStart, U32(DspChannelBlock::kLoopStart) - skip);
        }
    }

    ClearBytes(*this, DspChannelBlock::kSavedBlock, DspChannelBlock::kDelayHistory);
}

void DspBlock::SetOscInfo(uint32_t program)
{
    SetU32(DspChannelBlock::kWaveAddress, 0);
    SetU16(DspChannelBlock::kFormat, program);
    SetU16(DspChannelBlock::kLeftover, 0x10);
}

void DspBlock::InitAutoMixer()
{
    if (U16(DspChannelBlock::kAutoMixer) == 0)
    {
        SetU16(DspChannelBlock::kAutoVolume, 0);
        SetU16(DspChannelBlock::kAutoMixer, 1);
    }
    else
    {
        SetU16(DspChannelBlock::kAutoVolume, U16(DspChannelBlock::kAutoTarget));
    }
}

void DspBlock::SetAutoMixer(int32_t volume, int32_t pan, int32_t dolby, int32_t fxmix)
{
    const uint32_t fx = static_cast<uint32_t>(fxmix) & 0xff;
    SetU16(DspChannelBlock::kAutoTarget, static_cast<uint32_t>(volume));
    SetU16(DspChannelBlock::kAutoPanDolby,
           (static_cast<uint32_t>(pan) & 0xff) << 8 | (static_cast<uint32_t>(dolby) & 0xff));
    SetU16(DspChannelBlock::kAutoFxmix, fx << 8 | fx << 1);
    SetU16(DspChannelBlock::kAutoMixer, 1);
}

void DspBlock::SetPitch(int32_t pitch)
{
    const uint32_t step = static_cast<uint32_t>(pitch) & 0xffff;
    SetU16(DspChannelBlock::kPitch, step < 0x7fff ? step : 0x7fff);
}

void DspBlock::SetMixerInitVolume(uint32_t mixer, uint16_t volume)
{
    const std::size_t offset = MixerOffset(mixer);
    SetU16(offset + kMixerVolume, volume);
    SetU16(offset + kMixerTarget, volume);
    SetU16(offset + kMixerDelays, 0);
}

void DspBlock::SetMixerVolume(uint32_t mixer, uint16_t volume)
{
    if (U16(DspChannelBlock::kForcedStop) != 0)
    {
        return;
    }

    const std::size_t offset = MixerOffset(mixer);
    const uint32_t delays = U16(offset + kMixerDelays) & 0xff;
    SetU16(offset + kMixerTarget, volume);
    SetU16(offset + kMixerDelays, delays);
}

void DspBlock::SetBusConnect(uint32_t mixer, uint32_t bus)
{
    const uint32_t number = bus & 0xff;
    SetU16(MixerOffset(mixer) + kMixerBus, number < kBusAddresses.size() ? kBusAddresses[number] : 0);
}

void DspBlock::SetPauseFlag(uint8_t flag)
{
    SetU16(DspChannelBlock::kPause, flag);
}

void DspBlock::SetIirFilterParam(const std::array<uint16_t, 8>& values)
{
    for (std::size_t i = 0; i < values.size(); i++)
    {
        SetU16(DspChannelBlock::kIirCoefficients + 2 * i, values[i]);
    }
}

void DspBlock::SetFir8FilterParam(const std::array<uint16_t, 8>& values)
{
    for (std::size_t i = 0; i < values.size(); i++)
    {
        SetU16(DspChannelBlock::kFirCoefficients + 2 * i, values[i]);
    }
}

void DspBlock::SetFilterMode(uint32_t mode)
{
    // At most 20 taps with the IIR filter, and 24 without.
    const uint32_t iir = mode & 0x20;
    const uint32_t taps = mode & 0x1f;
    SetU16(DspChannelBlock::kFilterMode, iir | std::min(taps, iir != 0 ? 0x14u : 0x18u));
}

void DspBlock::SetDistFilter(int32_t amount)
{
    SetU16(DspChannelBlock::kDist, static_cast<uint32_t>(amount));
}

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
        effects.pan = (value - 0.5f) + effects.pan;
        break;
    case 3:
        effects.fxmix = value + effects.fxmix;
        break;
    case 4:
        effects.dolby = value + effects.dolby;
        break;
    case 5:
        effects.vibrato = value * effects.vibrato;
        break;
    case 6:
        effects.tremolo = value * effects.tremolo;
        break;
    default:
        break;
    }
}

Voices::Voices(uint32_t blocks) : blocks_(blocks)
{
    // The pool allocates the channels from index 0 up.
    for (std::size_t i = 0; i < kChannels; i++)
    {
        free_channels_[i] = static_cast<uint32_t>(kChannels - 1 - i);
    }
    free_count_ = kChannels;
}

uint32_t Voices::NewChannel(uint32_t owner)
{
    if (free_count_ == 0)
    {
        return Channel::kNone;
    }

    const uint32_t index = free_channels_[--free_count_];
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
        if (level > static_cast<int32_t>(priority) || level > best_priority)
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

void Voices::ReportDrop(ChannelOwner* owner, uint32_t dsp_channel)
{
    const uint32_t channel = dsp_channels_[dsp_channel].channel;
    if (channel != Channel::kNone)
    {
        FreeAndDelete(owner, channel);
    }
}

uint32_t Voices::Alloc(ChannelOwner* owner, uint32_t priority, uint32_t channel, bool force)
{
    const uint32_t index = LowestChannel(priority);
    if (index == Channel::kNone)
    {
        return Channel::kNone;
    }

    // JASDSPChannel::drop: the channel on the DSP channel ends. A DSP channel that plays stops at the next update.
    // allocForce marks it inactive first. The new channel then starts at the next update, with no stop before it.
    DspChannel& dsp = dsp_channels_[index];
    if (force)
    {
        dsp.status = DspChannel::kInactive;
    }
    ReportDrop(owner, index);
    dsp.channel = Channel::kNone;
    dsp.flags &= ~1u;
    if (dsp.status == DspChannel::kActive)
    {
        dsp.flags |= 2;
        dsp.status = DspChannel::kDrop;
    }

    dsp.priority = static_cast<int16_t>(priority);
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
            oscillator.Release();
        }
    }
    dsp_channels_[channel.dsp_channel].priority = static_cast<int16_t>(channel.priority >> 8);
    channel.status = Channel::kRelease;
}

void Voices::DeleteChannel(ChannelOwner* owner, uint32_t channel)
{
    // JASChannel's destructor. In every path that deletes a channel, the channel no longer holds a DSP channel at
    // this point. The destructor's drop of a DSP channel therefore never runs.
    Notify(owner, channel, Channel::kEventStop, nullptr);
    channels_[channel].alive = false;
    free_channels_[free_count_++] = channel;
}

int32_t Voices::FreeAndDelete(ChannelOwner* owner, uint32_t channel)
{
    dsp_channels_[channels_[channel].dsp_channel].channel = Channel::kNone;
    channels_[channel].dsp_channel = Channel::kNone;
    DeleteChannel(owner, channel);

    return -1;
}

int32_t Voices::Callback(DspMemory& memory, ChannelOwner* owner, uint32_t dsp_channel, DspChannel::Event event)
{
    const uint32_t channel = dsp_channels_[dsp_channel].channel;
    DspBlock block = Block(memory, dsp_channel);
    switch (event)
    {
    case DspChannel::kEventPlay:
        return Update(owner, channel, block);
    case DspChannel::kEventStart:
        return InitialUpdate(owner, channel, block);
    case DspChannel::kEventStop:
    case DspChannel::kEventDrop:
        return FreeAndDelete(owner, channel);
    }

    return 0;
}

void Voices::UpdateProc(DspMemory& memory, ChannelOwner* owner, uint32_t index)
{
    DspChannel& dsp = dsp_channels_[index];
    DspBlock block = Block(memory, index);

    // Without a callback, a stop reports -1 and a start or an update 0.
    const auto stop = [&]
    {
        const int32_t result =
            dsp.channel != Channel::kNone ? Callback(memory, owner, index, DspChannel::kEventStop) : -1;
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
        const int32_t result =
            dsp.channel != Channel::kNone ? Callback(memory, owner, index, DspChannel::kEventStart) : 0;
        if (result < 0)
        {
            dsp.status = DspChannel::kInactive;
            stop();
        }
        else
        {
            block.PlayStart();
        }

        return;
    }
    if (dsp.status == DspChannel::kInactive)
    {
        return;
    }

    if (dsp.channel != Channel::kNone && Callback(memory, owner, index, DspChannel::kEventPlay) < 0)
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

void Voices::UpdateFreeRun()
{
    free_run_.IncCounter(kLfoRate / dac_rate_);
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
    const SoundParams& sound = channel.sound_params;
    const ChannelParams& params = channel.params;

    // The pan in stereo and surround, and the surround level in surround. Each sums the levels of the instrument,
    // the oscillators and the track.
    float pan = 0.5f;
    float dolby = 0.0f;
    if (output_mode_ == 1 || output_mode_ == 2)
    {
        pan = (((sound.pan - 0.5f) + 0.5f) + (effects.pan - 0.5f)) + (params.pan - 0.5f);
    }
    if (output_mode_ == 2)
    {
        dolby = (sound.dolby + effects.dolby) + params.dolby;
    }
    const float fxmix = (sound.fxmix + effects.fxmix) + params.fxmix;

    // The volume: the velocity's square, then the three levels and the tremolo. The game fuses the tremolo's
    // product and sum.
    float volume = static_cast<float>(channel.velocity) / 127.0f;
    volume = volume * volume;
    const float levels = (sound.volume * effects.volume) * params.volume;
    volume = volume * (levels * std::fma(channel.tremolo.Value(), effects.tremolo, 1.0f));
    if (volume < 0.0f)
    {
        volume = 0.0f;
    }

    const float clamped_pan = Clamp01(pan);
    const float clamped_fxmix = Clamp01(fxmix);
    const float clamped_dolby = Clamp01(dolby);

    if (channel.DolbyMode())
    {
        float level = volume;
        if (output_mode_ == 0)
        {
            level = level * kMonoLevel;
        }
        level = Clamp01(level);
        block.SetAutoMixer(ToInt32(level * static_cast<float>(channel_level_)), ToInt32(clamped_pan * 127.5f),
                           ToInt32(clamped_dolby * 127.5f), ToInt32(clamped_fxmix * 127.5f));
        volumes.fill(0);
        return;
    }

    // Each mixer scales the volume by up to two levels. The low two nibbles of the mixer's setting name them. A pan
    // or a surround level follows a quarter of a sine in stereo and surround. In the game, one variable holds the
    // level for all the mixers, and no mixer sets it first. A nibble that names no level (4 or above 7) takes the
    // value that the variable last held. For the first mixer, that value is the surround level before its clamp. For
    // a later mixer, it is the last level of an earlier mixer.
    const float far_pan = 1.0f - clamped_pan;
    float scale = dolby;
    const auto pick = [&](uint32_t nibble)
    {
        switch (nibble)
        {
        case 1:
            scale = clamped_pan;
            break;
        case 2:
            scale = clamped_fxmix;
            break;
        case 3:
            scale = clamped_dolby;
            break;
        case 5:
            scale = far_pan;
            break;
        case 6:
            scale = 1.0f - clamped_fxmix;
            break;
        case 7:
            scale = 1.0f - clamped_dolby;
            break;
        default:
            break;
        }
    };
    const auto curve = [&]
    {
        return output_mode_ == 0 ? scale : SinRadian((scale * kPi) * 0.5f);
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
        const uint32_t first = config & 0xf;
        if (first != 0)
        {
            pick(first);
            level = first == 2 || first == 6 ? volume * scale : volume * curve();
        }

        // The second nibble's surround levels follow another curve, in every output mode.
        const uint32_t second = (config >> 4) & 0xf;
        if (second != 0)
        {
            pick(second);
            if (second == 3 || second == 7)
            {
                level = level * SinRadian((std::fma(scale, kSurroundScale, kSurroundBase) * kPi) * 0.5f);
            }
            else if (second == 2 || second == 6)
            {
                level = level * scale;
            }
            else
            {
                level = level * curve();
            }
        }

        volumes[i] = static_cast<uint16_t>(ToInt32(Clamp01(level) * static_cast<float>(channel_level_)));
    }
}

int32_t Voices::Pitch(const Channel& channel, const EffectParams& effects) const
{
    // The key and the sweep in octaves, the track's octaves, and the vibrato. The game fuses the vibrato's product
    // into the sum.
    const float keys = (static_cast<float>(channel.key) + channel.key_sweep) / 12.0f;
    const float octaves = channel.params.octaves + keys;
    const float exponent = std::fma(channel.vibrato.Value(), effects.vibrato, octaves);
    float pitch = channel.sound_params.pitch * effects.pitch * Pow2(exponent) * channel.params.pitch * 4096.0f;
    if (pitch < 0.0f)
    {
        pitch = 0.0f;
    }

    return ToInt32(pitch);
}

int32_t Voices::InitialUpdate(ChannelOwner* owner, uint32_t index, DspBlock& block)
{
    Channel& channel = channels_[index];
    if (channel.DolbyMode())
    {
        block.InitAutoMixer();
    }
    Notify(owner, index, Channel::kEventStart, &block);

    // The game ends a channel here whose wave's group is no longer loaded, or whose bank has gone. A rip loads every
    // wave and bank for as long as it plays.
    if (channel.type == Channel::kWave)
    {
        block.SetWaveInfo(channel.wave, channel.wave_address, channel.skip_samples);
    }
    else if (channel.type == Channel::kOsc)
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
    for (Oscillator& oscillator : channel.oscillators)
    {
        if (oscillator.Valid())
        {
            oscillator.Update();
            EffectOsc(oscillator, effects);
        }
    }
    channel.vibrato.ResetCounter(free_run_.Phase());
    channel.tremolo.ResetCounter(free_run_.Phase());

    std::array<uint16_t, 6> volumes{};
    UpdateEffectorParam(channel, block, volumes, effects);
    for (uint32_t i = 0; i < volumes.size(); i++)
    {
        block.SetMixerInitVolume(i, volumes[i]);
    }
    block.SetPitch(Pitch(channel, effects));
    block.SetPauseFlag(channel.pause ? 1 : 0);
    block.SetU16(DspChannelBlock::kLastSample, 0);

    return 0;
}

int32_t Voices::Update(ChannelOwner* owner, uint32_t index, DspBlock& block)
{
    Channel& channel = channels_[index];
    Notify(owner, index, Channel::kEventPlay, &block);
    block.SetPauseFlag(channel.pause ? 1 : 0);

    // A paused channel keeps its oscillators and LFOs where they are, and its levels leave the oscillators out. A
    // paused channel in its release ends.
    EffectParams effects;
    if (channel.pause)
    {
        if (channel.oscillators[0].Releasing())
        {
            return FreeAndDelete(owner, index);
        }
    }
    else
    {
        const float lfo_step = kLfoRate / dac_rate_;
        channel.vibrato.IncCounter(lfo_step);
        channel.tremolo.IncCounter(lfo_step);
        if (channel.update_timer != 0)
        {
            channel.update_timer--;
            if (channel.update_timer == 0)
            {
                Notify(owner, index, Channel::kEventTimer, &block);
            }
        }

        // The channel ends when its first oscillator stops.
        const float step = kOscillatorRate / dac_rate_;
        for (std::size_t i = 0; i < channel.oscillators.size(); i++)
        {
            Oscillator& oscillator = channel.oscillators[i];
            if (!oscillator.Valid())
            {
                continue;
            }
            oscillator.IncCounter(step);
            EffectOsc(oscillator, effects);
            if (i == 0 && oscillator.Stopped())
            {
                return FreeAndDelete(owner, index);
            }
        }
    }

    std::array<uint16_t, 6> volumes{};
    UpdateEffectorParam(channel, block, volumes, effects);
    for (uint32_t i = 0; i < volumes.size(); i++)
    {
        block.SetMixerVolume(i, volumes[i]);
    }
    block.SetPitch(Pitch(channel, effects));

    if (!channel.pause && channel.key_sweep_count != 0)
    {
        channel.key_sweep = channel.key_sweep + (channel.key_sweep_target - channel.key_sweep) /
                                                    static_cast<float>(channel.key_sweep_count);
        channel.key_sweep_count--;
    }

    return 0;
}

} // namespace hsf::jaudio
