// SPDX-License-Identifier: MIT

#include "atk/voice.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <utility>

#include "atk/tables.h"

namespace hsf::atk
{
namespace
{

// A pan curve's PanInfo (the switch in CalcMixVolume, 0xc259f0, with its jump table at 0x1affb39). Curves 0 to 8 are
// the square root, sine and linear curves in turn. Each curve comes plain, then centred at unity, then centred and held
// to unity. Anything past curve 8 is the plain square root.
PanInfo PanInfoOf(uint32_t pan_curve)
{
    static constexpr std::array<PanInfo, 9> kCurves = {{
        {0, false, false, false, false},
        {0, true, false, false, false},
        {0, true, true, false, false},
        {1, false, false, false, false},
        {1, true, false, false, false},
        {1, true, true, false, false},
        {2, false, false, false, false},
        {2, true, false, false, false},
        {2, true, true, false, false},
    }};

    return pan_curve < kCurves.size() ? kCurves[pan_curve] : kCurves[0];
}

// A biquad preset's coefficients for a value above 0 (GetCoefficients of the presets that the hardware manager
// registers, from 0xc06520): the table's row for the value, held to the table. The band-passes take the value as (2 -
// v)v. Types 1 to 5 are the Wii U's presets, made for its 32 kHz renderer, and types 6 to 10 the same made for 48 kHz.
// A game registers others from type 64. Here those turn the filter off.
bool BiquadPreset(int type, float value, std::array<int16_t, 5>& coefs)
{
    const auto row = [](float scaled, int last)
    {
        const int32_t index = util::ToInt32(scaled);
        return index < 0 ? 0 : (index < last ? index : last);
    };
    const auto take = [&coefs](const int16_t* entry)
    {
        std::copy(entry, entry + 5, coefs.begin());
    };

    const float band = (2.0f - value) * value;
    switch (type)
    {
    case 1:
        take(tables::kBiquadLpf[row(value * 111.0f, 111)]);
        return true;
    case 2:
        take(tables::kBiquadHpf[row(value * 96.0f, 96)]);
        return true;
    case 3:
        take(tables::kBiquadBpf512[row(band * 121.0f, 121)]);
        return true;
    case 4:
        take(tables::kBiquadBpf1024[row(band * 92.0f, 92)]);
        return true;
    case 5:
        take(tables::kBiquadBpf2048[row(band * 92.0f, 92)]);
        return true;
    case 6:
        take(tables::kBiquadLpf48k[row(value * 111.0f, 111)]);
        return true;
    case 7:
        take(tables::kBiquadHpf48k[row(value * 96.0f, 96)]);
        return true;
    case 8:
        take(tables::kBiquadBpf512_48k[row(band * 121.0f, 121)]);
        return true;
    case 9:
        take(tables::kBiquadBpf1024_48k[row(band * 92.0f, 92)]);
        return true;
    case 10:
        take(tables::kBiquadBpf2048_48k[row(band * 92.0f, 92)]);
        return true;
    default:
        return false;
    }
}

} // namespace

// ------------------------------------------------------------------------------------------------------- MultiVoice

void MultiVoice::Start()
{
    started_ = true;
    sync_ |= kSyncStart;
    pause_ = false;
}

void MultiVoice::Stop()
{
    if (sdk_started_)
    {
        for (int i = 0; i < channel_count_; ++i)
        {
            SetVoiceState(voices_[static_cast<std::size_t>(i)], LowLevelVoice::State::kStop);
        }
        sdk_started_ = false;
    }
    pause_ = false;
    paused_ = false;
    started_ = false;
}

void MultiVoice::Free()
{
    if (!active_)
    {
        return;
    }

    for (int i = 0; i < channel_count_; ++i)
    {
        FreeVoice(voices_[static_cast<std::size_t>(i)]);
    }
    channel_count_ = 0;
    manager_->RemoveVoiceList(*this);
    active_ = false;
}

void MultiVoice::SetSampleFormat(SampleFormat format)
{
    format_ = format;
    for (int i = 0; i < channel_count_; ++i)
    {
        voices_[static_cast<std::size_t>(i)].format = format;
    }
}

void MultiVoice::SetSampleRate(uint32_t rate)
{
    for (int i = 0; i < channel_count_; ++i)
    {
        voices_[static_cast<std::size_t>(i)].sample_rate = rate;
    }
}

void MultiVoice::SetAdpcmParam(int channel, const AdpcmCoefficients& coefs)
{
    voices_[static_cast<std::size_t>(channel)].coefs = coefs;
}

void MultiVoice::SetOutputReceiver(const OutputReceiver* receiver)
{
    if (channel_count_ < 1)
    {
        return;
    }

    receiver_ = receiver;
    for (int i = 0; i < channel_count_; ++i)
    {
        voices_[static_cast<std::size_t>(i)].receiver = receiver;
    }
}

void MultiVoice::AppendWaveBuffer(int channel, WaveBuffer& buffer, bool last)
{
    // Voice::AppendWaveBuffer (0xc27d60) queues the buffer on the LowLevelVoice, if the Voice still holds one.
    if (LowLevelVoice* low = voices_[static_cast<std::size_t>(channel)].low)
    {
        low->AppendWaveBuffer(buffer);
    }
    if (last)
    {
        last_buffer_ = &buffer;
    }
}

void MultiVoice::SetInterpolationType(uint8_t type)
{
    for (int i = 0; i < channel_count_; ++i)
    {
        voices_[static_cast<std::size_t>(i)].interpolation = type;
    }
}

void MultiVoice::SetVolume(float volume)
{
    volume = util::Fmax(volume, 0.0f);
    if (!(volume == volume_))
    {
        volume_ = volume;
        sync_ |= kSyncVe;
    }
}

void MultiVoice::SetPitch(float pitch)
{
    if (!(pitch == pitch_))
    {
        pitch_ = pitch;
        sync_ |= kSyncSrc;
    }
}

void MultiVoice::SetPanMode(uint32_t mode)
{
    if (mode != pan_mode_)
    {
        pan_mode_ = mode;
        sync_ |= kSyncMix;
    }
}

void MultiVoice::SetPanCurve(uint32_t curve)
{
    if (curve != pan_curve_)
    {
        pan_curve_ = curve;
        sync_ |= kSyncMix;
    }
}

void MultiVoice::SetLpfFreq(float lpf)
{
    if (!(lpf == lpf_freq_))
    {
        lpf_freq_ = lpf;
        sync_ |= kSyncLpf;
    }
}

void MultiVoice::SetBiquadFilter(int type, float value)
{
    value = value > 1.0f ? 1.0f : util::Fmax(value, 0.0f);
    const bool type_changed = static_cast<uint32_t>(type) != biquad_type_;
    if (type_changed)
    {
        biquad_type_ = static_cast<uint8_t>(type);
    }
    if (!(value == biquad_value_))
    {
        biquad_value_ = value;
        sync_ |= kSyncBiquad;
    }
    else if (type_changed)
    {
        sync_ |= kSyncBiquad;
    }
}

void MultiVoice::SetPriority(uint32_t priority)
{
    priority_ = priority;
    manager_->ChangeVoicePriority(*this);
    for (int i = 0; i < channel_count_; ++i)
    {
        // Voice::SetPriority (0xc27cb0).
        ChannelVoice& voice = voices_[static_cast<std::size_t>(i)];
        voice.priority = priority_;
        if (voice.low != nullptr)
        {
            voice.low->SetPriority(priority_);
        }
    }
}

void MultiVoice::SetOutputLine(uint32_t line)
{
    if (line != output_line_)
    {
        output_line_ = line;
        sync_ |= kSyncMix;
    }
}

void MultiVoice::SetOutputParam(const OutputParam& param)
{
    // SetOutputParamImpl (0xc25230): a change of what the mix depends on marks the mix.
    const auto take = [this](float value, float& kept)
    {
        if (!(value == kept))
        {
            kept = value;
            sync_ |= kSyncMix;
        }
    };

    take(util::Fmax(param.volume, 0.0f), output_.volume);
    if (param.mix_mode != output_.mix_mode)
    {
        output_.mix_mode = param.mix_mode;
        sync_ |= kSyncMix;
    }

    // Of the pan and the mix parameters, only what the mode mixes by.
    if (output_.mix_mode == 0)
    {
        take(param.pan, output_.pan);
        take(param.span, output_.span);
    }
    else if (output_.mix_mode == 1)
    {
        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            for (std::size_t i = 0; i < 6; ++i)
            {
                take(param.mix_parameter[channel][i], output_.mix_parameter[channel][i]);
            }
        }
    }

    take(param.main_send, output_.main_send);
    for (std::size_t bus = 0; bus < 3; ++bus)
    {
        take(param.fx_send[bus], output_.fx_send[bus]);
    }
}

bool MultiVoice::Alloc(int channels, int32_t priority, Callback callback)
{
    const int count = std::min(std::max(channels, 1), 2);
    channel_count_ = 0;
    int allocated = 0;
    for (; allocated < count; ++allocated)
    {
        if (!AllocVoice(voices_[static_cast<std::size_t>(allocated)], priority))
        {
            break;
        }
    }
    if (allocated != count)
    {
        for (int i = 0; i < allocated; ++i)
        {
            FreeVoice(voices_[static_cast<std::size_t>(i)]);
        }
        return false;
    }

    // The parameters start again. The priority, the output line, the mix parameters and the receiver keep what they
    // had.
    channel_count_ = count;
    sync_ = 0;
    biquad_type_ = 0;
    last_buffer_ = nullptr;
    started_ = false;
    sdk_started_ = false;
    pause_ = false;
    paused_ = false;
    callback_ = std::move(callback);
    volume_ = 1.0f;
    pitch_ = 1.0f;
    pan_mode_ = 0;
    pan_curve_ = 0;
    lpf_freq_ = 1.0f;
    biquad_value_ = 0.0f;
    output_.volume = 1.0f;
    output_.mix_mode = 0;
    output_.pan = 0.0f;
    output_.span = 0.0f;
    output_.main_send = 0.0f;
    output_.fx_send = {};
    active_ = true;

    return true;
}

void MultiVoice::Calc()
{
    if (!started_)
    {
        return;
    }

    if (sync_ & kSyncSrc)
    {
        CalcSrc();
        sync_ = static_cast<uint16_t>(sync_ & ~kSyncSrc);
    }
    if (sync_ & kSyncVe)
    {
        CalcVe();
        sync_ = static_cast<uint16_t>(sync_ & ~kSyncVe);
    }
    if (sync_ & kSyncMix)
    {
        CalcMix();
        sync_ = static_cast<uint16_t>(sync_ & ~kSyncMix);
    }
    if (sync_ & kSyncLpf)
    {
        CalcLpf();
        sync_ = static_cast<uint16_t>(sync_ & ~kSyncLpf);
    }
    if (sync_ & kSyncBiquad)
    {
        UpdateBiquadFilter();
        sync_ = static_cast<uint16_t>(sync_ & ~kSyncBiquad);
    }
}

void MultiVoice::Update()
{
    if (!active_)
    {
        return;
    }

    // The first update after a start works out the pitch, the mix and the volume, and starts the voices. The volume's
    // mark stays.
    enum class Action
    {
        kNone,
        kRun,
        kPause,
    };
    Action action = Action::kNone;
    if ((sync_ & kSyncStart) && started_ && !sdk_started_)
    {
        CalcSrc();
        CalcMix();
        CalcVe();
        sdk_started_ = true;
        sync_ = static_cast<uint16_t>(sync_ & ~(kSyncStart | kSyncSrc | kSyncMix));
        action = Action::kRun;
    }
    if (sdk_started_ && (sync_ & kSyncPause) && started_)
    {
        paused_ = pause_;
        sync_ = static_cast<uint16_t>(sync_ & ~kSyncPause);
        action = pause_ ? Action::kPause : Action::kRun;
    }

    // Voice::UpdateParam (0xc27d70): each voice's parameters reach its LowLevelVoice.
    for (int i = 0; i < channel_count_; ++i)
    {
        const ChannelVoice& voice = voices_[static_cast<std::size_t>(i)];
        if (voice.low != nullptr)
        {
            voice.low->Param() = voice.param;
        }
    }

    if (action != Action::kNone)
    {
        const LowLevelVoice::State state =
            action == Action::kRun ? LowLevelVoice::State::kPlay : LowLevelVoice::State::kPause;
        for (int i = 0; i < channel_count_; ++i)
        {
            SetVoiceState(voices_[static_cast<std::size_t>(i)], state);
        }
    }
}

void MultiVoice::UpdateVoiceStatus()
{
    if (!active_ || !sdk_started_ || channel_count_ < 1)
    {
        return;
    }

    // The wave ends once its last buffer is done.
    if (last_buffer_ != nullptr && last_buffer_->status != WaveBuffer::Status::kWait &&
        last_buffer_->status != WaveBuffer::Status::kPlay)
    {
        if (callback_)
        {
            const Callback callback = callback_;
            callback(*this, CallbackStatus::kFinishWave);
        }
        started_ = false;
        sdk_started_ = false;
        return;
    }

    // The renderer may have dropped a voice for one of higher priority.
    bool available = true;
    for (int i = 0; i < channel_count_; ++i)
    {
        if (!IsAvailable(voices_[static_cast<std::size_t>(i)]))
        {
            available = false;
            break;
        }
    }
    if (available)
    {
        return;
    }

    for (int i = 0; i < channel_count_; ++i)
    {
        FreeVoice(voices_[static_cast<std::size_t>(i)]);
    }
    channel_count_ = 0;
    active_ = false;
    manager_->RemoveVoiceList(*this);
    if (callback_)
    {
        const Callback callback = callback_;
        callback(*this, CallbackStatus::kDropDsp);
    }
    started_ = false;
    sdk_started_ = false;
}

void MultiVoice::CalcSrc()
{
    for (int i = 0; i < channel_count_; ++i)
    {
        voices_[static_cast<std::size_t>(i)].param.pitch = pitch_;
    }
}

void MultiVoice::CalcVe()
{
    const float volume = volume_ * manager_->hardware_->output_volume;
    for (int i = 0; i < channel_count_; ++i)
    {
        voices_[static_cast<std::size_t>(i)].param.volume = volume;
    }
}

void MultiVoice::CalcMix()
{
    for (int i = 0; i < channel_count_; ++i)
    {
        voices_[static_cast<std::size_t>(i)].param.mix = CalcMixVolumeForBuses(CalcMixVolume(i));
    }
}

void MultiVoice::CalcLpf()
{
    const uint16_t frequency = util::CalcLpfFreq(lpf_freq_);
    for (int i = 0; i < channel_count_; ++i)
    {
        // Voice::SetMonoFilter (0xc27e00): turning the filter off leaves its cutoff.
        VoiceParam& param = voices_[static_cast<std::size_t>(i)].param;
        if (frequency >= 16000)
        {
            param.lpf_on = false;
        }
        else
        {
            param.lpf_on = true;
            param.lpf_freq = frequency;
        }
    }
}

void MultiVoice::UpdateBiquadFilter()
{
    for (int i = 0; i < channel_count_; ++i)
    {
        // Voice::SetBiquadFilter (0xc27e20): turning the filter off leaves its coefficients. A value that isn't 0 or
        // less, NaN included, takes the preset's coefficients.
        VoiceParam& param = voices_[static_cast<std::size_t>(i)].param;
        std::array<int16_t, 5> coefs{};
        if (!(biquad_value_ <= 0.0f) && BiquadPreset(biquad_type_, biquad_value_, coefs))
        {
            param.biquad_on = true;
            param.biquad = coefs;
        }
        else
        {
            param.biquad_on = false;
        }
    }
}

MultiVoice::Gains MultiVoice::CalcMixVolume(int channel) const
{
    const HardwareSettings& hardware = *manager_->hardware_;
    const OutputMode mode = hardware.output_mode;
    float front_left = 0.0f;
    float front_right = 0.0f;
    float rear_left = 0.0f;
    float rear_right = 0.0f;
    float front_center = 0.0f;
    float lfe = 0.0f;
    if (output_.mix_mode != 0)
    {
        const std::array<float, 6>& mix = output_.mix_parameter[static_cast<std::size_t>(channel)];
        front_left = mix[0];
        front_right = mix[1];
        rear_left = mix[2];
        rear_right = mix[3];
        front_center = mix[4];
        lfe = mix[5];
    }
    else
    {
        PanInfo info = PanInfoOf(pan_curve_);
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
            if (channel_count_ >= 2 && pan_mode_ == 1)
            {
                // Balance: each channel keeps its side.
                if (channel == 0)
                {
                    left = util::CalcPanRatio(output_.pan, info, mode);
                }
                else if (channel == 1)
                {
                    right = util::CalcPanRatio(0.0f - output_.pan, info, mode);
                }
            }
            else
            {
                // Dual: a stereo wave's channels start at the sides and move with the pan.
                float pan = output_.pan;
                if (channel_count_ == 2 && channel == 0)
                {
                    pan = pan + -1.0f;
                }
                else if (channel_count_ == 2 && channel == 1)
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
            front = util::CalcSurroundPanRatio(output_.span, info);
            rear = util::CalcSurroundPanRatio(2.0f - output_.span, info);
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

OutputMix MultiVoice::CalcMixVolumeForBuses(const Gains& gains) const
{
    OutputMix mix{};
    if (receiver_ == nullptr)
    {
        return mix;
    }

    // The devices the voice's output line reaches.
    const HardwareSettings& hardware = *manager_->hardware_;
    uint32_t devices = 0;
    for (std::size_t line = 0; line < hardware.output_line_devices.size(); ++line)
    {
        if ((output_line_ >> line) & 1)
        {
            devices |= hardware.output_line_devices[line];
        }
    }

    // Each bus's send: bus 0 the main send, an offset from 1, and buses 1 to 3 the effect sends. A receiver that clamps
    // a bus's sends holds them to 0 to 1. Otherwise a send is held only below, at 0, and -0 and NaN stay. The sends
    // stay 0 unless the line reaches the console's output.
    const int buses = std::min(receiver_->bus_count, static_cast<int>(mix.size()));
    const int channels = receiver_->channel_count;
    std::array<float, 24> sends{};
    if ((devices & 1) != 0)
    {
        for (int bus = 0; bus < buses && bus <= 3; ++bus)
        {
            const float value =
                bus == 0 ? output_.main_send + 1.0f : output_.fx_send[static_cast<std::size_t>(bus - 1)];
            float& send = sends[static_cast<std::size_t>(bus)];
            if (receiver_->clamp_send[static_cast<std::size_t>(bus)])
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
                    (output_.volume * sends[static_cast<std::size_t>(bus)]) * gains[static_cast<std::size_t>(channel)];
            }
        }
    }

    return mix;
}

bool MultiVoice::AllocVoice(ChannelVoice& voice, int32_t priority)
{
    // A LowLevelVoice, and the Voice's defaults: stopped, at volume and pitch 1, with gains of 1 to the main bus's
    // front left and right, no filters, and PCM16 at 32 kHz.
    LowLevelVoice* low = manager_->allocator_->AllocVoice();
    if (low == nullptr)
    {
        return false;
    }

    low->SetPriority(static_cast<uint32_t>(priority));
    voice.low = low;
    voice.priority = static_cast<uint32_t>(priority);
    voice.state = LowLevelVoice::State::kStop;
    voice.param = VoiceParam{};
    voice.param.mix[0] = 1.0f;
    voice.param.mix[1] = 1.0f;
    voice.interpolation = 0;
    voice.format = SampleFormat::kPcm16;
    voice.sample_rate = 32000;
    voice.receiver = nullptr;

    return true;
}

void MultiVoice::FreeVoice(ChannelVoice& voice)
{
    voice.state = LowLevelVoice::State::kStop;
    if (voice.low != nullptr)
    {
        manager_->allocator_->FreeVoice(*voice.low);
        voice.low = nullptr;
    }
}

void MultiVoice::SetVoiceState(ChannelVoice& voice, LowLevelVoice::State state)
{
    // Stopping first drops the queued buffers. Playing gives the LowLevelVoice the wave's format, rate and
    // coefficients, and the receiver.
    voice.state = state;
    LowLevelVoice* low = voice.low;
    if (low == nullptr)
    {
        return;
    }

    if (state == LowLevelVoice::State::kStop)
    {
        low->FreeAllWaveBuffer();
    }
    else if (state == LowLevelVoice::State::kPlay)
    {
        low->SetWave(voice.format, voice.sample_rate, voice.coefs, voice.receiver);
    }
    low->SetState(state);
}

bool MultiVoice::IsAvailable(const ChannelVoice& voice)
{
    return voice.low != nullptr && voice.low->IsAvailable();
}

// ----------------------------------------------------------------------------------------------- MultiVoiceManager

MultiVoiceManager::MultiVoiceManager(LowLevelVoiceAllocator& allocator, const HardwareSettings& hardware, int count)
    : allocator_(&allocator), hardware_(&hardware)
{
    for (int i = 0; i < count; ++i)
    {
        voices_.push_back(std::make_unique<MultiVoice>());
        voices_.back()->manager_ = this;
        free_.push_back(voices_.back().get());
    }
}

MultiVoice* MultiVoiceManager::AllocVoice(int channels, int32_t priority, MultiVoice::Callback callback)
{
    // A free MultiVoice, after dropping voices if there's none. If the MultiVoice can't get the LowLevelVoices that its
    // channels require, voices are dropped once more, and the allocation runs again.
    if (free_.empty() && !DropLowestPriorityVoices(channels, priority))
    {
        return nullptr;
    }
    if (free_.empty())
    {
        return nullptr;
    }

    MultiVoice* voice = free_.front();
    if (!voice->Alloc(channels, priority, callback) &&
        (!DropLowestPriorityVoices(channels, priority) || !voice->Alloc(channels, priority, callback)))
    {
        return nullptr;
    }

    voice->priority_ = static_cast<uint32_t>(priority) & 0xFF;
    free_.remove(voice);
    AppendVoiceList(*voice);

    return voice;
}

bool MultiVoiceManager::DropLowestPriorityVoices(int channels, int32_t priority)
{
    if (active_.empty() || channels < 1)
    {
        return false;
    }

    // The voices of the lowest priority, taken in order until they have the channels, must all be at most `priority`
    // and below 255.
    int found = 0;
    for (auto it = active_.begin(); it != active_.end() && found < channels; ++it)
    {
        const int32_t other = static_cast<int32_t>((*it)->priority_);
        if (other == 255 || other > priority)
        {
            return false;
        }
        found += (*it)->channel_count_;
    }
    if (found < channels)
    {
        return false;
    }

    int dropped = 0;
    while (dropped < channels && !active_.empty())
    {
        MultiVoice& voice = *active_.front();
        dropped += voice.channel_count_;
        const MultiVoice::Callback callback = voice.callback_;
        voice.Stop();
        voice.Free();
        if (callback)
        {
            callback(voice, MultiVoice::CallbackStatus::kDropVoice);
        }
    }

    return dropped != 0;
}

void MultiVoiceManager::UpdateAllVoiceStatus()
{
    for (auto it = active_.begin(); it != active_.end();)
    {
        MultiVoice* voice = *it++;
        voice->UpdateVoiceStatus();
    }
}

void MultiVoiceManager::UpdateAllVoices()
{
    for (auto it = active_.begin(); it != active_.end();)
    {
        MultiVoice* voice = *it++;
        voice->Calc();
    }
    for (auto it = active_.begin(); it != active_.end();)
    {
        MultiVoice* voice = *it++;
        voice->Update();
    }
}

void MultiVoiceManager::AppendVoiceList(MultiVoice& voice)
{
    // After every voice of a lower or equal priority.
    auto it = active_.end();
    while (it != active_.begin())
    {
        const auto previous = std::prev(it);
        if (static_cast<int32_t>((*previous)->priority_) <= static_cast<int32_t>(voice.priority_))
        {
            break;
        }
        it = previous;
    }
    active_.insert(it, &voice);
}

void MultiVoiceManager::RemoveVoiceList(MultiVoice& voice)
{
    active_.remove(&voice);
    free_.push_back(&voice);
}

void MultiVoiceManager::ChangeVoicePriority(MultiVoice& voice)
{
    active_.remove(&voice);
    AppendVoiceList(voice);
}

} // namespace hsf::atk
