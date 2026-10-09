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
      voices_(allocator_, options_.hardware, options.voice_count),
      channels_(voices_, options.voice_count)
{
    global_vars_.fill(-1);
}

const fsar::Bank* Engine::GetBank(uint32_t bank_item)
{
    if (const auto it = banks_.find(bank_item); it != banks_.end())
    {
        return it->second.get();
    }

    const uint32_t index = bank_item & 0xFFFFFF;
    if ((bank_item >> 24) != 0x03 || index >= archive_->Banks().size())
    {
        return nullptr;
    }
    const std::span<const uint8_t> file = archive_->FileData(archive_->Banks()[index].file_id);
    if (file.empty())
    {
        return nullptr;
    }

    return banks_.emplace(bank_item, std::make_unique<const fsar::Bank>(fsar::Bank::Parse(file))).first->second.get();
}

const fsar::WaveArchive* Engine::GetWaveArchive(uint32_t wave_archive_item)
{
    if (const auto it = wave_archives_.find(wave_archive_item); it != wave_archives_.end())
    {
        return it->second.get();
    }

    const uint32_t index = wave_archive_item & 0xFFFFFF;
    if ((wave_archive_item >> 24) != 0x05 || index >= archive_->WaveArchives().size())
    {
        return nullptr;
    }
    const std::span<const uint8_t> file = archive_->FileData(archive_->WaveArchives()[index].file_id);
    if (file.empty())
    {
        return nullptr;
    }

    auto archive = std::make_unique<const fsar::WaveArchive>(fsar::WaveArchive::Parse(file));
    return wave_archives_.emplace(wave_archive_item, std::move(archive)).first->second.get();
}

std::optional<WaveInfo> Engine::GetWave(uint32_t wave_archive_item, uint32_t index)
{
    const auto key = std::make_pair(wave_archive_item, index);
    if (const auto it = waves_.find(key); it != waves_.end())
    {
        return it->second;
    }

    // WaveFileReader::ReadWaveInfo (0xc0b410): PCM8 and PCM16 by their codes, and any other code as DSP-ADPCM.
    std::optional<WaveInfo> result;
    const fsar::WaveArchive* archive = GetWaveArchive(wave_archive_item);
    if (archive != nullptr && index < archive->waves.size())
    {
        const fsar::Wave wave = fsar::Wave::Parse(archive->waves[index]);
        if (!wave.channels.empty())
        {
            WaveInfo info;
            switch (wave.encoding)
            {
            case fsar::WaveEncoding::kPcm8:
                info.format = SampleFormat::kPcm8;
                break;
            case fsar::WaveEncoding::kPcm16:
                info.format = SampleFormat::kPcm16;
                break;
            default:
                info.format = SampleFormat::kDspAdpcm;
                break;
            }
            info.loop = wave.loop;
            info.channel_count = static_cast<int>(wave.channels.size());
            info.sample_rate = wave.sample_rate;
            info.loop_start = wave.loop_start;
            info.loop_end = wave.loop_end;
            info.original_loop_start = wave.loop_start;
            for (std::size_t c = 0; c < std::min<std::size_t>(wave.channels.size(), 2); ++c)
            {
                const fsar::WaveChannel& from = wave.channels[c];
                WaveInfo::Channel& to = info.channels[c];
                to.data = from.data;
                if (from.adpcm)
                {
                    to.coefs = from.adpcm->coefs;
                    to.context = {from.adpcm->pred_scale, from.adpcm->yn1, from.adpcm->yn2};
                    to.loop_context = {from.adpcm->loop_pred_scale, from.adpcm->loop_yn1, from.adpcm->loop_yn2};
                }
            }
            result = info;
        }
    }

    waves_.emplace(key, result);

    return result;
}

Channel* Engine::NoteOn(const std::vector<uint32_t>& banks, int slot, const NoteOnInfo& info)
{
    if (slot < 0 || static_cast<std::size_t>(slot) >= banks.size())
    {
        return nullptr;
    }

    const fsar::Bank* bank = GetBank(banks[static_cast<std::size_t>(slot)]);
    if (bank == nullptr || info.prg_no >= bank->instruments.size() || !bank->instruments[info.prg_no])
    {
        return nullptr;
    }
    const fsar::VelocityRegion* region = bank->instruments[info.prg_no]->Find(info.key, info.velocity);
    if (region == nullptr || region->wave_id_index >= bank->waves.size())
    {
        return nullptr;
    }
    const fsar::WaveId& id = bank->waves[region->wave_id_index];
    const std::optional<WaveInfo> wave = GetWave(id.wave_archive_item, id.index);
    if (!wave)
    {
        return nullptr;
    }

    Channel* channel = channels_.AllocChannel(std::min(wave->channel_count, 2), info.priority, info.callback);
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
    channel->interpolation_type = region->interpolation_type;
    if (channel->voice != nullptr)
    {
        channel->voice->SetOutputReceiver(info.receiver);
    }
    channel->Start(*wave, info.length);

    return channel;
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

    const fsar::Sequence sequence = fsar::Sequence::Parse(file);
    players_.push_back(std::make_unique<SequenceSoundPlayer>(*this, info, sequence.data, receiver_));
    pending_.push_back({players_.back().get(), 1});

    return players_.back().get();
}

void Engine::FrameProcess()
{
    voices_.UpdateAllVoiceStatus();

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
        player->Update(1);
    }
    channels_.UpdateAllChannel(1, random_);
    voices_.UpdateAllVoices();

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

ArchiveModel::ArchiveModel(const fsar::SoundArchive& archive, EngineOptions options)
    : renderer(RendererParameterFor(options))
{
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

    final_mix.type = OutputReceiver::Type::kFinalMix;
    final_mix.mix = audio::Renderer::kFinalMix;
    final_mix.channel_count = kChannels;
    final_mix.bus_count = 1;
    final_mix.clamp_send = {true, true, true, true};
    sub_mix.type = OutputReceiver::Type::kSubMix;
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
