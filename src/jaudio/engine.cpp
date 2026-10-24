// SPDX-License-Identifier: MIT

// JAudio2's sound system on Hagi (see engine.h).

#include "jaudio/engine.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/jaudio/archive.h"
#include "ntdlib/jaudio/bank.h"
#include "ntdlib/jaudio/rarc.h"
#include "ntdlib/jaudio/wave_system.h"
#include "xsflib/binary.h"

#include "jaudio/channel.h"
#include "jaudio/dsp.h"
#include "jaudio/instruments.h"
#include "jaudio/tables.h"
#include "jaudio/track.h"

namespace hsf::jaudio
{
namespace
{

namespace nj = ntdlib::jaudio;

// The DSP's memory below the waves: the 64 channels' blocks, the FX lines' settings, the outputs of a frame's 7
// sub-frames, and the FX lines' buffers of 128 sub-frames each.
constexpr uint32_t kBlocks = 0x0000;
constexpr uint32_t kFxSettings = 0x6000;
constexpr uint32_t kLeft = 0x6100;
constexpr uint32_t kRight = 0x6800;
constexpr uint32_t kFxBuffers = 0x8000;
constexpr uint32_t kFxBufferBytes = FxLineSettings::kMaxSubframes * 0xa0;
static_assert(kFxBuffers + 4 * kFxBufferBytes <= SoundFiles::kWaves);

// The sound table's volume of a sound (JAudio2's getSoundInfo_ in the game's sound information): the table's byte
// times the float nearest 1/255.
constexpr float kVolumeScale = std::bit_cast<float>(0x3b808081u);

// The sections of the sound table that the game's managers play: effects and music.
constexpr uint8_t kEffectSection = 0;
constexpr uint8_t kMusicSection = 1;

// The tracks that the sequence manager reserves for each piece of music.
constexpr uint32_t kReservedTracks = 16;

constexpr uint32_t Tag(const char (&name)[5])
{
    return static_cast<uint32_t>(static_cast<uint8_t>(name[0])) << 24 |
           static_cast<uint32_t>(static_cast<uint8_t>(name[1])) << 16 |
           static_cast<uint32_t>(static_cast<uint8_t>(name[2])) << 8 | static_cast<uint8_t>(name[3]);
}

// A bank that the start-up archive's commands load ("bnk "): the wave system it plays and its offset.
struct BankCommand
{
    uint32_t wave_system = 0;
    uint32_t offset = 0;
};

// The banks of a start-up archive, in the order of its commands. Each tag is a 32-bit word in the archive's byte order.
// A little-endian archive starts with "<_AA".
std::vector<BankCommand> BankCommands(std::span<const uint8_t> baa)
{
    const bool little = !baa.empty() && baa[0] == '<';
    const xsflib::Reader r(baa, !little);
    std::vector<BankCommand> banks;
    for (std::size_t at = 4;;)
    {
        const uint32_t tag = r.U32(at);
        at += 4;
        if (tag == Tag(">_AA"))
        {
            return banks;
        }

        if (tag == Tag("bnk "))
        {
            banks.push_back({r.U32(at), r.U32(at + 4)});
        }
        if (tag == Tag("ws  ") || tag == Tag("bms "))
        {
            at += 12;
        }
        else if (tag == Tag("bnk ") || tag == Tag("bst ") || tag == Tag("bstn") || tag == Tag("bsc ") ||
                 tag == Tag("bl_<") || tag == Tag("vbnk") || tag == Tag("baac"))
        {
            at += 8;
        }
        else if (tag == Tag("bmsa") || tag == Tag("dsqb") || tag == Tag("bsft") || tag == Tag("sect") ||
                 tag == Tag("bfca"))
        {
            at += 4;
        }
        else if (tag != Tag(">_bl"))
        {
            throw xsflib::FormatError("start-up archive has a command of unknown kind at " + std::to_string(at - 4));
        }
    }
}

std::vector<uint8_t> Decoded(std::span<const uint8_t> data)
{
    std::vector<uint8_t> decoded;
    const std::span<const uint8_t> bytes = nj::DecodeIfYaz0(data, decoded);

    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

} // namespace

SoundFiles::SoundFiles(std::span<const uint8_t> startup, std::span<const uint8_t> sequences, const WaveArchives& waves)
    : baa_(Decoded(startup))
{
    archive_ = std::make_unique<nj::SoundArchive>(nj::SoundArchive::Parse(baa_));
    const std::span<const uint8_t> collection = archive_->Collection();
    collection_ = std::make_unique<Sequence>(std::vector<uint8_t>(collection.begin(), collection.end()));
    if (!sequences.empty())
    {
        const std::vector<uint8_t> rarc = Decoded(sequences);
        for (const nj::ArchiveFile& file : nj::ReadRarc(rarc))
        {
            files_[file.id] = std::make_unique<Sequence>(Decoded(file.data));
        }
    }

    LoadWaves(waves);
    LoadBanks();
}

const nj::SoundEntry* SoundFiles::Find(uint32_t id) const
{
    for (const nj::SoundEntry& sound : archive_->Sounds())
    {
        if (sound.Id() == id)
        {
            return &sound;
        }
    }

    return nullptr;
}

bool SoundFiles::RhythmPiece(const nj::SoundEntry& sound) const
{
    if ((sound.type & 0xf0) != 0x60 || std::size_t{sound.offset} + 8 > baa_.size())
    {
        return false;
    }

    const xsflib::Reader r(baa_, baa_[0] != '<');
    return (r.U16(sound.offset + 6) & 1) != 0;
}

SoundFiles::SequenceRef SoundFiles::SequenceOf(const nj::SoundEntry& sound) const
{
    if (sound.stream)
    {
        return {};
    }

    if (sound.source == nj::SoundEntry::Source::kArchive)
    {
        const auto file = files_.find(sound.resource);
        return file != files_.end() ? SequenceRef{file->second.get(), 0} : SequenceRef{};
    }

    if (sound.source == nj::SoundEntry::Source::kCollection)
    {
        const std::optional<uint32_t> offset = archive_->CollectionSequence(sound.group, sound.resource);
        return offset ? SequenceRef{collection_.get(), *offset} : SequenceRef{};
    }

    return {};
}

void SoundFiles::LoadWaves(const WaveArchives& waves)
{
    // Each wave system's wave archives go into the DSP's memory, each once, on 32-byte boundaries. A wave plays from
    // the archive of the first group that lists it, at the wave's offset there.
    const std::vector<nj::WaveSystem> systems = nj::ReadWaveSystems(baa_);
    std::map<std::string, const std::vector<uint8_t>*> archives;
    for (const nj::WaveSystem& system : systems)
    {
        for (const nj::WaveGroup& group : system.groups)
        {
            if (archives.contains(group.archive))
            {
                continue;
            }

            const std::vector<uint8_t>* bytes = waves(group.archive);
            archives[group.archive] = bytes;
            if (bytes == nullptr)
            {
                missing_waves_.push_back(group.archive);
            }
        }
    }

    uint32_t size = kWaves;
    std::map<std::string, uint32_t> addresses;
    for (const auto& [name, bytes] : archives)
    {
        if (bytes != nullptr && !bytes->empty())
        {
            addresses[name] = size;
            size = (size + static_cast<uint32_t>(bytes->size()) + 31) & ~31u;
        }
    }
    memory_ = std::make_unique<DspMemory>(size);
    for (const auto& [name, address] : addresses)
    {
        const std::vector<uint8_t>& bytes = *archives[name];
        std::copy(bytes.begin(), bytes.end(), memory_->Bytes().begin() + address);
    }

    for (const nj::WaveSystem& system : systems)
    {
        WaveBank& bank = wave_banks_[system.number];
        for (const auto& [id, info] : system.waves)
        {
            const auto group = std::find_if(system.groups.begin(), system.groups.end(), [id](const nj::WaveGroup& g)
                                            { return std::find(g.waves.begin(), g.waves.end(), id) != g.waves.end(); });
            if (group == system.groups.end())
            {
                continue;
            }

            const auto address = addresses.find(group->archive);
            if (address == addresses.end())
            {
                continue;
            }

            WaveInfo wave;
            wave.format = static_cast<uint8_t>(info.format);
            wave.base_key = info.base_key;
            wave.sample_rate = info.sample_rate;
            wave.loop_flags = info.loop ? 0xffffffff : 0;
            wave.loop_start = info.loop_start;
            wave.loop_end = info.loop_end;
            wave.sample_count = info.sample_count;
            wave.last = info.loop_last;
            wave.penult = info.loop_penult;
            bank.Add(id, BankWave{wave, address->second + info.offset});
        }
    }
}

void SoundFiles::LoadBanks()
{
    for (const BankCommand& command : BankCommands(baa_))
    {
        if (command.offset >= baa_.size())
        {
            throw xsflib::FormatError("a bank of the start-up archive starts past its end");
        }

        const std::span<const uint8_t> file = std::span<const uint8_t>(baa_).subspan(command.offset);
        const nj::Bank parsed = nj::Bank::Parse(file, command.wave_system);
        auto bank = std::make_unique<InstrumentBank>(parsed, file);
        const auto waves = wave_banks_.find(command.wave_system);
        bank->SetWaveBank(waves != wave_banks_.end() ? &waves->second : nullptr);
        banks_.push_back(std::move(bank));
    }
}

// The model of one sound: the channels, the tracks and the DSP, and the levels that the game's code gives them.
namespace
{

// Whether a channel holds its note: it plays at the sustain level of its envelopes, on a looping wave.
bool NoteHeld(const Channel& channel)
{
    if (!channel.alive || channel.status != Channel::kPlay ||
        (channel.type == Channel::kWave && channel.wave.loop_flags == 0))
    {
        return false;
    }

    return std::all_of(channel.oscillators.begin(), channel.oscillators.end(), [](const Oscillator& oscillator)
                       { return !oscillator.Valid() || oscillator.State().state == Oscillator::kSustain; });
}

// Whether a track has opened a track that is still open.
bool HasOpenChildren(const Tracks& tracks, uint32_t index)
{
    const Track& track = tracks.TrackAt(index);
    return std::any_of(track.children.begin(), track.children.end(), [&tracks](uint32_t child)
                       { return child != Track::kNone && tracks.TrackAt(child).status != Track::kFree; });
}

} // namespace

struct Engine::Model
{
    explicit Model(DspMemory& memory) : voices(kBlocks), tracks(voices), dsp(memory, DspTables())
    {
    }

    Voices voices;
    Tracks tracks;
    Dsp dsp;
    uint32_t root = Track::kNone;
    SoundParams sound;

    // The BGM manager's track controllers: each controlled track's levels (JAISoundChild's), and whether the
    // controller mutes the track while its volume is 0.
    struct Controller
    {
        SoundParams params;
        bool auto_mute = false;
    };
    std::vector<Controller> controllers;
    uint64_t subframe = 0;
};

Engine::Engine(const SoundFiles& files, const Settings& settings)
    : files_(files), settings_(settings), memory_(std::make_unique<DspMemory>(files.Memory().Size()))
{
    std::copy(files.Memory().Bytes().begin(), files.Memory().Bytes().end(), memory_->Bytes().begin());
}

Engine::~Engine() = default;

std::optional<std::string> Engine::Start(uint32_t id, const TrackStates& states)
{
    model_.reset();
    const nj::SoundEntry* sound = files_.Find(id);
    if (sound == nullptr)
    {
        return std::string("the sound table has no such sound");
    }

    const SoundFiles::SequenceRef sequence = files_.SequenceOf(*sound);
    if (sequence.sequence == nullptr)
    {
        return std::string("the sound plays no sequence that the files hold");
    }

    // The memory below the waves starts again from zeros.
    std::fill(memory_->Bytes().begin(), memory_->Bytes().begin() + SoundFiles::kWaves, uint8_t{0});
    model_ = std::make_unique<Model>(*memory_);
    Model& m = *model_;

    // The DSP's start-up (DsetupTable, DsetVARAM), and the FX lines that the game sets up (JASDsp::setFXLine).
    m.dsp.Setup(static_cast<uint16_t>(Voices::kDspChannels), kBlocks, kFxSettings);
    m.dsp.SetVaram(0);
    for (std::size_t line = 0; line < settings_.fx_lines.size(); line++)
    {
        // JASDsp's start-up leaves each line off, with its settings at 0. A line whose flags are 0 stays that way.
        const FxLineSettings& fx = settings_.fx_lines[line];
        if (fx.flags == 0)
        {
            continue;
        }

        const uint32_t at = kFxSettings + static_cast<uint32_t>(line * DspFxLine::kBytes);
        const uint32_t buffer = kFxBuffers + static_cast<uint32_t>(line) * kFxBufferBytes;
        memory_->SetNative16(at, fx.flags);
        memory_->SetNative16(at + 2, std::min(fx.subframes, FxLineSettings::kMaxSubframes));
        memory_->SetNative32(at + 8, buffer);
        for (std::size_t i = 0; i < 2; i++)
        {
            memory_->SetNative16(at + 0x10 + static_cast<uint32_t>(4 * i), fx.buses[i]);
            memory_->SetNative16(at + 0x12 + static_cast<uint32_t>(4 * i), fx.levels[i]);
        }
        for (std::size_t i = 0; i < fx.filter.size(); i++)
        {
            memory_->SetNative16(at + 0x18 + static_cast<uint32_t>(2 * i), static_cast<uint16_t>(fx.filter[i]));
        }
    }

    m.voices.SetDacRate(kDacRate);
    for (const std::unique_ptr<InstrumentBank>& bank : files_.Banks())
    {
        m.tracks.SetBank(bank->Number(), bank.get());
    }
    if (loop_hook_)
    {
        m.tracks.SetLoopHook(loop_hook_);
    }

    // The sound's levels (JAISound::mixOut): the manager's volume, times the category's for an effect, times the sound
    // table's volume. The game may have written over the table's byte.
    const auto written = settings_.sound_volumes.find(sound->Id());
    const uint8_t volume = written != settings_.sound_volumes.end() ? written->second : sound->volume;
    const float table = static_cast<float>(volume) * kVolumeScale;
    float manager = 1.0f;
    if (sound->section == kMusicSection)
    {
        manager = settings_.music_volume;
    }
    else if (sound->section == kEffectSection)
    {
        manager = settings_.effect_volume * settings_.category_volumes[sound->group % Settings::kCategories];
    }
    m.sound = SoundParams{manager * table, 0.0f, 1.0f, 0.5f, 0.0f};

    m.root = m.tracks.StartSequence(*sequence.sequence, sequence.offset, &m.sound);
    if (m.root == Track::kNone)
    {
        model_.reset();
        return std::string("no root track is free");
    }

    // The sequence manager's start of a piece of music: JAISeq reserves 16 child tracks (JAISeqMgr's count, from its
    // constructor at 0x235288).
    if (sound->section == kMusicSection)
    {
        m.tracks.ReserveChildTracks(m.root, kReservedTracks);
    }

    // The BGM manager's track controllers (AudTrackController::setMuteState) act on the reserved tracks. A fade at the
    // start takes no time. A fade in also unmutes its track.
    for (std::size_t i = 0; i < states.size() && i < 16; i++)
    {
        const uint8_t state = states[i] & 7;
        Model::Controller controller;
        controller.params.volume = state == 0 ? 0.0f : 1.0f;
        controller.auto_mute = (states[i] & 8) != 0 && state <= 3;
        m.controllers.push_back(controller);

        const uint32_t track = m.tracks.ChildTrack(m.root, static_cast<uint32_t>(i));
        if (track != Track::kNone && state >= 1 && state <= 3)
        {
            m.tracks.Mute(track, state == 2);
        }
    }

    // AudBgmMgr::start then hands a rhythm piece to the rhythm system (AudRhythmMeSystem::setSeq, 0x1fbe0c). The root
    // track and child track 0 take its parser, and child track 0 is muted. That track holds the beats and plays no
    // sound. The system's number of the track (at +0x10) is 0 from its constructor (0x2361e8) on.
    if (sound->section == kMusicSection && files_.RhythmPiece(*sound))
    {
        m.tracks.UseRhythmParser(m.root);
        const uint32_t beats = m.tracks.ChildTrack(m.root, 0);
        if (beats != Track::kNone)
        {
            m.tracks.UseRhythmParser(beats);
            m.tracks.Mute(beats, true);
        }
    }

    output_.assign(2 * Dsp::kSubframeSamples, int16_t{0});

    return std::nullopt;
}

void Engine::MixOut()
{
    Model& m = *model_;
    for (std::size_t i = 0; i < m.controllers.size(); i++)
    {
        const uint32_t track = m.tracks.ChildTrack(m.root, static_cast<uint32_t>(i));
        if (track == Track::kNone)
        {
            continue;
        }

        const Model::Controller& controller = m.controllers[i];
        m.tracks.AssignSoundParams(track, &controller.params);
        if (controller.auto_mute)
        {
            m.tracks.Mute(track, controller.params.volume == 0.0f);
        }
    }
}

void Engine::RunSubframe()
{
    Model& m = *model_;
    const auto in_frame = static_cast<uint32_t>(m.subframe % kSubframesPerFrame);
    if (in_frame == 0)
    {
        MixOut();
        m.dsp.StartFrame(kSubframesPerFrame, settings_.dsp_level, kLeft, kRight);
    }

    m.dsp.RenderSubframe();
    const uint32_t offset = in_frame * 2 * Dsp::kSubframeSamples;
    for (int i = 0; i < Dsp::kSubframeSamples; i++)
    {
        const auto at = static_cast<uint32_t>(2 * i) + offset;
        output_[2 * static_cast<std::size_t>(i)] = static_cast<int16_t>(memory_->Big16(kLeft + at));
        output_[2 * static_cast<std::size_t>(i) + 1] = static_cast<int16_t>(memory_->Big16(kRight + at));
    }

    m.voices.UpdateDspChannels(*memory_, &m.tracks);
    m.tracks.SeqMain();
    m.voices.UpdateFreeRun();
    m.subframe++;
}

bool Engine::Playing() const
{
    return model_ && model_->tracks.Playing(model_->root);
}

int Engine::ActiveChannels() const
{
    if (!model_)
    {
        return 0;
    }

    int count = 0;
    for (uint32_t i = 0; i < Voices::kDspChannels; i++)
    {
        count += model_->voices.DspChannelAt(i).status == DspChannel::kActive ? 1 : 0;
    }

    return count;
}

bool Engine::Busy() const
{
    return Playing() || ActiveChannels() != 0 || FxRinging();
}

bool Engine::FxRinging() const
{
    if (!model_)
    {
        return false;
    }

    const std::span<const uint8_t> buffers = memory_->Bytes().subspan(kFxBuffers, 4 * kFxBufferBytes);
    const auto& delay = model_->dsp.Delay().samples;
    return std::any_of(buffers.begin(), buffers.end(), [](uint8_t b) { return b != 0; }) ||
           std::any_of(delay.begin(), delay.end(), [](int16_t s) { return s != 0; });
}

uint32_t Engine::MissingBankNotes() const
{
    return model_ ? model_->tracks.MissingBankNotes() : 0;
}

void Engine::SetLoopHook(LoopHook hook)
{
    loop_hook_ = std::move(hook);
    if (model_)
    {
        model_->tracks.SetLoopHook(loop_hook_);
    }
}

std::vector<uint32_t> Engine::TreeOrder() const
{
    std::vector<uint32_t> order;
    if (!model_ || model_->root == Track::kNone)
    {
        return order;
    }

    const Tracks& tracks = model_->tracks;
    std::vector<uint32_t> pending{model_->root};
    while (!pending.empty())
    {
        const uint32_t index = pending.back();
        pending.pop_back();
        const Track& track = tracks.TrackAt(index);
        if (track.status == Track::kFree)
        {
            continue;
        }

        order.push_back(index);
        for (std::size_t slot = track.children.size(); slot-- > 0;)
        {
            if (track.children[slot] != Track::kNone)
            {
                pending.push_back(track.children[slot]);
            }
        }
    }

    return order;
}

bool Engine::PlayedNotes(uint32_t track) const
{
    return model_ && model_->tracks.PlayedNotes(track);
}

bool Engine::QuietPass(uint32_t track) const
{
    return model_ && model_->tracks.QuietPass(track);
}

bool Engine::Held() const
{
    if (!model_)
    {
        return false;
    }

    // A track that waits for its first note holds the count back.
    const Tracks& tracks = model_->tracks;
    for (const uint32_t index : TreeOrder())
    {
        if (!HasOpenChildren(tracks, index) && !tracks.PlayedNotes(index) && !tracks.Looped(index))
        {
            return false;
        }
    }

    bool held = false;
    for (uint32_t i = 0; i < Voices::kChannels; i++)
    {
        const Channel& channel = model_->voices.ChannelAt(i);
        if (!channel.alive || channel.status == Channel::kStop)
        {
            continue;
        }

        if (!NoteHeld(channel))
        {
            return false;
        }

        held = true;
    }

    return held;
}

bool Engine::Moving() const
{
    if (!model_)
    {
        return false;
    }

    for (const uint32_t index : TreeOrder())
    {
        const Track& track = model_->tracks.TrackAt(index);
        if (std::any_of(track.params.begin(), track.params.end(), [](const TrackParam& p) { return p.count != 0; }))
        {
            return true;
        }
    }

    for (uint32_t i = 0; i < Voices::kChannels; i++)
    {
        const Channel& channel = model_->voices.ChannelAt(i);
        if (channel.alive && channel.key_sweep_count != 0)
        {
            return true;
        }
    }

    return false;
}

bool Engine::WaitsForNotes(uint32_t track) const
{
    return model_ && model_->tracks.TrackAt(track).wait_note_finish;
}

bool Engine::WaitsForEver(uint32_t index) const
{
    if (!WaitsForNotes(index))
    {
        return false;
    }

    const Track& track = model_->tracks.TrackAt(index);
    bool held = false;
    for (uint32_t i = 0; i < track.mgr_count; i++)
    {
        for (const uint32_t channel : track.mgrs[i].channels)
        {
            if (channel == Channel::kNone)
            {
                continue;
            }

            if (!NoteHeld(model_->voices.ChannelAt(channel)))
            {
                return false;
            }

            held = true;
        }
    }

    return held;
}

bool Engine::Waiting() const
{
    if (!model_)
    {
        return false;
    }

    const std::vector<uint32_t> order = TreeOrder();
    return !order.empty() && std::all_of(order.begin(), order.end(), [this](uint32_t index)
                                         { return HasOpenChildren(model_->tracks, index) || WaitsForNotes(index); });
}

Engine::WavePlace Engine::DspChannelPlace(uint32_t index) const
{
    WavePlace place;
    if (!model_ || index >= Voices::kDspChannels)
    {
        return place;
    }

    // An oscillator's channel plays no wave.
    const DspChannel& dsp_channel = model_->voices.DspChannelAt(index);
    if (dsp_channel.status != DspChannel::kActive || dsp_channel.channel == Channel::kNone ||
        model_->voices.ChannelAt(dsp_channel.channel).type != Channel::kWave)
    {
        return place;
    }

    const uint32_t block = kBlocks + index * static_cast<uint32_t>(DspChannelBlock::kBytes);
    place.playing = true;
    place.loops = memory_->Native16(block + static_cast<uint32_t>(DspChannelBlock::kLoops)) != 0;
    place.position =
        -static_cast<int64_t>(memory_->Native32(block + static_cast<uint32_t>(DspChannelBlock::kRemaining)));

    return place;
}

} // namespace hsf::jaudio
