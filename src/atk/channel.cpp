// SPDX-License-Identifier: MIT

#include "atk/channel.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace hsf::atk
{
namespace
{

// The game's 1/255 (0x1af0b2c), the float nearest to it.
const float kOneOver255 = std::bit_cast<float>(0x3B808081u);

} // namespace

// ----------------------------------------------------------------------------------------------------------- Channel

void Channel::InitParam(Callback owner)
{
    auto_sweep = true;
    release_priority_fix = false;
    callback = std::move(owner);
    key = 60;
    original_key = 60;
    next_in_track = nullptr;
    pause = false;
    ignore_note_off = false;
    length = 0;
    loop_ = false;
    original_loop_start_ = 0;
    loop_start_ = 0;
    init_pan = 0.0f;
    init_span = 0.0f;
    tune = 1.0f;
    cached_pitch = 0.0f;
    cached_ratio = 1.0f;
    biquad_value = 0.0f;
    output_line = 1;
    track_volume = 1.0f;
    bend = 0.0f;
    user_pitch = 1.0f;
    lpf = 0.0f;
    biquad_type = 0;
    output.volume = 1.0f;
    output.mix_mode = 0;
    output.pan = 0.0f;
    output.span = 0.0f;
    output.main_send = 0.0f;
    output.fx_send = {};
    silence_from = 255;
    silence_to = 255;
    silence_duration = 0;
    silence_counter = 0;
    sweep_pitch = 0.0f;
    sweep_counter = 0;
    sweep_length = 0;
    envelope.Initialize(CurveAdshr::kInitialDecibel);
    release_requested_ = false;
    updated_ = false;

    // No LFO steps until the track gives the channel its types.
    for (std::size_t i = 0; i < lfos.size(); ++i)
    {
        lfos[i].param = CurveLfoParam{};
        lfo_types[i] = 0xFF;
    }
    pan_mode = 0;
    pan_curve = 0;
    key_group = 0;

    // From the hardware manager's +0xe8. The renderer never gets it.
    interpolation_type = 0;
    init_volume = 1.0f;
    velocity_volume = 1.0f;
}

void Channel::Start(const WaveInfo& wave, int32_t note_length)
{
    length = note_length;
    for (CurveLfo& lfo : lfos)
    {
        lfo.Reset();
    }
    envelope.Reset(CurveAdshr::kInitialDecibel);
    sweep_counter = 0;
    voice->SetSampleFormat(wave.format);
    voice->SetSampleRate(wave.sample_rate);
    voice->SetInterpolationType(interpolation_type);
    AppendWaveBuffer(wave);
    voice->Start();
    active = true;
}

void Channel::AppendWaveBuffer(const WaveInfo& wave)
{
    // Each buffer has all the channel's samples. The first plays from the start to the end, and for a wave that loops,
    // the second plays its loop, for ever.
    loop_ = wave.loop;
    loop_start_ = wave.loop_start;
    original_loop_start_ = wave.original_loop_start;
    const bool adpcm = wave.format == SampleFormat::kDspAdpcm;
    for (int channel = 0; channel < voice->ChannelCount(); ++channel)
    {
        const std::size_t c = static_cast<std::size_t>(channel);
        const WaveInfo::Channel& data = wave.channels[c];
        if (adpcm)
        {
            contexts_[c] = data.context;
            if (wave.loop)
            {
                loop_contexts_[c] = data.loop_context;
            }
            voice->SetAdpcmParam(channel, data.coefs);
        }

        WaveBuffer& start = wave_buffers_[c][0];
        start.data = data.data;
        start.sample_offset = 0;
        start.sample_length = wave.loop_end;
        start.loop = false;
        start.adpcm_context = adpcm ? &contexts_[c] : nullptr;
        if (wave.loop)
        {
            WaveBuffer& loop = wave_buffers_[c][1];
            loop.data = data.data;
            loop.sample_offset = loop_start_;
            loop.sample_length = wave.loop_end;
            loop.loop = true;
            loop.adpcm_context = adpcm ? &loop_contexts_[c] : nullptr;
            voice->AppendWaveBuffer(channel, start, false);
            voice->AppendWaveBuffer(channel, loop, true);
        }
        else
        {
            voice->AppendWaveBuffer(channel, start, true);
        }
    }
}

float Channel::GetSweepValue() const
{
    if (sweep_pitch == 0.0f || sweep_counter >= sweep_length)
    {
        return 0.0f;
    }

    return (sweep_pitch * static_cast<float>(sweep_length - sweep_counter)) / static_cast<float>(sweep_length);
}

void Channel::Update(bool periodic, int frames, Random& random)
{
    if (!active)
    {
        return;
    }
    if (pause)
    {
        periodic = false;
    }

    // The silence ramp counts frames, then the volume comes from the velocity, the region and the track.
    const int counted = static_cast<uint16_t>(silence_counter + frames);
    silence_counter = static_cast<uint16_t>(counted > silence_duration ? silence_duration : counted);
    uint32_t silence = silence_to;
    if (counted < silence_duration)
    {
        const int32_t change = (static_cast<int32_t>(silence_to) - static_cast<int32_t>(silence_from)) * counted /
                               static_cast<int32_t>(silence_duration);
        silence = static_cast<uint8_t>(silence_from + change);
    }
    if (envelope.GetStatus() == CurveAdshr::Status::kRelease && envelope.GetValue() < CurveAdshr::kInitialDecibel)
    {
        Stop();
        return;
    }

    // The pitch: the key, the bend, the sweep and the pitch LFOs. The result is a ratio, kept for the next frame.
    float pitch = bend + static_cast<float>(static_cast<int32_t>(key) - static_cast<int32_t>(original_key));
    pitch = pitch + GetSweepValue();
    for (std::size_t i = 0; i < lfos.size(); ++i)
    {
        if (lfo_types[i] == 0)
        {
            pitch = pitch + lfos[i].GetValue(random);
        }
    }
    float ratio = cached_ratio;
    if (!(pitch == cached_pitch))
    {
        ratio = util::CalcPitchRatio(util::ToInt32(pitch * 256.0f));
        cached_pitch = pitch;
        cached_ratio = ratio;
    }

    // The output parameters, with the note's pan and surround pan, and the pan LFOs.
    OutputParam param = output;
    param.pan = init_pan + output.pan;
    for (std::size_t i = 0; i < lfos.size(); ++i)
    {
        if (lfo_types[i] == 2)
        {
            param.pan = lfos[i].GetValue(random) + param.pan;
        }
    }
    float volume = velocity_volume * velocity_volume;
    volume = volume * init_volume;
    volume = volume * track_volume;
    volume = volume * static_cast<float>(silence);
    volume = volume * kOneOver255;
    param.span = init_span + output.span;

    // Frames of 5 ms pass. LFOs without a type don't step. A note-off that came before the channel's first update takes
    // effect now.
    if (periodic)
    {
        const int msec = frames * 5;
        if (auto_sweep)
        {
            UpdateSweep(msec);
        }
        for (std::size_t i = 0; i < lfos.size(); ++i)
        {
            if (lfo_types[i] != 0xFF)
            {
                lfos[i].Update(msec);
            }
        }
        envelope.Update(msec);
        if (release_requested_)
        {
            Release();
            release_requested_ = false;
        }
    }

    const float lpf_freq = lpf + 1.0f;
    volume = volume * util::CalcVolumeRatio(envelope.GetValue());
    for (std::size_t i = 0; i < lfos.size(); ++i)
    {
        if (lfo_types[i] == 1)
        {
            volume = volume * util::CalcVolumeRatio(lfos[i].GetValue(random) * 6.0f);
        }
    }

    if (voice != nullptr)
    {
        voice->SetVolume(volume);
        voice->SetPitch((ratio * tune) * user_pitch);
        voice->SetPanMode(pan_mode);
        voice->SetPanCurve(pan_curve);
        voice->SetLpfFreq(lpf_freq);
        voice->SetBiquadFilter(biquad_type, biquad_value);
        voice->SetOutputLine(output_line);
        voice->SetOutputParam(param);
    }
    updated_ = true;
}

void Channel::Stop()
{
    if (voice == nullptr)
    {
        return;
    }

    voice->Stop();
    voice->Free();
    voice = nullptr;
    pause = false;
    active = false;
}

void Channel::Release()
{
    if (envelope.GetStatus() != CurveAdshr::Status::kRelease)
    {
        if (voice != nullptr && !release_priority_fix)
        {
            voice->SetPriority(1);
        }
        envelope.SetStatus(CurveAdshr::Status::kRelease);
    }
    pause = false;
}

void Channel::NoteOff()
{
    if (ignore_note_off)
    {
        return;
    }

    if (updated_)
    {
        Release();
    }
    else
    {
        release_requested_ = true;
    }
}

void Channel::CallChannelCallback(CallbackStatus status)
{
    if (callback)
    {
        const Callback owner = callback;
        owner(*this, status);
        callback = nullptr;
    }
}

void Channel::OnVoiceEvent(MultiVoice& dropped, MultiVoice::CallbackStatus status)
{
    switch (status)
    {
    case MultiVoice::CallbackStatus::kFinishWave:
        dropped.Free();
        CallChannelCallback(CallbackStatus::kFinish);
        break;

    case MultiVoice::CallbackStatus::kCancel:
        dropped.Free();
        CallChannelCallback(CallbackStatus::kCancel);
        break;

    case MultiVoice::CallbackStatus::kDropVoice:
    case MultiVoice::CallbackStatus::kDropDsp:
        CallChannelCallback(CallbackStatus::kDrop);
        break;

    default:
        CallChannelCallback(CallbackStatus::kFinish);
        break;
    }
    voice = nullptr;
    pause = false;
    active = false;
    allocated = false;
}

// ---------------------------------------------------------------------------------------------------- ChannelManager

ChannelManager::ChannelManager(MultiVoiceManager& voices, int count)
    : voices_(&voices), pool_(static_cast<std::size_t>(count))
{
    for (int i = 0; i < count; ++i)
    {
        free_.push_back(i);
    }
}

Channel* ChannelManager::AllocChannel(int channels, int32_t priority, Channel::Callback callback)
{
    // A channel from the pool (ChannelManager::Alloc, 0xc242c0), at the end of the list, then its MultiVoice.
    if (free_.empty())
    {
        return nullptr;
    }
    const int index = free_.back();
    free_.pop_back();
    pool_[static_cast<std::size_t>(index)] = std::make_unique<Channel>();
    Channel* channel = pool_[static_cast<std::size_t>(index)].get();
    active_.push_back(channel);
    channel->allocated = true;

    const auto on_voice = [channel](MultiVoice& dropped, MultiVoice::CallbackStatus status)
    {
        channel->OnVoiceEvent(dropped, status);
    };
    MultiVoice* voice = voices_->AllocVoice(channels, priority, on_voice);
    if (voice == nullptr)
    {
        Free(*channel);
        return nullptr;
    }
    channel->voice = voice;
    channel->InitParam(std::move(callback));

    return channel;
}

void ChannelManager::UpdateAllChannel(int frames, Random& random)
{
    for (auto it = active_.begin(); it != active_.end();)
    {
        Channel& channel = **it++;
        channel.Update(true, frames, random);
        if (!channel.active)
        {
            channel.CallChannelCallback(Channel::CallbackStatus::kStopped);
            Free(channel);
        }
    }
}

int ChannelManager::Index(const Channel& channel) const
{
    for (std::size_t i = 0; i < pool_.size(); ++i)
    {
        if (pool_[i].get() == &channel)
        {
            return static_cast<int>(i);
        }
    }

    return -1;
}

bool ChannelManager::AnyChanging() const
{
    const auto changing = [](const Channel* channel)
    {
        return channel->envelope.GetStatus() != CurveAdshr::Status::kSustain;
    };

    return std::any_of(active_.begin(), active_.end(), changing);
}

void ChannelManager::Free(Channel& channel)
{
    const int index = Index(channel);
    if (index < 0)
    {
        return;
    }

    active_.remove(&channel);
    free_.push_back(index);
    pool_[static_cast<std::size_t>(index)].reset();
}

} // namespace hsf::atk
