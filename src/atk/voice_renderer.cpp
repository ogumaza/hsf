// SPDX-License-Identifier: MIT

#include "atk/voice_renderer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/util.h"

namespace hsf::atk
{

VoiceRenderer::VoiceRenderer(LowLevelVoiceAllocator& allocator, const HardwareSettings& hardware, int voice_count)
    : allocator_(&allocator), hardware_(&hardware), buffers_(static_cast<std::size_t>(voice_count))
{
}

int VoiceRenderer::Acquire(uint32_t priority, Voice&)
{
    LowLevelVoice* low = allocator_->AllocVoice();
    if (low == nullptr)
    {
        return -1;
    }

    low->SetPriority(priority);

    return allocator_->PlaceOf(*low);
}

void VoiceRenderer::Free(int renderer)
{
    allocator_->FreeVoice(allocator_->At(renderer));
}

bool VoiceRenderer::IsAvailable(int renderer) const
{
    return allocator_->At(renderer).IsAvailable();
}

void VoiceRenderer::SetPriority(int renderer, uint32_t priority)
{
    allocator_->At(renderer).SetPriority(priority);
}

void VoiceRenderer::SetState(int renderer, const Voice& voice)
{
    LowLevelVoice& low = allocator_->At(renderer);
    const auto state = static_cast<LowLevelVoice::State>(voice.state);
    if (state == LowLevelVoice::State::kStop)
    {
        low.FreeAllWaveBuffer();
    }
    else if (state == LowLevelVoice::State::kPlay)
    {
        low.SetWave(voice.format, voice.sample_rate, voice.coefs, voice.receiver);
    }
    low.SetState(state);
}

void VoiceRenderer::SetParam(int renderer, const VoiceParam& param)
{
    allocator_->At(renderer).Param() = param;
}

void VoiceRenderer::AppendWave(VoiceId place, std::span<Voice> voices, const WaveInfo& wave)
{
    Buffers& buffers = buffers_[static_cast<std::size_t>(place)];
    buffers.last = nullptr;
    const bool adpcm = wave.format == SampleFormat::kDspAdpcm;
    for (std::size_t c = 0; c < voices.size(); ++c)
    {
        const WaveInfo::Channel& data = wave.channels[c];
        if (adpcm)
        {
            buffers.contexts[c] = data.context;
            if (wave.loop)
            {
                buffers.loop_contexts[c] = data.loop_context;
            }

            // MultiVoice::SetAdpcmParam (0xc26490).
            voices[c].coefs = data.coefs;
        }

        WaveBuffer& start = buffers.wave_buffers[c][0];
        start.data = data.data;
        start.sample_offset = 0;
        start.sample_length = wave.loop_end;
        start.loop = false;
        start.adpcm_context = adpcm ? &buffers.contexts[c] : nullptr;
        if (wave.loop)
        {
            WaveBuffer& loop = buffers.wave_buffers[c][1];
            loop.data = data.data;
            loop.sample_offset = wave.loop_start;
            loop.sample_length = wave.loop_end;
            loop.loop = true;
            loop.adpcm_context = adpcm ? &buffers.loop_contexts[c] : nullptr;
            AppendWaveBuffer(buffers, voices[c], start, false);
            AppendWaveBuffer(buffers, voices[c], loop, true);
        }
        else
        {
            AppendWaveBuffer(buffers, voices[c], start, true);
        }
    }
}

void VoiceRenderer::AppendWaveBuffer(Buffers& buffers, const Voice& voice, WaveBuffer& buffer, bool last)
{
    // Voice::AppendWaveBuffer (0xc27d60) queues the buffer on the LowLevelVoice.
    if (voice.renderer >= 0)
    {
        allocator_->At(voice.renderer).AppendWaveBuffer(buffer);
    }
    if (last)
    {
        buffers.last = &buffer;
    }
}

bool VoiceRenderer::IsPlayFinished(VoiceId place) const
{
    const WaveBuffer* last = buffers_[static_cast<std::size_t>(place)].last;
    return last != nullptr && last->status != WaveBuffer::Status::kWait && last->status != WaveBuffer::Status::kPlay;
}

void VoiceRenderer::CalcMix(MultiVoice& voice) const
{
    const std::span<Voice> voices = voice.Voices();
    for (std::size_t i = 0; i < voices.size(); ++i)
    {
        voices[i].param.mix = CalcMixVolumeForBuses(voice, CalcMixVolume(voice, static_cast<int>(i)));
    }
}

void VoiceRenderer::CalcLpf(MultiVoice& voice) const
{
    const uint16_t frequency = util::CalcLpfFreq(voice.LpfFreq());
    for (Voice& each : voice.Voices())
    {
        if (frequency >= 16000)
        {
            each.param.lpf_on = false;
        }
        else
        {
            each.param.lpf_on = true;
            each.param.lpf_freq = frequency;
        }
    }
}

void VoiceRenderer::CalcBiquadFilter(MultiVoice& voice) const
{
    // Voice::SetBiquadFilter (0xc27e20): turning the filter off leaves its coefficients. A value that isn't 0 or less,
    // NaN included, takes the preset's coefficients. The hardware manager registers the presets as types 1 to 10, from
    // 0xc06520 (ntdlib's BiquadPresetCoefficients). A game registers others from type 64. Here those turn the filter
    // off.
    const int16_t* coefs =
        voice.BiquadValue() <= 0.0f
            ? nullptr
            : ntdlib::nwsnd::BiquadPresetCoefficients<ntdlib::nwsnd::NxPolicy>(voice.BiquadType(), voice.BiquadValue());
    for (Voice& each : voice.Voices())
    {
        each.param.biquad_on = coefs != nullptr;
        if (coefs != nullptr)
        {
            std::copy_n(coefs, 5, each.param.biquad.begin());
        }
    }
}

VoiceRenderer::Gains VoiceRenderer::CalcMixVolume(const MultiVoice& voice, int channel) const
{
    const HardwareSettings& hardware = *hardware_;
    const OutputParam& output = voice.GetOutputs();
    const OutputMode mode = hardware.output_mode;
    float front_left = 0.0f;
    float front_right = 0.0f;
    float rear_left = 0.0f;
    float rear_right = 0.0f;
    float front_center = 0.0f;
    float lfe = 0.0f;
    if (output.mix_mode != 0)
    {
        const std::array<float, 6>& mix = output.mix_parameter[static_cast<std::size_t>(channel)];
        front_left = mix[0];
        front_right = mix[1];
        rear_left = mix[2];
        rear_right = mix[3];
        front_center = mix[4];
        lfe = mix[5];
    }
    else
    {
        PanInfo info = PanInfoOf(voice.PanCurve());
        info.surround_curves = hardware.surround_pan_curves;
        float left = 0.0f;
        float right = 0.0f;
        if (mode == OutputMode::kMono)
        {
            left = util::CalcPanRatio(0.0f, info, OutputMode::kMono);
            right = left;
        }
        else if (mode == OutputMode::kStereo || mode == OutputMode::kSurround)
        {
            if (voice.ChannelCount() >= 2 && voice.PanMode() == 1)
            {
                // Balance: each channel keeps its side.
                if (channel == 0)
                {
                    left = util::CalcPanRatio(output.pan, info, mode);
                }
                else if (channel == 1)
                {
                    right = util::CalcPanRatio(0.0f - output.pan, info, mode);
                }
            }
            else
            {
                // Dual: a stereo wave's channels start at the sides and move with the pan.
                float pan = output.pan;
                if (voice.ChannelCount() == 2 && channel == 0)
                {
                    pan = pan + -1.0f;
                }
                else if (voice.ChannelCount() == 2 && channel == 1)
                {
                    pan = pan + 1.0f;
                }
                left = util::CalcPanRatio(pan, info, mode);
                right = util::CalcPanRatio(0.0f - pan, info, mode);
            }
        }

        // Any other mode gets no gains at all.
        float front = 0.0f;
        float rear = 0.0f;
        if (mode == OutputMode::kSurround)
        {
            front = util::CalcSurroundPanRatio(output.span, info);
            rear = util::CalcSurroundPanRatio(2.0f - output.span, info);
        }
        else if (mode == OutputMode::kMono || mode == OutputMode::kStereo)
        {
            front = util::CalcSurroundPanRatio(0.0f, info);
            rear = util::CalcSurroundPanRatio(2.0f, info);
        }
        front_left = left * front;
        front_right = right * front;
        rear_left = left * rear;
        rear_right = right * rear;
    }

    // The mode of the end user's output: a mono output gets the fronts' centre, and a stereo one folds a surround mix's
    // rear channels into the front.
    if (hardware.end_user_output_mode == OutputMode::kMono)
    {
        if (mode == OutputMode::kStereo)
        {
            const float mono = (front_right + front_left) * 0.5f;
            return {mono, mono, rear_left, rear_right, front_center, lfe};
        }
        if (mode == OutputMode::kSurround)
        {
            const float mono = ((rear_right + front_right) + (rear_left + front_left)) * 0.5f;
            return {mono, mono, 0.0f, 0.0f, front_center, lfe};
        }
    }
    else if (hardware.end_user_output_mode == OutputMode::kStereo && mode == OutputMode::kSurround)
    {
        return {rear_left + front_left, rear_right + front_right, 0.0f, 0.0f, front_center, lfe};
    }

    return {front_left, front_right, rear_left, rear_right, front_center, lfe};
}

OutputMix VoiceRenderer::CalcMixVolumeForBuses(const MultiVoice& voice, const Gains& gains) const
{
    OutputMix mix{};
    const OutputReceiver* receiver = voice.GetOutputReceiver();
    if (receiver == nullptr)
    {
        return mix;
    }

    // The devices the voice's output line reaches.
    const HardwareSettings& hardware = *hardware_;
    const OutputParam& output = voice.GetOutputs();
    uint32_t devices = 0;
    for (std::size_t line = 0; line < hardware.output_line_devices.size(); ++line)
    {
        if ((voice.OutputLine() >> line) & 1)
        {
            devices |= hardware.output_line_devices[line];
        }
    }

    // Each bus's send: bus 0 the main send, an offset from 1, and buses 1 to 3 the effect sends. A receiver that clamps
    // a bus's sends holds them to 0 to 1. Otherwise a send is held only below, at 0, and -0 and NaN stay. The sends
    // stay 0 unless the line reaches the console's output.
    const int buses = std::min(receiver->bus_count, static_cast<int>(mix.size()));
    const int channels = receiver->channel_count;
    std::array<float, 24> sends{};
    if ((devices & 1) != 0)
    {
        for (int bus = 0; bus < buses && bus <= 3; ++bus)
        {
            const float value = bus == 0 ? output.main_send + 1.0f : output.fx_send[static_cast<std::size_t>(bus - 1)];
            float& send = sends[static_cast<std::size_t>(bus)];
            if (receiver->clamp_send[static_cast<std::size_t>(bus)])
            {
                send = value > 1.0f ? 1.0f : util::Fmax(value, 0.0f);
            }
            else
            {
                send = value < 0.0f ? 0.0f : value;
            }
        }
    }

    // The gain of a bus's channel is (volume * send) * gain.
    for (int bus = 0; bus < buses; ++bus)
    {
        for (int channel = 0; channel < std::min(channels, 6); ++channel)
        {
            const int index = bus * channels + channel;
            if (index < static_cast<int>(mix.size()))
            {
                mix[static_cast<std::size_t>(index)] =
                    (output.volume * sends[static_cast<std::size_t>(bus)]) * gains[static_cast<std::size_t>(channel)];
            }
        }
    }

    return mix;
}

} // namespace hsf::atk
