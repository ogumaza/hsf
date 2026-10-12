// SPDX-License-Identifier: MIT

#include "audio/renderer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <utility>

namespace hsf::audio
{
namespace
{

// The precision of volumes and mix volumes from REV7 on. This client requests REV8.
constexpr int kGainBits = 23;

// The depop's decay a sample, in Q15, for a mix at 48 kHz and at any other rate.
constexpr int32_t kDepopDecay48k = 31529;
constexpr int32_t kDepopDecay = 30923;

// A float in fixed point with `bits` fraction bits, towards zero and held to 32 bits (fcvtzs).
int32_t ToFixed(float x, int bits)
{
    const float scaled = x * static_cast<float>(1 << bits);
    int32_t value = 0;
    ConvertToInt(&value, &scaled, 1);

    return value;
}

int32_t WrapSub(int32_t a, int32_t b)
{
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}

int32_t WrapAdd(int32_t a, int32_t b)
{
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

// The step of a ramp across a frame: the difference of the gains, which wraps, over the sample count.
int32_t RampStep(int32_t from, int32_t to, int count)
{
    return WrapSub(to, from) / count;
}

// The bytes a DSP-ADPCM range up to `sample` takes, frames of 8 bytes for 14 samples.
int64_t AdpcmBytes(int32_t sample)
{
    const int64_t frame = sample / 14;
    const int64_t in_frame = sample % 14;
    return 8 * frame + (in_frame == 0 ? 0 : in_frame / 2 + 1 + in_frame % 2);
}

} // namespace

Renderer::Renderer(const RendererParameter& parameter)
    : parameter_(parameter),
      voices_(static_cast<std::size_t>(parameter.voice_count)),
      resources_(static_cast<std::size_t>(parameter.voice_count)),
      mixes_(static_cast<std::size_t>(parameter.sub_mix_count) + 1),
      buffers_(static_cast<std::size_t>(parameter.mix_buffer_count + kMaxChannels) *
               static_cast<std::size_t>(parameter.sample_count)),
      depop_(static_cast<std::size_t>(parameter.mix_buffer_count))
{
    mixes_[kFinalMix].sample_rate = parameter.sample_rate;
}

void Renderer::SetObserver(RendererObserver* observer)
{
    observer_ = observer;
    if (observer_ != nullptr)
    {
        observer_->OpenAudioRenderer(parameter_);
    }
}

Renderer::Voice& Renderer::At(int voice)
{
    return voices_.at(static_cast<std::size_t>(voice));
}

const Renderer::Voice& Renderer::At(int voice) const
{
    return voices_.at(static_cast<std::size_t>(voice));
}

// The client keeps its voices in use by priority, 0 first, a new voice before the others of its priority.
void Renderer::Insert(int voice)
{
    const int32_t priority = At(voice).priority;
    const auto at =
        std::find_if(order_.begin(), order_.end(), [&](int other) { return At(other).priority >= priority; });
    order_.insert(at, voice);
}

void Renderer::Remove(int voice)
{
    order_.erase(std::remove(order_.begin(), order_.end(), voice), order_.end());
}

int Renderer::AcquireVoiceSlot(int32_t sample_rate, int channel_count, SampleFormat format, int32_t priority,
                               const AdpcmCoefficients* coefficients)
{
    const int voice = TakeVoiceSlot(sample_rate, channel_count, format, priority, coefficients);
    if (observer_ != nullptr)
    {
        observer_->AcquireVoiceSlot(sample_rate, channel_count, format, priority, coefficients, voice);
    }

    return voice;
}

int Renderer::TakeVoiceSlot(int32_t sample_rate, int channel_count, SampleFormat format, int32_t priority,
                            const AdpcmCoefficients* coefficients)
{
    // A free slot, and the first free channel resources, one for each channel.
    const auto slot = std::find_if(voices_.begin(), voices_.end(), [](const Voice& v) { return !v.in_use; });
    std::vector<int> free;
    for (std::size_t r = 0; r < resources_.size() && static_cast<int>(free.size()) < channel_count; ++r)
    {
        if (!resources_[r].in_use)
        {
            free.push_back(static_cast<int>(r));
        }
    }
    if (slot == voices_.end() || channel_count < 1 || channel_count > kMaxChannels ||
        static_cast<int>(free.size()) < channel_count)
    {
        return -1;
    }

    Voice& voice = *slot;
    voice = Voice{};
    voice.in_use = true;
    voice.is_new = true;
    voice.format = format;
    voice.sample_rate = sample_rate;
    voice.priority = priority;
    voice.channel_count = channel_count;
    if (coefficients != nullptr)
    {
        voice.coefficients = *coefficients;
    }
    for (int c = 0; c < channel_count; ++c)
    {
        ChannelResource& resource = resources_[static_cast<std::size_t>(free[static_cast<std::size_t>(c)])];
        resource.in_use = true;
        resource.client_mix = {};
        voice.resources[static_cast<std::size_t>(c)] = free[static_cast<std::size_t>(c)];
    }

    const int index = static_cast<int>(slot - voices_.begin());
    Insert(index);

    return index;
}

void Renderer::ReleaseVoiceSlot(int voice)
{
    Voice& v = At(voice);
    for (int c = 0; c < v.channel_count; ++c)
    {
        ChannelResource& resource = resources_[static_cast<std::size_t>(v.resources[static_cast<std::size_t>(c)])];
        resource.in_use = false;
        resource.client_mix = {};
    }
    v.in_use = false;
    Remove(voice);
    if (observer_ != nullptr)
    {
        observer_->ReleaseVoiceSlot(voice);
    }
}

void Renderer::SetVoicePlayState(int voice, PlayState state)
{
    At(voice).play_state = state;
    if (observer_ != nullptr)
    {
        observer_->SetVoicePlayState(voice, state);
    }
}

PlayState Renderer::GetVoicePlayState(int voice) const
{
    const PlayState state = At(voice).play_state;
    if (observer_ != nullptr)
    {
        observer_->GetVoicePlayState(voice, state);
    }

    return state;
}

void Renderer::SetVoicePitch(int voice, float pitch)
{
    At(voice).pitch = pitch;
    if (observer_ != nullptr)
    {
        observer_->SetVoicePitch(voice, pitch);
    }
}

void Renderer::SetVoiceVolume(int voice, float volume)
{
    At(voice).volume = volume;
    if (observer_ != nullptr)
    {
        observer_->SetVoiceVolume(voice, volume);
    }
}

void Renderer::SetVoicePriority(int voice, int32_t priority)
{
    At(voice).priority = priority;
    Remove(voice);
    Insert(voice);
    if (observer_ != nullptr)
    {
        observer_->SetVoicePriority(voice, priority);
    }
}

void Renderer::SetVoiceSrcQuality(int voice, SrcQuality quality)
{
    At(voice).quality = quality;
    if (observer_ != nullptr)
    {
        observer_->SetVoiceSrcQuality(voice, quality);
    }
}

void Renderer::SetVoiceBiquadFilterParameter(int voice, int index, const BiquadFilterParameter& parameter)
{
    At(voice).biquads.at(static_cast<std::size_t>(index)) = parameter;
    if (observer_ != nullptr)
    {
        observer_->SetVoiceBiquadFilterParameter(voice, index, parameter);
    }
}

void Renderer::SetVoiceDestination(int voice, int mix)
{
    At(voice).mix = mix;
    if (observer_ != nullptr)
    {
        observer_->SetVoiceDestination(voice, mix);
    }
}

void Renderer::SetVoiceMixVolume(int voice, float volume, int channel, int buffer)
{
    const int resource = At(voice).resources.at(static_cast<std::size_t>(channel));
    resources_[static_cast<std::size_t>(resource)].client_mix.at(static_cast<std::size_t>(buffer)) = volume;
    if (observer_ != nullptr)
    {
        observer_->SetVoiceMixVolume(voice, volume, channel, buffer);
    }
}

bool Renderer::AppendWaveBuffer(int voice, const WaveBufferInfo& buffer)
{
    const bool appended = QueueWaveBuffer(voice, buffer);
    if (observer_ != nullptr)
    {
        observer_->AppendWaveBuffer(voice, buffer, appended);
    }

    return appended;
}

bool Renderer::QueueWaveBuffer(int voice, const WaveBufferInfo& buffer)
{
    Voice& v = At(voice);
    if (v.buffer_count >= kWaveBufferCount)
    {
        return false;
    }

    ClientWaveBuffer& slot = v.buffers[static_cast<std::size_t>((v.buffer_index + v.buffer_count) % kWaveBufferCount)];
    slot.info = buffer;
    slot.sent = false;
    ++v.buffer_count;

    return true;
}

const void* Renderer::GetReleasedWaveBuffer(int voice)
{
    const void* tag = TakeReleasedWaveBuffer(voice);
    if (observer_ != nullptr)
    {
        observer_->GetReleasedWaveBuffer(voice, tag);
    }

    return tag;
}

const void* Renderer::TakeReleasedWaveBuffer(int voice)
{
    Voice& v = At(voice);
    if (v.consumed == v.released || v.buffer_count == 0)
    {
        return nullptr;
    }

    const void* tag = v.buffers[static_cast<std::size_t>(v.buffer_index)].info.tag;
    v.buffer_index = (v.buffer_index + 1) % kWaveBufferCount;
    --v.buffer_count;
    ++v.released;

    return tag;
}

uint64_t Renderer::GetVoicePlayedSampleCount(int voice) const
{
    const uint64_t count = At(voice).played;
    if (observer_ != nullptr)
    {
        observer_->GetVoicePlayedSampleCount(voice, count);
    }

    return count;
}

bool Renderer::IsVoiceDroppedFlagOn(int voice) const
{
    const bool dropped = At(voice).dropped;
    if (observer_ != nullptr)
    {
        observer_->IsVoiceDroppedFlagOn(voice, dropped);
    }

    return dropped;
}

bool Renderer::AcquireFinalMix(int buffer_count)
{
    const bool acquired = TakeFinalMix(buffer_count);
    if (observer_ != nullptr)
    {
        observer_->AcquireFinalMix(buffer_count, acquired);
    }

    return acquired;
}

bool Renderer::TakeFinalMix(int buffer_count)
{
    Mix& mix = mixes_[kFinalMix];
    if (mix.in_use || buffer_count < 1 || buffer_count > kMaxMixBuffers)
    {
        return false;
    }

    mix.in_use = true;
    mix.buffer_count = buffer_count;
    mix.mix_volumes.assign(static_cast<std::size_t>(buffer_count), {});
    mixes_changed_ = true;

    return true;
}

int Renderer::AcquireSubMix(int32_t sample_rate, int buffer_count)
{
    const int sub_mix = TakeSubMix(sample_rate, buffer_count);
    if (observer_ != nullptr)
    {
        observer_->AcquireSubMix(sample_rate, buffer_count, sub_mix);
    }

    return sub_mix;
}

int Renderer::TakeSubMix(int32_t sample_rate, int buffer_count)
{
    for (std::size_t id = 1; id < mixes_.size(); ++id)
    {
        Mix& mix = mixes_[id];
        if (mix.in_use)
        {
            continue;
        }
        if (buffer_count < 1 || buffer_count > kMaxMixBuffers)
        {
            return kNoMix;
        }

        mix = Mix{};
        mix.in_use = true;
        mix.sample_rate = sample_rate;
        mix.buffer_count = buffer_count;
        mix.mix_volumes.assign(static_cast<std::size_t>(buffer_count), {});
        mixes_changed_ = true;

        return static_cast<int>(id);
    }

    return kNoMix;
}

void Renderer::SetSubMixDestination(int sub_mix, int destination)
{
    mixes_.at(static_cast<std::size_t>(sub_mix)).destination = destination;
    mixes_changed_ = true;
    if (observer_ != nullptr)
    {
        observer_->SetSubMixDestination(sub_mix, destination);
    }
}

void Renderer::SetSubMixMixVolume(int sub_mix, float volume, int source, int destination)
{
    Mix& mix = mixes_.at(static_cast<std::size_t>(sub_mix));
    mix.mix_volumes.at(static_cast<std::size_t>(source)).at(static_cast<std::size_t>(destination)) = volume;
    if (observer_ != nullptr)
    {
        observer_->SetSubMixMixVolume(sub_mix, volume, source, destination);
    }
}

void Renderer::AddDeviceSink(std::span<const int> inputs)
{
    sinks_.assign(inputs.begin(), inputs.end());
    output_.assign(static_cast<std::size_t>(parameter_.sample_count) * sinks_.size(), 0);
    if (observer_ != nullptr)
    {
        observer_->AddDeviceSink(inputs);
    }
}

uint32_t Renderer::GetRequiredBufferSizeForAuxSendReturnBuffer(const RendererParameter& parameter, int frames,
                                                               int channels)
{
    const int64_t samples = static_cast<int64_t>(channels * frames * parameter.sample_count);
    return static_cast<uint32_t>((samples * 4 + 0xBF) & ~int64_t{0x3F});
}

int Renderer::AddAux(int mix, uint32_t buffer_size)
{
    // The rings' room, after their headers, in samples and in frames of one channel. They go round nowhere until the
    // effect has its channels.
    Aux aux;
    aux.mix = mix;
    aux.enabled = true;
    const uint32_t bytes = buffer_size - 0x80;
    aux.capacity = bytes / 4;
    aux.capacity_frames = bytes / (static_cast<uint32_t>(parameter_.sample_count) * 4);
    aux.send.assign(aux.capacity, 0);
    aux.receive.assign(aux.capacity, 0);
    auxes_.push_back(std::move(aux));

    const int id = static_cast<int>(auxes_.size()) - 1;
    mixes_.at(static_cast<std::size_t>(mix)).effects.push_back(id);
    if (observer_ != nullptr)
    {
        observer_->AddAux(mix, buffer_size, id);
    }

    return id;
}

void Renderer::SetAuxInputOutput(int aux, std::span<const int8_t> inputs, std::span<const int8_t> outputs)
{
    Aux& a = auxes_.at(static_cast<std::size_t>(aux));
    const int most = mixes_.at(static_cast<std::size_t>(a.mix)).buffer_count;
    const int count = std::min(most, static_cast<int>(std::min(inputs.size(), outputs.size())));
    a.inputs.assign(inputs.begin(), inputs.begin() + std::max(count, 0));
    a.outputs.assign(outputs.begin(), outputs.begin() + std::max(count, 0));
    a.ring = count < 1 ? 0
                       : a.capacity_frames / static_cast<uint32_t>(count) * static_cast<uint32_t>(count) *
                             static_cast<uint32_t>(parameter_.sample_count);
    if (observer_ != nullptr)
    {
        observer_->SetAuxInputOutput(aux, inputs, outputs);
    }
}

void Renderer::SetAuxEnabled(int aux, bool enabled)
{
    Aux& a = auxes_.at(static_cast<std::size_t>(aux));
    a.enabled = enabled;
    a.send_read = 0;
    a.receive_write = 0;
    if (observer_ != nullptr)
    {
        observer_->SetAuxEnabled(aux, enabled);
    }
}

int32_t Renderer::GetAuxSampleCount(int aux) const
{
    const auto count = static_cast<int32_t>(auxes_.at(static_cast<std::size_t>(aux)).capacity);
    if (observer_ != nullptr)
    {
        observer_->GetAuxSampleCount(aux, count);
    }

    return count;
}

int32_t Renderer::GetAuxSampleRate(int aux) const
{
    const int32_t rate = mixes_.at(static_cast<std::size_t>(auxes_.at(static_cast<std::size_t>(aux)).mix)).sample_rate;
    if (observer_ != nullptr)
    {
        observer_->GetAuxSampleRate(aux, rate);
    }

    return rate;
}

int32_t Renderer::ReadAuxSendBuffer(int aux, std::span<int32_t> samples)
{
    const int32_t count = ReadAuxSend(aux, samples);
    if (observer_ != nullptr)
    {
        observer_->ReadAuxSendBuffer(aux, samples, count);
    }

    return count;
}

int32_t Renderer::ReadAuxSend(int aux, std::span<int32_t> samples)
{
    // Everything the renderer has written since the last read, up to the span.
    Aux& a = auxes_.at(static_cast<std::size_t>(aux));
    const uint32_t ring = a.ring;
    if (ring == 0)
    {
        return 0;
    }

    const uint32_t written = (a.send_write < a.send_read ? ring : 0) - a.send_read + a.send_write;
    const uint32_t count = std::min(written, static_cast<uint32_t>(samples.size()));
    for (uint32_t i = 0; i < count; ++i)
    {
        samples[i] = a.send[(a.send_read + i) % ring];
    }
    a.send_read = (a.send_read + count) % ring;

    return static_cast<int32_t>(count);
}

int32_t Renderer::WriteAuxReturnBuffer(int aux, std::span<const int32_t> samples)
{
    const int32_t written = WriteAuxReturn(aux, samples);
    if (observer_ != nullptr)
    {
        observer_->WriteAuxReturnBuffer(aux, samples, written);
    }

    return written;
}

int32_t Renderer::WriteAuxReturn(int aux, std::span<const int32_t> samples)
{
    // As far as the renderer's next read, or a whole ring when the two are at the same place.
    Aux& a = auxes_.at(static_cast<std::size_t>(aux));
    const uint32_t ring = a.ring;
    if (ring == 0)
    {
        return 0;
    }

    const uint32_t room = (a.receive_read > a.receive_write ? 0 : ring) + a.receive_read - a.receive_write;
    const uint32_t count = std::min(room, static_cast<uint32_t>(samples.size()));
    for (uint32_t i = 0; i < count; ++i)
    {
        a.receive[(a.receive_write + i) % ring] = samples[i];
    }
    a.receive_write = (a.receive_write + count) % ring;

    return static_cast<int32_t>(count);
}

int32_t* Renderer::Buffer(int index)
{
    return buffers_.data() + static_cast<std::size_t>(index) * static_cast<std::size_t>(parameter_.sample_count);
}

void Renderer::RenderFrame()
{
    Update();
    Generate();
    if (observer_ != nullptr)
    {
        observer_->RenderFrame();
    }
}

// RequestUpdateAudioRenderer: the renderer takes the client's parameters, and the client takes the voices' statuses.
// The renderer copied the statuses at the last command generation, before that frame's processing.
void Renderer::Update()
{
    for (std::size_t slot = 0; slot < voices_.size(); ++slot)
    {
        Voice& voice = voices_[slot];
        Voice::Server& server = voice.server;
        server.in_use = voice.in_use;
        if (!voice.in_use)
        {
            continue;
        }

        // A new voice starts again from nothing (VoiceInfo::Initialize).
        if (voice.is_new)
        {
            server = Voice::Server{};
            server.in_use = true;
            server.is_new = true;
        }

        // The play state that the renderer keeps (VoiceInfo::UpdateParameters, 0x6a300).
        server.previous = server.state;
        switch (voice.play_state)
        {
        case PlayState::kStarted:
            server.state = 0;
            break;
        case PlayState::kStopped:
            server.state = server.state == 1 ? 1 : 2;
            break;
        case PlayState::kPaused:
            server.state = 3;
            break;
        }

        server.format = voice.format;
        server.sample_rate = voice.sample_rate;
        server.priority = voice.priority;
        server.sort_order =
            static_cast<int32_t>(std::find(order_.begin(), order_.end(), static_cast<int>(slot)) - order_.begin());
        server.channel_count = voice.channel_count;
        server.pitch = voice.pitch;
        server.volume = voice.volume;
        server.quality = voice.quality;
        server.biquads = voice.biquads;
        server.mix = voice.mix;
        server.buffer_count = voice.buffer_count;

        // The buffers the client appended since the last update, if their ranges fit (VoiceInfo::UpdateWaveBuffer).
        for (std::size_t i = 0; i < voice.buffers.size(); ++i)
        {
            const WaveBufferInfo& info = voice.buffers[i].info;
            if (voice.buffers[i].sent || info.start < 0 || info.end < 0)
            {
                continue;
            }

            const uint64_t size = info.buffer.size();
            const uint64_t bytes = voice.format == SampleFormat::kPcmFloat ? 4 : 2;
            const bool fits = voice.format == SampleFormat::kAdpcm
                                  ? static_cast<uint64_t>(AdpcmBytes(info.start)) <= size &&
                                        static_cast<uint64_t>(AdpcmBytes(info.end)) <= size
                                  : static_cast<uint64_t>(info.start) * bytes <= size &&
                                        static_cast<uint64_t>(info.end) * bytes <= size;
            if (!fits)
            {
                continue;
            }

            WaveBuffer& buffer = server.buffers[i];
            buffer.buffer = info.buffer;
            buffer.start = info.start;
            buffer.end = info.end;
            buffer.loop = info.loop;
            buffer.end_of_stream = info.end_of_stream;
            buffer.loop_start = info.start;
            buffer.loop_end = info.end;
            buffer.loop_count = info.loop ? -1 : 0;
            buffer.context = nullptr;
            if (info.context != nullptr)
            {
                server.contexts[i] = *info.context;
                buffer.context = &server.contexts[i];
            }
            server.sent[i] = false;
        }

        for (int c = 0; c < voice.channel_count; ++c)
        {
            ChannelResource& resource =
                resources_[static_cast<std::size_t>(voice.resources[static_cast<std::size_t>(c)])];
            resource.mix = resource.client_mix;
        }

        // The statuses: none for a voice the renderer hasn't started yet, otherwise channel 0's statuses.
        if (server.is_new)
        {
            voice.played = 0;
            voice.consumed = 0;
        }
        else
        {
            const VoiceState& reported = resources_[static_cast<std::size_t>(voice.resources[0])].reported;
            voice.played = reported.played;
            voice.consumed = reported.consumed;
        }
        voice.dropped = false;
        voice.is_new = false;
        for (ClientWaveBuffer& buffer : voice.buffers)
        {
            buffer.sent = true;
        }
    }

    // The effects' settings.
    for (Aux& aux : auxes_)
    {
        aux.server_enabled = aux.enabled;
        aux.server_inputs = aux.inputs;
        aux.server_outputs = aux.outputs;
        aux.server_ring = aux.ring;
    }
}

// The renderer's mixes in the order that the renderer renders them: the furthest from the final mix first, and the
// final mix last. Each mix takes the next range of mix buffers, from the first buffer on.
void Renderer::LayOutMixes()
{
    const int count = static_cast<int>(mixes_.size());
    std::vector<int> distance(mixes_.size(), -1);
    for (int id = 0; id < count; ++id)
    {
        int at = id;
        int steps = 0;
        while (at != kFinalMix && steps <= count && at >= 0 && at < count &&
               mixes_[static_cast<std::size_t>(at)].in_use)
        {
            at = mixes_[static_cast<std::size_t>(at)].destination;
            ++steps;
        }
        distance[static_cast<std::size_t>(id)] = at == kFinalMix && steps <= count ? steps : -1;
    }

    std::vector<int> order(mixes_.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b)
                     { return distance[static_cast<std::size_t>(a)] > distance[static_cast<std::size_t>(b)]; });
    int offset = 0;
    for (const int id : order)
    {
        Mix& mix = mixes_[static_cast<std::size_t>(id)];
        if (mix.in_use)
        {
            mix.offset = offset;
            offset += mix.buffer_count;
        }
    }
    mixes_changed_ = false;
}

void Renderer::Generate()
{
    if (mixes_changed_)
    {
        LayOutMixes();
    }
    std::fill(buffers_.begin(), buffers_.end(), 0);

    // The voices, the least important first, and among equals the oldest first.
    std::vector<int> sorted;
    for (std::size_t slot = 0; slot < voices_.size(); ++slot)
    {
        if (voices_[slot].server.in_use)
        {
            sorted.push_back(static_cast<int>(slot));
        }
    }
    std::stable_sort(sorted.begin(), sorted.end(),
                     [&](int a, int b)
                     {
                         const Voice::Server& x = At(a).server;
                         const Voice::Server& y = At(b).server;
                         if (x.priority != y.priority)
                         {
                             return x.priority > y.priority;
                         }

                         return x.sort_order > y.sort_order;
                     });
    for (const int index : sorted)
    {
        RenderVoice(At(index));
    }

    // The sub-mixes in their order, then the final mix and the sink.
    std::vector<int> mix_order;
    for (std::size_t id = 1; id < mixes_.size(); ++id)
    {
        if (mixes_[id].in_use)
        {
            mix_order.push_back(static_cast<int>(id));
        }
    }
    std::stable_sort(
        mix_order.begin(), mix_order.end(), [&](int a, int b)
        { return mixes_[static_cast<std::size_t>(a)].offset < mixes_[static_cast<std::size_t>(b)].offset; });
    for (const int id : mix_order)
    {
        RenderSubMix(mixes_[static_cast<std::size_t>(id)]);
    }
    RenderFinalMix();

    const Mix& final_mix = mixes_[kFinalMix];
    const std::size_t channels = sinks_.size();
    for (std::size_t k = 0; k < channels; ++k)
    {
        const int32_t* samples = Buffer(final_mix.offset + sinks_[k]);
        for (int t = 0; t < parameter_.sample_count; ++t)
        {
            const int32_t x = samples[t];
            output_[static_cast<std::size_t>(t) * channels + k] = static_cast<int16_t>(std::clamp(x, -32768, 32767));
        }
    }
}

void Renderer::RenderVoice(Voice& voice)
{
    Voice::Server& server = voice.server;
    const int channels = server.channel_count;
    const auto state = [&](int c) -> VoiceState&
    {
        return resources_[static_cast<std::size_t>(voice.resources[static_cast<std::size_t>(c)])].dsp;
    };
    const auto report = [&]()
    {
        for (int c = 0; c < channels; ++c)
        {
            ChannelResource& resource =
                resources_[static_cast<std::size_t>(voice.resources[static_cast<std::size_t>(c)])];
            resource.reported = resource.dsp;
        }
    };

    // A voice that isn't in use, has no buffers, or was dropped, is skipped (VoiceInfo::ShouldSkip).
    if (server.buffer_count == 0)
    {
        report();
        return;
    }

    // A new voice starts from cleared states, at its volumes (VoiceInfo::UpdateForCommandGeneration, 0x6b480).
    if (server.is_new)
    {
        for (int c = 0; c < channels; ++c)
        {
            ChannelResource& resource =
                resources_[static_cast<std::size_t>(voice.resources[static_cast<std::size_t>(c)])];
            resource.dsp = VoiceState{};
            resource.previous_mix = resource.mix;
        }
        server.previous_volume = server.volume;
        server.is_new = false;
    }

    // The play state (VoiceInfo::UpdateParametersForCommandGeneration, 0x6ad00): a started voice takes its new buffers,
    // and plays if it has any; a voice that stops gives back every buffer and starts again from nothing. A voice that
    // was playing and stops or pauses has a frame of depop.
    bool render = false;
    if (server.state == 0)
    {
        for (std::size_t i = 0; i < kWaveBufferCount; ++i)
        {
            if (!server.sent[i])
            {
                for (int c = 0; c < channels; ++c)
                {
                    state(c).valid[i] = true;
                }
                server.sent[i] = true;
            }
        }
        server.stopping = false;
        render = std::any_of(state(0).valid.begin(), state(0).valid.end(), [](bool v) { return v; });
    }
    else
    {
        if (server.state == 2)
        {
            for (std::size_t i = 0; i < kWaveBufferCount; ++i)
            {
                server.sent[i] = true;
                for (int c = 0; c < channels; ++c)
                {
                    VoiceState& s = state(c);
                    if (s.valid[i])
                    {
                        s.index = (s.index + 1) % kWaveBufferCount;
                        ++s.consumed;
                        s.valid[i] = false;
                    }
                }
            }
            for (int c = 0; c < channels; ++c)
            {
                VoiceState& s = state(c);
                s.offset = 0;
                s.played = 0;
                s.adpcm = {};
                s.fraction = 0;
                s.history = {};
            }
            server.state = 1;
        }
        server.stopping = server.previous == 0;
        render = server.stopping;
    }

    // The renderer reports the states as the generation leaves them, before this frame's processing.
    report();
    if (!render)
    {
        return;
    }

    const bool connected = server.mix >= 0 && server.mix < static_cast<int>(mixes_.size()) &&
                           mixes_[static_cast<std::size_t>(server.mix)].in_use;
    const Mix* destination = connected ? &mixes_[static_cast<std::size_t>(server.mix)] : nullptr;
    const int count = parameter_.sample_count;
    for (int c = 0; c < channels; ++c)
    {
        VoiceState& s = state(c);
        ChannelResource& resource = resources_[static_cast<std::size_t>(voice.resources[static_cast<std::size_t>(c)])];
        int32_t* work = Buffer(parameter_.mix_buffer_count + c);

        // The depop preparation: a voice that stops leaves its last samples to fade out.
        if (server.stopping)
        {
            if (destination != nullptr)
            {
                for (int b = 0; b < destination->buffer_count; ++b)
                {
                    int32_t& last = s.last_samples[static_cast<std::size_t>(b)];
                    if (last != 0)
                    {
                        int32_t& depop = depop_[static_cast<std::size_t>(destination->offset + b)];
                        depop = WrapAdd(depop, last);
                        last = 0;
                    }
                }
            }
            server.previous_volume = 0.0f;
            continue;
        }

        // The data source.
        DecodeFromWaveBuffersArgs args;
        args.format = server.format;
        args.out = work;
        args.state = &s;
        args.buffers = &server.buffers;
        args.channel = c;
        args.channel_count = channels;
        args.quality = server.quality;
        args.pitch = ToFixed(server.pitch, 15);
        args.source_rate = server.sample_rate;
        args.target_rate = parameter_.sample_rate;
        args.count = count;
        args.coefficients = &voice.coefficients;
        DecodeFromWaveBuffers(args);
        if (destination == nullptr)
        {
            continue;
        }

        // The biquads, in floating point, a filter's state cleared when it comes on.
        for (std::size_t i = 0; i < 2; ++i)
        {
            const BiquadFilterParameter& filter = server.biquads[i];
            if (!filter.enabled)
            {
                continue;
            }

            if (!server.biquads_were_enabled[i])
            {
                s.biquads[i] = {};
            }
            std::vector<float> samples(static_cast<std::size_t>(count));
            ConvertToFloat(samples.data(), work, count);
            ApplyBiquadFilter(samples.data(), samples.data(), filter.b, filter.a, s.biquads[i], count);
            ConvertToInt(work, samples.data(), count);
        }

        // The volume, from the last frame's volume.
        const int32_t from = ToFixed(server.previous_volume, kGainBits);
        const int32_t to = server.previous_volume == server.volume ? from : ToFixed(server.volume, kGainBits);
        ApplyLinearEnvelopeGain(work, work, from, RampStep(from, to, count), count, kGainBits);
        server.previous_volume = server.volume;

        // The mix volumes, from the last frame's volumes: one command for a destination of more than 6 buffers, which
        // gives a silent buffer a last sample of 0, else one for each buffer that isn't silent.
        const bool grouped = destination->buffer_count > 6;
        for (int b = 0; b < destination->buffer_count; ++b)
        {
            const float before = resource.previous_mix[static_cast<std::size_t>(b)];
            const float now = resource.mix[static_cast<std::size_t>(b)];
            int32_t& last = s.last_samples[static_cast<std::size_t>(b)];
            if (!grouped && before == 0.0f && now == 0.0f)
            {
                continue;
            }

            const int32_t start = ToFixed(before, kGainBits);
            const int32_t end = before == now ? start : ToFixed(now, kGainBits);
            if (grouped && start == 0 && end == 0)
            {
                last = 0;
                continue;
            }

            last = ApplyMixRamp(Buffer(destination->offset + b), work, start, RampStep(start, end, count), count,
                                kGainBits);
        }
        resource.previous_mix = resource.mix;
        server.biquads_were_enabled = {server.biquads[0].enabled, server.biquads[1].enabled};
    }
}

// The depop of a mix's buffers: each buffer's step fades out across the frames.
void Renderer::Depop(const Mix& mix)
{
    const int32_t decay = mix.sample_rate == 48000 ? kDepopDecay48k : kDepopDecay;
    for (int b = 0; b < mix.buffer_count; ++b)
    {
        int32_t& depop = depop_[static_cast<std::size_t>(mix.offset + b)];
        if (depop != 0)
        {
            depop = ApplyDepopMix(Buffer(mix.offset + b), depop, decay, parameter_.sample_count);
        }
    }
}

// A mix's effects in their order. An aux's command for each channel writes the input buffer into the send ring and
// reads the output buffer from the return ring, both from the renderer's offsets plus the channel's place in the frame.
// The last channel's command moves both offsets on by a frame of every channel (GenerateAuxCommand, 0x5f4a0, and
// WriteAuxBufferDsp and ReadAuxBufferDsp, 0x6d2b0 and 0x6d0f0). A disabled aux starts its rings again and passes its
// inputs through.
void Renderer::RenderEffects(const Mix& mix)
{
    const auto count = static_cast<uint32_t>(parameter_.sample_count);
    for (const int id : mix.effects)
    {
        Aux& aux = auxes_[static_cast<std::size_t>(id)];
        const std::size_t channels = aux.server_inputs.size();
        if (!aux.server_enabled)
        {
            aux.send_write = 0;
            aux.receive_read = 0;
            for (std::size_t k = 0; k < channels; ++k)
            {
                const int32_t* in = Buffer(mix.offset + aux.server_inputs[k]);
                int32_t* out = Buffer(mix.offset + aux.server_outputs[k]);
                if (in != out)
                {
                    std::copy(in, in + count, out);
                }
            }
            continue;
        }

        const uint32_t ring = aux.server_ring;
        for (std::size_t k = 0; k < channels && ring != 0; ++k)
        {
            const uint32_t offset = static_cast<uint32_t>(k) * count;
            const int32_t* in = Buffer(mix.offset + aux.server_inputs[k]);
            if (aux.send_write + offset <= ring)
            {
                for (uint32_t t = 0; t < count; ++t)
                {
                    aux.send[(aux.send_write + offset + t) % ring] = in[t];
                }
            }
            int32_t* out = Buffer(mix.offset + aux.server_outputs[k]);
            if (aux.receive_read + offset <= ring)
            {
                for (uint32_t t = 0; t < count; ++t)
                {
                    out[t] = aux.receive[(aux.receive_read + offset + t) % ring];
                }
            }
        }
        if (ring != 0)
        {
            const auto frame = static_cast<uint32_t>(channels) * count;
            aux.send_write = (aux.send_write + frame) % ring;
            aux.receive_read = (aux.receive_read + frame) % ring;
        }
    }
}

void Renderer::RenderSubMix(const Mix& mix)
{
    Depop(mix);
    RenderEffects(mix);

    const int target = mix.destination;
    if (target < 0 || target >= static_cast<int>(mixes_.size()) || !mixes_[static_cast<std::size_t>(target)].in_use)
    {
        return;
    }

    const Mix& destination = mixes_[static_cast<std::size_t>(target)];
    for (int i = 0; i < mix.buffer_count; ++i)
    {
        for (int j = 0; j < destination.buffer_count; ++j)
        {
            const float gain = mix.volume * mix.mix_volumes[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)];
            if (gain != 0.0f)
            {
                ApplyMix(Buffer(destination.offset + j), Buffer(mix.offset + i), ToFixed(gain, kGainBits),
                         parameter_.sample_count, kGainBits);
            }
        }
    }
}

void Renderer::RenderFinalMix()
{
    const Mix& mix = mixes_[kFinalMix];
    if (!mix.in_use)
    {
        return;
    }

    Depop(mix);
    RenderEffects(mix);
    const int32_t gain = ToFixed(mix.volume, kGainBits);
    for (int b = 0; b < mix.buffer_count; ++b)
    {
        int32_t* samples = Buffer(mix.offset + b);
        ApplyUniformGain(samples, samples, gain, parameter_.sample_count, kGainBits);
    }
}

} // namespace hsf::audio
