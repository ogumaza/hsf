// SPDX-License-Identifier: MIT

#include "atk/engine.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace hsf::atk
{
namespace
{

// The renderer nn::atk opens (HardwareManager::Initialize, 0xc05340): 5 ms frames at 48 kHz, one sub-mix, 30 mix
// buffers (24 for the sub-mix and 6 for the final mix), and a voice for each of the sound system's voices.
audio::RendererParameter RendererParameterFor(const EngineOptions& options)
{
    audio::RendererParameter parameter;
    parameter.sample_rate = ArchiveModel::kSampleRate;
    parameter.sample_count = ArchiveModel::kFrameSamples;
    parameter.mix_buffer_count = 30;
    parameter.sub_mix_count = 1;
    parameter.voice_count = options.voice_count;

    return parameter;
}

} // namespace

Engine::Engine(audio::Renderer& renderer, const fsar::SoundArchive& archive, const OutputReceiver& receiver,
               EngineOptions options)
    : renderer_(&renderer),
      archive_(&archive),
      receiver_(&receiver),
      options_(options),
      allocator_(renderer, options_.hardware, options.voice_count),
      voice_renderer_(allocator_, options_.hardware, options.voice_count),
      voices_(options.voice_count),
      channels_(options.voice_count),
      files_(archive, kSarOptions)
{
    global_vars_.fill(-1);
}

Channel* Engine::NoteOn(const std::vector<uint32_t>& banks, int slot, const NoteOnInfo& info)
{
    const std::optional<Files::Note> note = files_.FindNote(banks, slot, info.prg_no, info.key, info.velocity);
    if (!note)
    {
        return nullptr;
    }

    const fsar::VelocityRegion* region = note->region;
    Channel* channel = channels_.AllocChannel(*this, std::min(note->wave.channel_count, 2), info.priority,
                                              info.owner_player, info.owner_track);
    if (channel == nullptr)
    {
        return nullptr;
    }

    channel->key = static_cast<uint8_t>(info.key);
    channel->original_key = region->original_key;
    channel->velocity_volume = static_cast<float>(static_cast<uint8_t>(info.velocity)) * (1.0f / 127.0f);
    channel->init_volume = static_cast<float>(region->volume) * (1.0f / 127.0f);
    channel->tune = region->pitch;
    channel->envelope.SetAttack(region->adshr.attack);
    channel->envelope.SetHold(region->adshr.hold);
    channel->envelope.SetDecay(region->adshr.decay);
    channel->envelope.SetSustain(region->adshr.sustain);
    channel->envelope.SetRelease(region->adshr.release);
    channel->init_pan = static_cast<float>(info.init_pan + static_cast<int>(region->pan) - 0x40) / 63.0f;
    channel->key_group = region->key_group;
    channel->ignore_note_off = region->ignore_note_off;
    channel->interpolation_type = region->interpolation;
    if (channel->voice != ntdlib::nwsnd::kNoVoice)
    {
        channel->Voice(*this).SetOutputReceiver(info.receiver);
    }
    channel->Start(*this, note->wave, info.length);

    return channel;
}

void Engine::ChannelEvent(int player, int track, Channel& channel, Channel::CallbackStatus)
{
    players_[static_cast<std::size_t>(player)]->ChannelEvent(*this, track, channel);
}

SequenceSoundPlayer* Engine::StartSequence(uint32_t index)
{
    if (index >= archive_->Sounds().size())
    {
        return nullptr;
    }

    const fsar::SoundInfo& info = archive_->Sounds()[index];
    if (info.type != fsar::SoundType::kSequence)
    {
        return nullptr;
    }

    const std::span<const uint8_t> file = archive_->FileData(info.file_id);
    if (file.empty())
    {
        return nullptr;
    }

    const fsar::Sequence sequence = fsar::Sequence::Parse(file, kSarOptions);
    const int id = static_cast<int>(players_.size());
    players_.push_back(std::make_unique<SequenceSoundPlayer>(id, info, sequence.data, receiver_));
    pending_.push_back({players_.back().get(), 1});

    return players_.back().get();
}

void Engine::FrameProcess()
{
    voices_.UpdateAllVoiceStatus(*this);

    // The sound thread's commands. They start the sounds that are due.
    for (auto it = pending_.begin(); it != pending_.end();)
    {
        if (it->frames > 0)
        {
            --it->frames;
            ++it;
            continue;
        }

        it->player->Start();
        it = pending_.erase(it);
    }

    // The players' frames, the channels and the voices, a renderer frame each.
    for (const std::unique_ptr<SequenceSoundPlayer>& player : players_)
    {
        player->Update(*this, 1);
    }
    channels_.UpdateAllChannel(*this);
    voices_.UpdateAllVoices(*this);

    // HardwareManager::Update (0xc05d90) has no fades to move here. The random numbers step once a frame.
    random_.Next();
}

void Engine::RunFrame()
{
    // SoundThread::Run: the voices' statuses, the sound frame, then the voices' parameters and states reach the
    // renderer. The renderer renders a frame with them.
    allocator_.UpdateAllVoiceStatus();
    FrameProcess();
    allocator_.UpdateAllVoice(options_.hardware.output_mode);
    UpdateEffects();
    renderer_->RenderFrame();
}

void Engine::AppendEffect(int bus, std::unique_ptr<EffectAux> effect)
{
    // Buffers for the effect's ring, with whole frames of the effect's channels. The effect takes the bus's first
    // channels.
    const uint32_t size = audio::Renderer::GetRequiredBufferSizeForAuxSendReturnBuffer(
        renderer_->Parameter(), EffectAux::kFrameCount, EffectAux::kChannelCount);
    AttachedEffect attached;
    attached.effect = std::move(effect);
    attached.aux = renderer_->AddAux(receiver_->mix, size);
    std::vector<int8_t> buffers;
    for (int channel = 0; channel < EffectAux::kChannelCount; ++channel)
    {
        buffers.push_back(static_cast<int8_t>((bus + 1) * receiver_->channel_count + channel));
    }
    renderer_->SetAuxInputOutput(attached.aux, buffers, buffers);
    renderer_->SetAuxEnabled(attached.aux, true);
    attached.work.resize(static_cast<std::size_t>(renderer_->GetAuxSampleCount(attached.aux)));
    effects_.at(static_cast<std::size_t>(bus) + 1).push_back(std::move(attached));
}

void Engine::UpdateEffects()
{
    // Only a receiver whose voices reach every bus updates its effects.
    if (!options_.hardware.all_buses)
    {
        return;
    }

    for (std::vector<AttachedEffect>& bus : effects_)
    {
        for (AttachedEffect& attached : bus)
        {
            const int32_t capacity = renderer_->GetAuxSampleCount(attached.aux);
            EffectAux::BufferInfo info;
            info.sample_count_per_frame = capacity / (EffectAux::kFrameCount * EffectAux::kChannelCount);
            info.sample_rate = renderer_->GetAuxSampleRate(attached.aux);
            info.frame_count = EffectAux::kFrameCount;
            info.channel_count = EffectAux::kChannelCount;
            info.output_mode = options_.hardware.output_mode;
            info.read_sample_count = renderer_->ReadAuxSendBuffer(attached.aux, attached.work);
            const auto samples =
                std::span<int32_t>(attached.work).first(static_cast<std::size_t>(info.read_sample_count));
            attached.effect->UpdateBuffer(samples, info);
            renderer_->WriteAuxReturnBuffer(attached.aux, samples);
        }
    }
}

bool Engine::IsBusy() const
{
    for (const std::unique_ptr<SequenceSoundPlayer>& player : players_)
    {
        if (!player->IsFinished())
        {
            return true;
        }
    }

    return channels_.ActiveCount() > 0;
}

ArchiveModel::ArchiveModel(const fsar::SoundArchive& archive, EngineOptions options, audio::RendererObserver* observer)
    : renderer(RendererParameterFor(options))
{
    renderer.SetObserver(observer);

    // The final mix, and sub-mix 0. Each of sub-mix 0's buses reaches the final mix's channels at 1.0.
    constexpr int kChannels = 6;
    constexpr int kBuses = 4;
    renderer.AcquireFinalMix(kChannels);
    const int sub = renderer.AcquireSubMix(kSampleRate, kChannels * kBuses);
    renderer.SetSubMixDestination(sub, audio::Renderer::kFinalMix);
    for (int bus = 0; bus < kBuses; ++bus)
    {
        for (int channel = 0; channel < kChannels; ++channel)
        {
            renderer.SetSubMixMixVolume(sub, 1.0f, bus * kChannels + channel, channel);
        }
    }

    // The console's output in stereo: the final mix's front left and right. Nothing reaches its other channels while
    // nn::atk mixes for stereo.
    constexpr std::array<int, 2> kSinkInputs = {0, 1};
    renderer.AddDeviceSink(kSinkInputs);

    sub_mix.mix = sub;
    sub_mix.channel_count = kChannels;
    sub_mix.bus_count = kBuses;
    engine = std::make_unique<Engine>(renderer, archive, sub_mix, options);

    // The scene's reverbs, in place before any sound starts.
    for (std::size_t bus = 0; bus < options.aux_reverbs.size(); ++bus)
    {
        if (options.aux_reverbs[bus])
        {
            engine->AppendEffect(static_cast<int>(bus), std::make_unique<I3dl2AuxEffect>(*options.aux_reverbs[bus]));
        }
    }
}

} // namespace hsf::atk
