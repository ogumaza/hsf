// SPDX-License-Identifier: MIT

#include "atk/low_level_voice.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace hsf::atk
{
namespace
{

// The renderer's priority for an nn::atk priority: 255 minus the priority (SetPriority, 0xc26ea0, and Acquire,
// 0xc26f00). nn::atk's highest priority is then the renderer's highest priority.
int32_t RendererPriority(uint32_t priority)
{
    return priority == 255 ? 0 : 255 - static_cast<int32_t>(priority);
}

// A volume or a gain as the renderer gets it, held to -128 to 128. fminnm holds the value below 128 and gives 128 for a
// NaN.
float ClampVolume(float volume)
{
    const float held = util::Fminnm(volume, 128.0f);
    return volume < -128.0f ? -128.0f : held;
}

// A pitch as the renderer gets it, held to 1/16 to 16. fmax holds the pitch above 1/16 and passes a NaN through.
float ClampPitch(float pitch)
{
    const float held = util::Fmax(pitch, 0.0625f);
    return pitch > 16.0f ? 16.0f : held;
}

// A biquad's coefficients as the renderer takes them.
audio::BiquadFilterParameter Biquad(bool enabled, const std::array<int16_t, 5>& coefficients)
{
    audio::BiquadFilterParameter parameter;
    parameter.enabled = enabled;
    parameter.b = {coefficients[0], coefficients[1], coefficients[2]};
    parameter.a = {coefficients[3], coefficients[4]};

    return parameter;
}

} // namespace

// ----------------------------------------------------------------------------------------------------- LowLevelVoice

void LowLevelVoice::AppendWaveBuffer(WaveBuffer& buffer)
{
    buffer.next = nullptr;
    buffer.status = WaveBuffer::Status::kWait;
    if (tail_ != nullptr)
    {
        tail_->next = &buffer;
    }
    else
    {
        head_ = &buffer;
    }
    tail_ = &buffer;
}

bool LowLevelVoice::Loops() const
{
    for (const WaveBuffer* buffer = head_; buffer != nullptr; buffer = buffer->next)
    {
        if (buffer->loop)
        {
            return true;
        }
    }

    return false;
}

void LowLevelVoice::FreeAllWaveBuffer()
{
    for (WaveBuffer* buffer = head_; buffer != nullptr; buffer = buffer->next)
    {
        buffer->status = WaveBuffer::Status::kDone;
    }
    head_ = nullptr;
    tail_ = nullptr;
    last_ = nullptr;
}

void LowLevelVoice::SetPriority(uint32_t priority)
{
    priority_ = priority;
    if (IsVoiceValid())
    {
        owner_->renderer_->SetVoicePriority(slot_, RendererPriority(priority));
    }
}

void LowLevelVoice::SetState(State state)
{
    state_ = state;
    if (state == State::kStop)
    {
        if (acquired_)
        {
            if (IsVoiceValid())
            {
                owner_->renderer_->ReleaseVoiceSlot(slot_);
            }
            slot_ = -1;
            acquired_ = false;
        }
    }
    else if (state == State::kPlay && !acquired_)
    {
        // The voice counts as acquired even when the renderer had no voice to give.
        Acquire();
        acquired_ = true;
    }
}

void LowLevelVoice::SetWave(SampleFormat format, uint32_t sample_rate, const AdpcmCoefficients& coefficients,
                            const OutputReceiver* receiver)
{
    format_ = format;
    sample_rate_ = sample_rate;
    coefficients_ = coefficients;
    receiver_ = receiver;
}

bool LowLevelVoice::Acquire()
{
    // nn::atk's formats as the renderer's formats: anything but PCM8 and DSP-ADPCM plays as PCM16.
    audio::SampleFormat format = audio::SampleFormat::kPcmInt16;
    const AdpcmCoefficients* coefficients = nullptr;
    if (format_ == SampleFormat::kDspAdpcm)
    {
        format = audio::SampleFormat::kAdpcm;
        coefficients = &coefficients_;
    }
    else if (format_ == SampleFormat::kPcm8)
    {
        format = audio::SampleFormat::kPcmInt8;
    }

    audio::Renderer& renderer = *owner_->renderer_;
    slot_ = renderer.AcquireVoiceSlot(static_cast<int32_t>(sample_rate_), 1, format, RendererPriority(priority_),
                                      coefficients);
    if (slot_ < 0)
    {
        return false;
    }

    if (owner_->hardware_->high_src_quality)
    {
        renderer.SetVoiceSrcQuality(slot_, audio::SrcQuality::kHigh);
    }
    if (receiver_ != nullptr)
    {
        renderer.SetVoiceDestination(slot_, receiver_->mix);
    }

    return true;
}

void LowLevelVoice::UpdateParam(OutputMode mode)
{
    if (!IsVoiceValid() || state_ == State::kStop)
    {
        return;
    }

    audio::Renderer& renderer = *owner_->renderer_;
    if (state_ == State::kPause && renderer.GetVoicePlayState(slot_) == audio::PlayState::kStarted)
    {
        return;
    }

    // Biquad 0 is the sequence's biquad preset, and biquad 1 the low-pass.
    renderer.SetVoiceVolume(slot_, ClampVolume(param_.volume));
    renderer.SetVoicePitch(slot_, ClampPitch(param_.pitch));
    renderer.SetVoiceBiquadFilterParameter(slot_, 0, Biquad(param_.biquad_on, param_.biquad));
    UpdateLowPass();
    UpdateMixVolume(mode);
}

void LowLevelVoice::UpdateLowPass()
{
    // A cutoff from 1 to 15,999 Hz, from the tables for the renderer's rate, or computed for the wave's rate.
    const HardwareSettings& hardware = *owner_->hardware_;
    std::array<int16_t, 5> coefficients{};
    if (static_cast<uint16_t>(param_.lpf_freq - 1) <= 15998)
    {
        coefficients = hardware.exact_low_pass
                           ? util::CalcLowPassBiquad(param_.lpf_freq, static_cast<int32_t>(sample_rate_), false)
                           : util::CalcLowPassBiquad(param_.lpf_freq, hardware.renderer_sample_rate, true);
    }

    owner_->renderer_->SetVoiceBiquadFilterParameter(slot_, 1, Biquad(param_.lpf_on, coefficients));
}

void LowLevelVoice::UpdateMixVolume(OutputMode mode)
{
    if (receiver_ == nullptr)
    {
        return;
    }

    // Every bus of the receiver, or bus 0 alone. Outside surround output, only the front left and right channels get
    // their gains, and the others 0.
    const int buses = owner_->hardware_->all_buses ? receiver_->bus_count : 1;
    const int channels = receiver_->channel_count;
    for (int bus = 0; bus < buses; ++bus)
    {
        for (int channel = 0; channel < channels; ++channel)
        {
            const int index = bus * channels + channel;
            const bool sounds = mode == OutputMode::kSurround || channel <= 1;
            const float volume = sounds && index < static_cast<int>(param_.mix.size())
                                     ? ClampVolume(param_.mix[static_cast<std::size_t>(index)])
                                     : 0.0f;
            owner_->renderer_->SetVoiceMixVolume(slot_, volume, 0, index);
        }
    }
}

void LowLevelVoice::UpdateStatePlay(OutputMode mode)
{
    WaveBuffer* head = head_;
    if (head == nullptr)
    {
        return;
    }

    // A paused voice resumes.
    audio::Renderer& renderer = *owner_->renderer_;
    if (renderer.GetVoicePlayState(slot_) == audio::PlayState::kPaused)
    {
        renderer.SetVoicePlayState(slot_, audio::PlayState::kStarted);
        return;
    }

    // A waiting buffer starts the voice. A buffer that already plays means that the renderer stopped the voice by
    // itself, and the queue is done.
    if (head->status == WaveBuffer::Status::kWait)
    {
        AppendToRenderer(*head);
        last_ = head;
        renderer.SetVoicePlayState(slot_, audio::PlayState::kStarted);
        UpdateMixVolume(mode);
        head->status = WaveBuffer::Status::kPlay;
    }
    else if (head->status == WaveBuffer::Status::kPlay)
    {
        FreeAllWaveBuffer();
    }
}

void LowLevelVoice::UpdateWaveBuffer()
{
    WaveBuffer* head = head_;
    if (head == nullptr)
    {
        return;
    }

    // At most one buffer a frame that the renderer has finished with: the head. Without a next buffer, the renderer's
    // voice stops.
    audio::Renderer& renderer = *owner_->renderer_;
    if (renderer.GetReleasedWaveBuffer(slot_) != nullptr)
    {
        head_ = head->next;
        head->status = WaveBuffer::Status::kDone;
        if (head_ != nullptr)
        {
            head_->status = WaveBuffer::Status::kPlay;
        }
        else
        {
            tail_ = nullptr;
            renderer.SetVoicePlayState(slot_, audio::PlayState::kStopped);
        }
    }

    // The buffer after the last one the renderer has, one a frame.
    WaveBuffer* next = last_ != nullptr ? last_->next : nullptr;
    if (next != nullptr && AppendToRenderer(*next))
    {
        last_ = next;
    }
}

bool LowLevelVoice::AppendToRenderer(WaveBuffer& buffer)
{
    // Every buffer ends a stream. The renderer's played count therefore starts again at each buffer.
    audio::WaveBufferInfo info;
    info.buffer = buffer.data;
    info.start = static_cast<int32_t>(buffer.sample_offset);
    info.end = static_cast<int32_t>(buffer.sample_length);
    info.loop = buffer.loop;
    info.end_of_stream = true;
    info.context = format_ == SampleFormat::kDspAdpcm ? buffer.adpcm_context : nullptr;
    info.tag = &buffer;

    return owner_->renderer_->AppendWaveBuffer(slot_, info);
}

// -------------------------------------------------------------------------------------------- LowLevelVoiceAllocator

LowLevelVoiceAllocator::LowLevelVoiceAllocator(audio::Renderer& renderer, const HardwareSettings& hardware, int count)
    : renderer_(&renderer),
      hardware_(&hardware),
      voices_(static_cast<std::size_t>(count)),
      places_(static_cast<std::size_t>(count), -1)
{
    for (LowLevelVoice& voice : voices_)
    {
        voice.owner_ = this;
        list_.push_back(&voice);
    }
}

LowLevelVoice* LowLevelVoiceAllocator::AllocVoice()
{
    if (count_ >= static_cast<int>(list_.size()))
    {
        return nullptr;
    }

    LowLevelVoice* voice = list_[static_cast<std::size_t>(count_)];
    places_[static_cast<std::size_t>(voice - voices_.data())] = count_;
    ++count_;
    voice->play_position_ = 0;
    voice->receiver_ = nullptr;
    voice->allocated_ = true;
    voice->priority_ = 0;
    voice->sample_rate_ = 32000;
    voice->format_ = SampleFormat::kPcm16;

    return voice;
}

void LowLevelVoiceAllocator::FreeVoice(LowLevelVoice& voice)
{
    voice.FreeAllWaveBuffer();
    if (voice.acquired_)
    {
        if (voice.IsVoiceValid())
        {
            renderer_->ReleaseVoiceSlot(voice.slot_);
        }
        voice.slot_ = -1;
        voice.acquired_ = false;
    }
    voice.allocated_ = false;
    voice.state_ = LowLevelVoice::State::kStop;

    // The last allocated voice takes the freed one's place.
    const std::size_t index = static_cast<std::size_t>(&voice - voices_.data());
    const int place = places_[index];
    if (place < 0)
    {
        return;
    }

    --count_;
    LowLevelVoice* last = list_[static_cast<std::size_t>(count_)];
    list_[static_cast<std::size_t>(place)] = last;
    list_[static_cast<std::size_t>(count_)] = &voice;
    places_[static_cast<std::size_t>(last - voices_.data())] = place;
    places_[index] = -1;
}

void LowLevelVoiceAllocator::UpdateAllVoiceStatus()
{
    for (int i = 0; i < count_; ++i)
    {
        // A voice the renderer dropped is lost to its owner.
        LowLevelVoice& voice = *list_[static_cast<std::size_t>(i)];
        if (voice.IsVoiceValid() && renderer_->IsVoiceDroppedFlagOn(voice.slot_) && voice.allocated_)
        {
            voice.allocated_ = false;
            continue;
        }
        if (!voice.IsVoiceValid() || voice.state_ != LowLevelVoice::State::kPlay ||
            renderer_->GetVoicePlayState(voice.slot_) != audio::PlayState::kStarted)
        {
            continue;
        }

        // The renderer counts from the start of the buffer that plays. A buffer that loops counts on past its end.
        voice.UpdateWaveBuffer();
        if (voice.acquired_)
        {
            int64_t played = static_cast<int64_t>(renderer_->GetVoicePlayedSampleCount(voice.slot_));
            int64_t start = 0;
            if (const WaveBuffer* head = voice.head_)
            {
                start = head->sample_offset;
                const int64_t length = head->sample_length - head->sample_offset;
                if (head->loop && played > length && length != 0)
                {
                    played %= length;
                }
            }
            voice.play_position_ = played + start;
        }
    }
}

void LowLevelVoiceAllocator::UpdateAllVoice(OutputMode mode)
{
    for (int i = 0; i < count_; ++i)
    {
        LowLevelVoice& voice = *list_[static_cast<std::size_t>(i)];
        voice.UpdateParam(mode);
        if (!voice.IsVoiceValid())
        {
            continue;
        }

        // The renderer's voice takes the state the voice should be in.
        const bool started = renderer_->GetVoicePlayState(voice.slot_) == audio::PlayState::kStarted;
        switch (voice.state_)
        {
        case LowLevelVoice::State::kPlay:
            if (!started)
            {
                voice.UpdateStatePlay(mode);
            }
            break;

        case LowLevelVoice::State::kStop:
            if (started)
            {
                renderer_->SetVoicePlayState(voice.slot_, audio::PlayState::kStopped);
            }
            break;

        case LowLevelVoice::State::kPause:
            if (started)
            {
                renderer_->SetVoicePlayState(voice.slot_, audio::PlayState::kPaused);
            }
            break;
        }
    }
}

} // namespace hsf::atk
