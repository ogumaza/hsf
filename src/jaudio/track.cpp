// SPDX-License-Identifier: MIT

#include "jaudio/track.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

#include "jaudio/calc.h"
#include "jaudio/iir_table.h"

namespace hsf::jaudio
{

namespace
{

// main's constants, by their bits.
const float kTempoScale = std::bit_cast<float>(0x3faaaaabu); // 1.3333334: ticks per sub-frame per tempo × timebase
const float kThird = std::bit_cast<float>(0x3eaaaaabu);      // 0.33333334: the pitch's octaves per bend semitone

// JASDriver's sub-frames per frame (sSubFrames).
constexpr uint32_t kSubFrames = 7;

// The envelopes that a new track's notes take: none for the volume (JASTrack's sEnvOsc) and none for the pitch
// (sPitchEnvOsc). A note then keeps its instrument's.
const OscData kEnvOsc{0, 1.0f, {}, {}, 1.0f, 0.0f};
const OscData kPitchEnvOsc{1, 1.0f, {}, {}, 1.0f, 0.0f};

// JASTrack's OSC_ENV, the volume's envelope of a note on the DSP's oscillator: no attack, and a release of 10 to
// silence along the square curve.
constexpr std::array<OscPoint, 2> kOscEnvRelease = {OscPoint{1, 10, 0}, OscPoint{15, 0, 0}};
const OscData kOscEnv{0, 1.0f, {}, kOscEnvRelease, 1.0f, 0.0f};

// The rate of the DSP's oscillators' waveforms: the step of a note's pitch at its base key.
constexpr float kOscillatorRate = 16736.015625f;

// The key of a beat for the rhythm parser (AudRhythmSeqParser's first key of a rhythm type, 0x18 from its
// constructor at 0x2363f0).
constexpr uint32_t kBeatKey = 0x18;

// The registers that JASRegisterParam keeps, and the flag register.
constexpr uint32_t kRegisters = 14;
constexpr uint32_t kRegFlag = 3;

// A channel's owner: its track and the track's channel manager.
uint32_t Owner(uint32_t track, uint32_t mgr)
{
    return track * 4 + mgr;
}

// The parser's tables of commands: each one's argument count and widths, two bits each from the first argument up (0
// a byte, 1 16 bits, 2 24 bits, 3 a register named by a byte). JASSeqParser's sCmdInfo from 0xa0 on, and its
// sExtCmdInfo for 0xb0's commands. A command without an action reads its arguments and does nothing.
struct CommandInfo
{
    uint8_t count = 0;
    uint16_t widths = 0;
};

constexpr std::array<CommandInfo, 0x60> kCommands = []
{
    std::array<CommandInfo, 0x60> c{};
    const auto set = [&c](uint32_t command, uint8_t count, uint16_t widths)
    {
        c[command - 0xa0] = {count, widths};
    };
    set(0xb1, 3, 0x0);
    set(0xb2, 1, 0x0);
    set(0xb3, 4, 0x40);
    set(0xb4, 1, 0x0);
    set(0xb8, 2, 0x0);
    set(0xb9, 2, 0x4);
    set(0xba, 3, 0x10);
    set(0xbb, 3, 0x14);
    set(0xc1, 2, 0x8);
    set(0xc2, 1, 0x0);
    set(0xc3, 1, 0x2);
    set(0xc4, 2, 0x8);
    set(0xc6, 1, 0x0);
    set(0xc7, 1, 0x2);
    set(0xc8, 2, 0x8);
    set(0xc9, 2, 0xb);
    set(0xca, 2, 0xb);
    set(0xcb, 1, 0x1);
    set(0xd0, 2, 0x0);
    set(0xd1, 2, 0xc);
    set(0xd2, 1, 0x0);
    set(0xd3, 1, 0x0);
    set(0xd4, 2, 0xc);
    set(0xd5, 2, 0xc);
    set(0xd6, 2, 0x0);
    set(0xd7, 2, 0x0);
    set(0xd8, 2, 0x4);
    set(0xd9, 3, 0x30);
    set(0xda, 3, 0x10);
    set(0xdb, 2, 0x0);
    set(0xdc, 4, 0xe0);
    set(0xe0, 1, 0x1);
    set(0xe1, 1, 0x1);
    set(0xe2, 1, 0x0);
    set(0xe3, 1, 0x0);
    set(0xe7, 2, 0x4);
    set(0xe8, 2, 0x8);
    set(0xe9, 5, 0x155);
    set(0xea, 2, 0x4);
    set(0xeb, 1, 0x0);
    set(0xec, 4, 0x55);
    set(0xed, 1, 0x2);
    set(0xf1, 1, 0x0);
    set(0xf3, 1, 0x2);
    set(0xf4, 1, 0x1);
    set(0xf5, 1, 0x1);
    set(0xf8, 2, 0x4);
    set(0xf9, 1, 0x1);
    return c;
}();

// A parameter's value from a byte, as paramE gives it: the byte in the high half, and below it twice the byte when
// the byte's top bit is clear. Then a fraction of 32767.
float ByteParam(uint32_t value)
{
    const uint32_t low = (value & 0x80) != 0 ? 0 : value << 1;

    return static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(low | value << 8))) / 32767.0f;
}

float HalfParam(uint32_t value)
{
    return static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(value))) / 32767.0f;
}

} // namespace

std::span<const OscPoint> Sequence::Table(uint32_t offset) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    auto [table, added] = tables_.try_emplace(offset);
    if (added)
    {
        for (uint64_t at = offset; at + 6 <= bytes_.size(); at += 6)
        {
            const auto at32 = static_cast<uint32_t>(at);
            table->second.push_back({static_cast<int16_t>(Native16(at32)), static_cast<int16_t>(Native16(at32 + 2)),
                                     static_cast<int16_t>(Native16(at32 + 4))});
        }
    }

    return table->second;
}

Tracks::Tracks(Voices& voices) : voices_(voices)
{
    tracks_.resize(kPoolTracks + kRootTracks);
    notes_.resize(tracks_.size());
    loop_notes_.resize(tracks_.size());
    looped_.resize(tracks_.size());
    quiet_.resize(tracks_.size());
    for (std::size_t i = kPoolTracks; i-- > 0;)
    {
        tracks_[i].in_pool = true;
        free_tracks_.push_back(static_cast<uint32_t>(i));
    }
}

void Tracks::SetBank(uint32_t number, const InstrumentBank* bank)
{
    if (number < banks_.size())
    {
        banks_[number] = bank;
    }
}

uint32_t Tracks::StartSequence(const Sequence& sequence, uint32_t offset, const SoundParams* sound)
{
    for (uint32_t index = kPoolTracks; index < tracks_.size(); index++)
    {
        Track& track = tracks_[index];
        if (track.allocated && track.status == Track::kRun)
        {
            continue;
        }

        // JAISeq: the track's construction, its channel manager's levels from the sound, the reader at the
        // sequence's start, and JASTrack::startSeq.
        Construct(index);
        track.allocated = true;
        track.mgrs[0].sound_params = sound;
        track.sequence = &sequence;
        track.cursor = offset;
        track.depth = 0;
        track.stack.fill(0);
        track.loop_counts.fill(0);
        list_.push_back(index);
        track.status = Track::kRun;

        return index;
    }

    return Track::kNone;
}

void Tracks::ReserveChildTracks(uint32_t root, uint32_t count)
{
    // Each track comes from the pool with the flag of a child that its parent frees. connectChild puts the track in a
    // slot that's empty.
    const auto reserve = [this](uint32_t parent, uint32_t slot)
    {
        const uint32_t child = AllocTrack();
        if (child == Track::kNone)
        {
            return Track::kNone;
        }

        Construct(child);
        tracks_[child].flags |= Track::kOwned;
        if (tracks_[parent].children[slot] == Track::kNone)
        {
            tracks_[child].parent = parent;
            tracks_[parent].children[slot] = child;
        }

        return child;
    };

    for (uint32_t i = 0; i < 2; i++)
    {
        const uint32_t child = reserve(root, i);
        for (uint32_t slot = 0; child != Track::kNone && slot < 16; slot++)
        {
            if (i * 16 + slot < count)
            {
                reserve(child, slot);
            }
        }
    }
}

void Tracks::UseRhythmParser(uint32_t index)
{
    tracks_[index].rhythm_parser = true;
}

void Tracks::Construct(uint32_t index)
{
    Track& track = tracks_[index];
    track.flags = 0;
    track.mgr_count = 1;
    track.status = Track::kFree;
    track.mgrs[0] = ChannelMgr{};
    track.mgrs[0].exists = true;
    for (std::size_t i = 1; i < track.mgrs.size(); i++)
    {
        track.mgrs[i] = ChannelMgr{};
    }

    Init(index);
}

void Tracks::Init(uint32_t index)
{
    Track& track = tracks_[index];
    notes_[index] = 0;
    loop_notes_[index] = 0;
    looped_[index] = false;
    quiet_[index] = false;

    // JASSeqCtrl::init and the reader's, the ports and the registers.
    track.sequence = nullptr;
    track.cursor = 0;
    track.depth = 0;
    track.stack.fill(0);
    track.loop_counts.fill(0);
    track.timer = 0;
    track.interrupted = false;
    track.swap = 0;
    track.interrupt_table = 0;
    track.pending = 0;
    track.mask = 0;
    track.interrupt_timer = 0;
    track.wait_note_finish = false;
    track.timer_count = 0;
    track.timer_reload = 0;
    track.rhythm_parser = false;
    track.import_flags = 0;
    track.export_flags = 0;
    track.ports.fill(0);
    track.registers.fill(0);

    track.params = {TrackParam{1.0f, 1.0f, 0}, TrackParam{}, TrackParam{},
                    TrackParam{0.5f, 0.5f, 0}, TrackParam{}, TrackParam{}};
    track.adsr = {OscPoint{0, 0, 0x7fff}, OscPoint{0, 0, 0x7fff}, OscPoint{0, 0, 0}, OscPoint{14, 0, 0}};
    track.parent = Track::kNone;
    track.oscillators = {kEnvOsc, kPitchEnvOsc};
    track.oscillator_tables = {false, false};
    track.children.fill(Track::kNone);

    // The first channel manager starts again without channels. The others go.
    ChannelMgr& first = track.mgrs[0];
    first.params = ChannelParams{};
    first.channels.fill(Channel::kNone);
    first.sound_params = nullptr;
    track.mgr_count = 1;
    for (std::size_t i = 1; i < track.mgrs.size(); i++)
    {
        track.mgrs[i] = ChannelMgr{};
    }

    track.vibrato_delay = 0;
    track.tremolo_delay = 0;
    track.playtime = 1.0f;
    track.vibrato_depth = 0.0f;
    track.vibrato_pitch = 1.0f;
    track.tremolo_depth = 0.0f;
    track.tremolo_pitch = 1.0f;
    track.pan_power = 1.0f;
    track.direct_release = 0;
    track.tempo = 120;
    track.timebase = 48;
    track.tempo_rate = 1.0f;
    track.skip_samples = 0;
    track.interval = track.tempo_rate * (7680.0f / voices_.DacRate());
    track.transpose = 0;
    track.latest_key = 60;
    track.bank = 0;
    track.program = 0xf0;
    track.bend_sense = 12;
    track.note_on_priority = 0x40;
    track.release_priority = 0;
    track.gate_rate = 100;
    track.mix_configs = {0x150, 0x210, 0x352, 0x412, 0, 0};
    track.fir.fill(0);
    track.fir[0] = 0x7fff;
    track.iir.fill(0);
    track.iir[0] = 0x7fff;
    track.filter_mode = 0;
    track.flags = static_cast<uint8_t>((track.flags & 0x80) | Track::kInvalid | Track::kGateByTempo);
    track.status = Track::kFree;
}

uint32_t Tracks::AllocTrack()
{
    if (free_tracks_.empty())
    {
        return Track::kNone;
    }

    const uint32_t index = free_tracks_.back();
    free_tracks_.pop_back();
    tracks_[index].allocated = true;

    return index;
}

void Tracks::FreeTrack(uint32_t index)
{
    tracks_[index].allocated = false;
    if (tracks_[index].in_pool)
    {
        free_tracks_.push_back(index);
    }
}

uint32_t Tracks::Root(uint32_t index) const
{
    while (tracks_[index].parent != Track::kNone)
    {
        index = tracks_[index].parent;
    }

    return index;
}

bool Tracks::Muted(uint32_t index) const
{
    for (; index != Track::kNone; index = tracks_[index].parent)
    {
        if ((tracks_[index].flags & Track::kMute) != 0)
        {
            return true;
        }
    }

    return false;
}

int32_t Tracks::TransposeSum(uint32_t index) const
{
    int32_t sum = 0;
    for (; index != Track::kNone; index = tracks_[index].parent)
    {
        sum += tracks_[index].transpose;
    }

    return sum;
}

void Tracks::SeqMain()
{
    // The list drops a sequence that ends. The game reads each node's next before it runs the node.
    std::vector<uint32_t> list = list_;
    for (const uint32_t index : list)
    {
        Track& track = tracks_[index];
        bool end = false;
        while (!end && track.playtime >= 1.0f)
        {
            track.playtime = track.playtime + -1.0f;
            if (TickProc(index) < 0)
            {
                end = true;
            }
        }
        if (end)
        {
            UpdateSeq(index, false, 1.0f);
            Close(index);
            std::erase(list_, index);
            if ((track.flags & Track::kOwned) != 0)
            {
                FreeTrack(index);
            }
            continue;
        }

        track.playtime = track.playtime + track.interval;
        UpdateSeq(index, false, 1.0f);
    }
}

int32_t Tracks::TickProc(uint32_t index)
{
    Track& track = tracks_[index];
    const int32_t result = SeqTick(index);
    for (TrackParam& param : track.params)
    {
        if (param.count != 0)
        {
            const float step = (param.target - param.value) / static_cast<float>(param.count);
            param.count--;
            param.value = param.value + step;
        }
    }
    track.flags |= Track::kInvalid;
    if (result < 0)
    {
        return -1;
    }

    for (uint32_t& child : track.children)
    {
        if (child == Track::kNone || tracks_[child].status != Track::kRun)
        {
            continue;
        }

        if (TickProc(child) < 0)
        {
            UpdateSeq(Root(index), false, 1.0f);
            Close(child);
            if ((tracks_[child].flags & Track::kOwned) != 0)
            {
                FreeTrack(child);
                child = Track::kNone;
            }
        }
    }

    return 0;
}

int32_t Tracks::SeqTick(uint32_t index)
{
    Track& track = tracks_[index];
    if (track.sequence == nullptr)
    {
        return 0;
    }

    // Interrupt 6 comes every tick, and interrupt 5 when the timer's count runs out.
    if ((track.mask & 0x40) != 0)
    {
        track.pending |= 0x40;
    }
    if (track.timer_count != 0)
    {
        track.timer_count--;
        if (track.timer_count == 0)
        {
            if ((track.mask & 0x20) != 0)
            {
                track.pending |= 0x20;
            }
            if (track.interrupt_timer == 0)
            {
                track.timer_count = track.timer_reload;
            }
            else if (--track.interrupt_timer != 0)
            {
                track.timer_count = track.timer_reload;
            }
        }
    }

    if (track.wait_note_finish)
    {
        if (!CheckNoteStop(track, 0))
        {
            return 0;
        }
        track.wait_note_finish = false;
    }

    if (track.timer > 0)
    {
        track.timer--;
    }
    CheckInterrupt(track);
    while ((track.interrupted || track.timer == 0) && !track.wait_note_finish)
    {
        if (Parse(index) < 0)
        {
            return -1;
        }
    }

    return 0;
}

void Tracks::CheckInterrupt(Track& track)
{
    if (track.interrupted)
    {
        return;
    }
    const uint32_t ready = track.pending & track.mask;
    if (ready == 0)
    {
        return;
    }

    const uint32_t i = static_cast<uint32_t>(std::countr_zero(ready));
    track.pending = static_cast<uint16_t>(track.pending & ~(1u << i));
    const uint32_t target = Be24(track, track.interrupt_table + 3 * i);
    track.interrupted = true;
    track.swap = track.cursor;
    track.cursor = target;
}

uint32_t Tracks::Be24(const Track& track, uint32_t offset) const
{
    const Sequence& s = *track.sequence;

    return static_cast<uint32_t>(s.U8(offset)) << 16 | static_cast<uint32_t>(s.U8(offset + 1)) << 8 | s.U8(offset + 2);
}

bool Tracks::CheckNoteStop(const Track& track, uint32_t note) const
{
    for (uint32_t i = 0; i < track.mgr_count; i++)
    {
        const ChannelMgr& mgr = track.mgrs[i];
        if (mgr.exists && mgr.channels[note] != Channel::kNone)
        {
            return false;
        }
    }

    return true;
}

void Tracks::UpdateSeq(uint32_t index, bool update, float num)
{
    Track& track = tracks_[index];
    update = update || (track.flags & Track::kInvalid) != 0;
    track.flags &= static_cast<uint8_t>(~Track::kInvalid);
    if (update)
    {
        UpdateTrack(index, num);
    }

    const float child_num = track.pan_power * num;
    for (const uint32_t child : track.children)
    {
        if (child != Track::kNone && tracks_[child].status == Track::kRun)
        {
            UpdateSeq(child, update, child_num);
        }
    }
}

void Tracks::UpdateTempo(Track& track)
{
    if (track.parent != Track::kNone)
    {
        const Track& parent = tracks_[track.parent];
        track.tempo = parent.tempo;
        track.timebase = parent.timebase;
        track.interval = parent.interval;
        return;
    }

    const uint32_t ticks = uint32_t{track.tempo} * track.timebase;
    const float scaled = static_cast<float>(static_cast<int32_t>(ticks)) * kTempoScale;
    track.interval = track.tempo_rate * (scaled / voices_.DacRate());
}

void Tracks::UpdateTrack(uint32_t index, float num)
{
    Track& track = tracks_[index];
    UpdateTempo(track);

    // Each channel manager's levels: the track's, the sound's and the parent's.
    for (uint32_t i = 0; i < track.mgr_count; i++)
    {
        ChannelMgr& mgr = track.mgrs[i];
        if (!mgr.exists)
        {
            continue;
        }

        float volume = track.params[0].value * track.params[0].value;
        float pan = (track.params[3].value + -0.5f) * track.pan_power;
        float fxmix = track.params[2].value;
        float dolby = track.params[4].value;
        float pitch = 1.0f;
        if (const SoundParams* sound = mgr.sound_params)
        {
            fxmix = fxmix + sound->fxmix;
            volume = volume * sound->volume;
            dolby = dolby + sound->dolby;
            pitch = sound->pitch;
            pan = pan + (sound->pan + -0.5f);
        }
        pan = pan * num;
        float octaves = (track.params[1].value * static_cast<float>(track.bend_sense)) * kThird;
        if (track.parent != Track::kNone)
        {
            const Track& parent = tracks_[track.parent];
            const ChannelMgr& from = parent.mgrs[i].exists ? parent.mgrs[i] : parent.mgrs[0];
            mgr.params.volume = volume * from.params.volume;
            mgr.params.pitch = pitch * from.params.pitch;
            pan = pan + (from.params.pan + -0.5f);
            mgr.params.fxmix = fxmix + from.params.fxmix;
            mgr.params.dolby = dolby + from.params.dolby;
            octaves = octaves + from.params.octaves;
        }
        else
        {
            mgr.params.volume = volume;
            mgr.params.pitch = pitch;
            mgr.params.fxmix = fxmix;
            mgr.params.dolby = dolby;
        }
        mgr.params.octaves = octaves;
        mgr.params.pan = pan + 0.5f;
    }
}

void Tracks::Close(uint32_t index)
{
    Track& track = tracks_[index];
    for (uint32_t& child : track.children)
    {
        if (child == Track::kNone)
        {
            continue;
        }

        Close(child);
        if ((tracks_[child].flags & Track::kOwned) != 0)
        {
            FreeTrack(child);
            child = Track::kNone;
        }
    }

    for (uint32_t i = 0; i < track.mgr_count; i++)
    {
        if (track.mgrs[i].exists)
        {
            ReleaseAll(track.mgrs[i]);
        }
    }

    track.parent = Track::kNone;
    track.status = Track::kStop;
}

void Tracks::ReleaseAll(ChannelMgr& mgr)
{
    for (uint32_t& channel : mgr.channels)
    {
        if (channel != Channel::kNone)
        {
            voices_.Release(channel, 0);
            voices_.Free(channel);
            channel = Channel::kNone;
        }
    }
}

uint32_t Tracks::ReadMidiValue(Track& track)
{
    const Sequence& s = *track.sequence;
    uint32_t value = s.U8(track.cursor++);
    if ((value & 0x80) == 0)
    {
        return value;
    }

    value &= 0x7f;
    for (int i = 0; i < 3; i++)
    {
        const uint32_t byte = s.U8(track.cursor++);
        value = value << 7 | (byte & 0x7f);
        if ((byte & 0x80) == 0)
        {
            return value;
        }
        if (i == 2)
        {
            return 0; // a fourth byte with its top bit set
        }
    }

    return value;
}

int32_t Tracks::Parse(uint32_t index)
{
    Track& track = tracks_[index];
    const Sequence& s = *track.sequence;
    const uint32_t command = s.U8(track.cursor++);
    if ((command & 0x80) == 0)
    {
        // A note: its flags (the note id in the low three bits) and its velocity. A gate note (id 0) has its time.
        const uint32_t flags = s.U8(track.cursor);
        const uint32_t velocity = s.U8(track.cursor + 1);
        track.cursor += 2;
        const uint32_t note = flags & 7;
        if (note == 0)
        {
            const uint32_t time = ReadMidiValue(track);
            NoteOnGate(index, command, velocity, time, flags);
        }
        else
        {
            // The rhythm parser's execNoteOnMidi (0x10d540). AudBgmTempoAdjuster gives a beat a rate of 1 in a piece
            // that plays beside no stream. The tempo then goes to the root track unchanged.
            if (track.rhythm_parser && command == kBeatKey)
            {
                SetTempo(tracks_[Root(index)], static_cast<uint32_t>(static_cast<int32_t>(rhythm_tempo_)));
            }
            NoteOn(index, note, command, velocity);
        }

        return 0;
    }

    if ((command & 0xf0) == 0x90)
    {
        // The next command with some of its arguments from registers: a bit for each, from the top bit down.
        const uint32_t count = (command & 7) + 1;
        const uint32_t bits = s.U8(track.cursor++);
        uint32_t mask = 0;
        for (uint32_t i = 0; i < count; i++)
        {
            if ((bits & (0x80u >> i)) != 0)
            {
                mask |= 3u << (2 * i);
            }
        }
        const uint32_t next = s.U8(track.cursor++);
        return ParseCommand(index, next, mask);
    }

    if ((command & 0xf0) == 0x80)
    {
        if ((command & 7) != 0)
        {
            NoteOff(index, command & 7, 0);
        }

        return 0;
    }

    // The rhythm parser's parse (0x12ca20) keeps a tempo command's tempo before the command runs.
    if (track.rhythm_parser && command == 0xe0)
    {
        rhythm_tempo_ = static_cast<float>(static_cast<uint32_t>(s.U8(track.cursor)) << 8 | s.U8(track.cursor + 1));
    }

    return ParseCommand(index, command, 0);
}

int32_t Tracks::ParseCommand(uint32_t index, uint32_t command, uint32_t register_mask)
{
    Track& track = tracks_[index];
    const Sequence& s = *track.sequence;
    command &= 0xff;
    if (command == 0xb0)
    {
        // JASSeqParser's extended commands: the next byte names one. None reads arguments, and only 01 has an
        // action, a print of the track's state.
        track.cursor++;
        return 0;
    }

    const CommandInfo info = command >= 0xa0 ? kCommands[command - 0xa0] : CommandInfo{};

    Args args;
    uint32_t widths = info.widths | register_mask;
    for (uint32_t i = 0; i < info.count && i < args.values.size(); i++)
    {
        uint32_t value = 0;
        switch (widths & 3)
        {
        case 0:
            value = s.U8(track.cursor);
            track.cursor += 1;
            break;
        case 1:
            value = static_cast<uint32_t>(s.U8(track.cursor)) << 8 | s.U8(track.cursor + 1);
            track.cursor += 2;
            break;
        case 2:
            value = Be24(track, track.cursor);
            track.cursor += 3;
            break;
        default:
            value = ReadReg(track, s.U8(track.cursor));
            track.cursor += 1;
            break;
        }
        widths >>= 2;
        args.values[i] = value;
    }

    return Execute(index, command, args);
}

uint32_t Tracks::RegisterRead(const Track& track, uint32_t reg)
{
    const std::array<uint16_t, kRegisters>& r = track.registers;
    if (reg < kRegisters)
    {
        return r[reg];
    }

    switch (reg)
    {
    case 0x20:
        return (r[0] & 0xffu) << 8 | (r[1] & 0xffu);
    case 0x21:
        return uint32_t{r[4]} << 16 | r[5];
    case 0x22:
        return r[4] >> 8;
    case 0x23:
        return r[4] & 0xffu;
    case 0x24:
        return r[5] >> 8;
    case 0x25:
        return r[5] & 0xffu;
    case 0x26:
        return uint32_t{r[6]} << 16 | r[7];
    case 0x27:
        return uint32_t{r[8]} << 16 | r[9];
    // The game's read (0x7548c in Super Mario Galaxy's main) returns registers 10 and 11 for both 0x28 and 0x29. A
    // write of 0x29 sets registers 12 and 13.
    case 0x28:
    case 0x29:
        return uint32_t{r[10]} << 16 | r[11];
    default:
        return 0;
    }
}

void Tracks::RegisterWrite(Track& track, uint32_t reg, uint32_t value)
{
    std::array<uint16_t, kRegisters>& r = track.registers;
    if (reg < kRegisters)
    {
        r[reg] = static_cast<uint16_t>(reg <= 2 ? value & 0xff : value);
        return;
    }

    const auto pair = [&r, value](std::size_t high)
    {
        r[high] = static_cast<uint16_t>(value >> 16);
        r[high + 1] = static_cast<uint16_t>(value);
    };
    switch (reg)
    {
    case 0x20:
        r[0] = static_cast<uint16_t>((value >> 8) & 0xff);
        r[1] = static_cast<uint16_t>(value & 0xff);
        break;
    case 0x21:
        pair(4);
        break;
    case 0x22:
        r[4] = static_cast<uint16_t>((r[4] & 0x00ff) | (value & 0xff) << 8);
        break;
    case 0x23:
        r[4] = static_cast<uint16_t>((r[4] & 0xff00) | (value & 0xff));
        break;
    case 0x24:
        r[5] = static_cast<uint16_t>((r[5] & 0x00ff) | (value & 0xff) << 8);
        break;
    case 0x25:
        r[5] = static_cast<uint16_t>((r[5] & 0xff00) | (value & 0xff));
        break;
    case 0x26:
        pair(6);
        break;
    case 0x27:
        pair(8);
        break;
    case 0x28:
        pair(10);
        break;
    case 0x29:
        pair(12);
        break;
    default:
        break;
    }
}

uint32_t Tracks::ReadReg(const Track& track, uint32_t reg) const
{
    if (reg <= 0x3f)
    {
        return RegisterRead(track, reg);
    }
    if (reg >= 0x40 && reg < 0x50)
    {
        return track.ports[reg - 0x40];
    }

    switch (reg)
    {
    case 0x60:
        {
            // The children that play.
            uint32_t bits = 0;
            for (uint32_t i = 0; i < track.children.size(); i++)
            {
                const uint32_t child = track.children[i];
                if (child != Track::kNone && tracks_[child].status == Track::kRun)
                {
                    bits |= 1u << i;
                }
            }

            return bits;
        }
    case 0x61:
        return track.depth != 0 ? track.loop_counts[track.depth - 1] : 0;
    case 0x62:
        return track.timebase;
    case 0x63:
        return static_cast<uint32_t>(static_cast<int32_t>(track.transpose));
    case 0x64:
        return track.bend_sense;
    case 0x65:
        return track.gate_rate;
    case 0x66:
        return track.skip_samples;
    case 0x67:
        return track.bank;
    case 0x68:
        return track.program;
    case 0x69:
        return ToUint32(track.pan_power * 32767.0f);
    case 0x6a:
        // The game's read (0x7518c in Super Mario Galaxy's main) ORs the two priorities without a shift. A write of
        // 0x6a puts the release priority in the high byte.
        return uint32_t{track.note_on_priority} | track.release_priority;
    case 0x6b:
        return track.note_on_priority;
    case 0x6c:
        return track.release_priority;
    case 0x6d:
        return track.direct_release;
    case 0x6e:
        return ToUint32(track.vibrato_depth * 1524.0f);
    case 0x6f:
        return ToUint32(track.vibrato_depth * 12192.0f);
    case 0x70:
        return ToUint32(track.tremolo_depth * 256.0f);
    case 0x71:
        return ToUint32(track.vibrato_pitch * 64.0f);
    case 0x72:
        return ToUint32(track.tremolo_pitch * 64.0f);
    case 0x73:
        return track.vibrato_delay;
    case 0x74:
        return track.tremolo_delay;
    default:
        return 0;
    }
}

void Tracks::WriteReg(uint32_t index, uint32_t reg, uint32_t value)
{
    // Every write sets the flag register too.
    Track& track = tracks_[index];
    RegisterWrite(track, kRegFlag, value);
    if (reg <= 0x3f)
    {
        RegisterWrite(track, reg, value);
        return;
    }
    if (reg >= 0x40 && reg < 0x50)
    {
        track.ports[reg - 0x40] = static_cast<uint16_t>(value);
        return;
    }

    switch (reg)
    {
    case 0x62:
        track.timebase = static_cast<uint16_t>(value);
        track.flags |= Track::kInvalid;
        UpdateTempo(track);
        break;
    case 0x63:
        track.transpose = static_cast<int8_t>(static_cast<uint8_t>(value));
        break;
    case 0x64:
        track.bend_sense = static_cast<uint8_t>(value);
        break;
    case 0x65:
        track.gate_rate = static_cast<uint8_t>(value);
        break;
    case 0x66:
        track.skip_samples = value;
        break;
    case 0x67:
        track.bank = static_cast<uint16_t>(value & 0xff);
        break;
    case 0x68:
        track.program = static_cast<uint16_t>(value & 0xff);
        break;
    case 0x69:
        track.pan_power = static_cast<float>(value) / 32767.0f;
        break;
    case 0x6a:
        track.note_on_priority = static_cast<uint8_t>(value);
        track.release_priority = static_cast<uint8_t>(value >> 8);
        break;
    case 0x6b:
        track.note_on_priority = static_cast<uint8_t>(value);
        break;
    case 0x6c:
        track.release_priority = static_cast<uint8_t>(value);
        break;
    case 0x6d:
        track.direct_release = static_cast<uint16_t>(value);
        break;
    case 0x6e:
        track.vibrato_depth = static_cast<float>(value) / 1524.0f;
        break;
    case 0x6f:
        track.vibrato_depth = static_cast<float>(value) / 12192.0f;
        break;
    case 0x70:
        track.tremolo_depth = static_cast<float>(value) * (1.0f / 256.0f);
        break;
    case 0x71:
        track.vibrato_pitch = static_cast<float>(value) * (1.0f / 64.0f);
        break;
    case 0x72:
        track.tremolo_pitch = static_cast<float>(value) * (1.0f / 64.0f);
        break;
    case 0x73:
        track.vibrato_delay = static_cast<uint16_t>(value);
        break;
    case 0x74:
        track.tremolo_delay = static_cast<uint16_t>(value);
        break;
    default:
        break;
    }
}

bool Tracks::Condition(const Track& track, uint32_t condition)
{
    const uint32_t flag = track.registers[kRegFlag];
    switch (condition)
    {
    case 0:
        return true;
    case 1:
        return flag == 0;
    case 2:
        return flag != 0;
    case 3:
        return flag == 1;
    case 4:
        return (flag & 0x8000) != 0;
    case 5:
        return (flag & 0x8000) == 0;
    default:
        return false;
    }
}

void Tracks::JumpBack(uint32_t index, uint32_t target)
{
    // The cursor has moved past the jump. A jump to the jump itself or before it goes back.
    const Track& track = tracks_[index];
    if (!loop_hook_ || track.depth != 0 || target >= track.cursor)
    {
        return;
    }

    const bool played = notes_[index] != loop_notes_[index];
    loop_notes_[index] = notes_[index];
    looped_[index] = true;
    quiet_[index] = !played;
    loop_hook_(index, target, played);
}

uint32_t Tracks::ChildTrack(uint32_t root, uint32_t id) const
{
    const uint32_t child = tracks_[root].children[(id >> 4) & 15];
    if (child == Track::kNone)
    {
        return Track::kNone;
    }

    return tracks_[child].children[id & 15];
}

void Tracks::AssignSoundParams(uint32_t index, const SoundParams* params)
{
    tracks_[index].mgrs[0].sound_params = params;
}

void Tracks::Mute(uint32_t index, bool mute)
{
    Track& track = tracks_[index];
    track.flags = static_cast<uint8_t>(mute ? track.flags | Track::kMute : track.flags & ~Track::kMute);
}

int32_t Tracks::Execute(uint32_t index, uint32_t command, const Args& a)
{
    Track& track = tracks_[index];
    const Sequence& s = *track.sequence;
    const auto call = [&track](uint32_t offset)
    {
        if (track.depth < track.stack.size())
        {
            track.stack[track.depth++] = track.cursor;
            track.cursor = offset;
        }
    };
    const auto ret = [&track]
    {
        if (track.depth != 0)
        {
            track.cursor = track.stack[--track.depth];
        }
    };
    const auto child_of = [&track](uint32_t value)
    {
        return track.children[(value >> 4) & 15];
    };

    switch (command)
    {
    case 0xb1:
        NoteOn(index, a[1], a[0], a[2]);
        break;
    case 0xb2:
        NoteOff(index, a[0], 0);
        break;
    case 0xb3:
        NoteOnGate(index, a[1], a[2], a[3], a[0]);
        break;
    case 0xb4:
        if (a[0] <= 0xff)
        {
            SetLatestKey(index, a[0]);
        }
        break;
    case 0xb8:
        SetParam(track, a[0], ByteParam(a[1]), 0);
        break;
    case 0xb9:
        SetParam(track, a[0], HalfParam(a[1]), 0);
        break;
    case 0xba:
        SetParam(track, a[0], ByteParam(a[1]), a[2]);
        break;
    case 0xbb:
        SetParam(track, a[0], HalfParam(a[1]), a[2]);
        break;
    case 0xc1:
        {
            const uint32_t child = OpenChild(index, a[0]);
            if (child != Track::kNone)
            {
                Track& t = tracks_[child];
                t.sequence = track.sequence;
                t.cursor = a[1];
                t.depth = 0;
                t.stack.fill(0);
                t.loop_counts.fill(0);
                t.status = Track::kRun;
            }
            break;
        }
    case 0xc2:
        CloseChild(index, a[0]);
        break;
    case 0xc3:
        call(a[0]);
        break;
    case 0xc4:
        if (Condition(track, a[0]))
        {
            call(a[1]);
        }
        break;
    case 0xc5:
        ret();
        break;
    case 0xc6:
        if (Condition(track, a[0]))
        {
            ret();
        }
        break;
    case 0xc7:
        JumpBack(index, a[0]);
        track.cursor = a[0];
        break;
    case 0xc8:
        if (Condition(track, a[0]))
        {
            JumpBack(index, a[1]);
            track.cursor = a[1];
        }
        break;
    case 0xc9:
        track.cursor = Be24(track, a[1] + 3 * a[0]);
        break;
    case 0xca:
        call(Be24(track, a[1] + 3 * a[0]));
        break;
    case 0xcb:
        if (track.depth < track.stack.size())
        {
            track.stack[track.depth] = track.cursor;
            track.loop_counts[track.depth] = static_cast<uint16_t>(a[0]);
            track.depth++;
        }
        break;
    case 0xcc:
        if (track.depth != 0)
        {
            const uint32_t top = track.depth - 1;
            const uint16_t count = track.loop_counts[top];
            if (count >= 2)
            {
                track.loop_counts[top] = static_cast<uint16_t>(count - 1);
                track.cursor = track.stack[top];
            }
            else
            {
                track.depth = top;
            }
        }
        break;
    case 0xd0:
        {
            // readPort: the port's value from the game, which clears its flag.
            uint32_t value = 0;
            if (a[0] < track.ports.size())
            {
                track.import_flags = static_cast<uint16_t>(track.import_flags & ~(1u << a[0]));
                value = track.ports[a[0]];
            }
            WriteReg(index, a[1], value & 0xffff);
            break;
        }
    case 0xd1:
        if (a[0] < track.ports.size())
        {
            track.export_flags = static_cast<uint16_t>(track.export_flags | 1u << a[0]);
            track.ports[a[0]] = static_cast<uint16_t>(a[1]);
        }
        break;
    case 0xd2:
        RegisterWrite(track, kRegFlag, (uint32_t{track.import_flags} >> (a[0] & 31)) & 1);
        break;
    case 0xd3:
        RegisterWrite(track, kRegFlag, (uint32_t{track.export_flags} >> (a[0] & 31)) & 1);
        break;
    case 0xd4:
        if (track.parent != Track::kNone)
        {
            WritePort(track.parent, a[0], a[1]);
        }
        break;
    case 0xd5:
        if (const uint32_t child = child_of(a[0]); child != Track::kNone)
        {
            WritePort(child, a[0] & 15, a[1]);
        }
        break;
    case 0xd6:
    case 0xd7:
        {
            const uint32_t from = command == 0xd6 ? track.parent : child_of(a[0]);
            const uint32_t port = command == 0xd6 ? a[0] : a[0] & 15;
            if (from != Track::kNone)
            {
                Track& other = tracks_[from];
                uint32_t value = 0;
                if (port < other.ports.size())
                {
                    other.export_flags = static_cast<uint16_t>(other.export_flags & ~(1u << port));
                    value = other.ports[port];
                }
                WriteReg(index, a[1], value & 0xffff);
            }
            break;
        }
    case 0xd8:
        WriteReg(index, a[0], a[1]);
        break;
    case 0xd9:
    case 0xda:
        {
            const uint32_t op = a[0];
            const uint32_t reg = a[1];
            const uint32_t value = a[2];
            switch (op)
            {
            case 0:
                WriteReg(index, reg, value);
                break;
            case 1:
                WriteReg(index, reg, ReadReg(track, reg) + value);
                break;
            case 2:
                WriteReg(index, reg, ReadReg(track, reg) - value);
                break;
            case 3:
                WriteReg(index, kRegFlag, ReadReg(track, reg) - value);
                break;
            case 4:
                WriteReg(index, 0x21, ReadReg(track, reg) * value);
                break;
            case 5:
                WriteReg(index, reg, ReadReg(track, reg) & value);
                break;
            case 6:
                WriteReg(index, reg, ReadReg(track, reg) | value);
                break;
            case 7:
                WriteReg(index, reg, ReadReg(track, reg) ^ value);
                break;
            case 8:
                {
                    seq_random_ = seq_random_ * 0x19660du + 0x3c6ef35fu;
                    const uint32_t r = seq_random_ >> 9;
                    WriteReg(index, reg, value != 0 ? r % value : r);
                    break;
                }
            case 9:
                WriteReg(index, reg, ReadReg(track, reg) << (value & 31));
                break;
            case 10:
                WriteReg(index, reg, ReadReg(track, reg) >> (value & 31));
                break;
            default:
                break;
            }
            break;
        }
    case 0xdb:
        if (a[0] == 0xb)
        {
            WriteReg(index, a[1], 0u - ReadReg(track, a[1]));
        }
        break;
    case 0xdc:
        {
            const uint32_t table = a[2];
            const uint32_t at = a[3];
            const auto be = [&s](uint32_t offset, int bytes)
            {
                uint32_t value = 0;
                for (int i = 0; i < bytes; i++)
                {
                    value = value << 8 | s.U8(offset + static_cast<uint32_t>(i));
                }

                return value;
            };
            switch (a[0])
            {
            case 0xc:
                WriteReg(index, a[1], s.U8(table + at));
                break;
            case 0xd:
                WriteReg(index, a[1], be(table + 2 * at, 2));
                break;
            case 0xe:
                WriteReg(index, a[1], be(table + 3 * at, 3));
                break;
            case 0xf:
                WriteReg(index, a[1], be(table + 4 * at, 4));
                break;
            case 0x10:
                WriteReg(index, a[1], be(table + at, 4));
                break;
            default:
                break;
            }
            break;
        }
    case 0xe0:
        SetTempo(track, a[0]);
        break;
    case 0xe1:
        track.bank = static_cast<uint16_t>((a[0] & 0xffff) >> 8);
        track.program = static_cast<uint16_t>(a[0] & 0xff);
        break;
    case 0xe2:
        track.bank = static_cast<uint16_t>(a[0] & 0xff);
        break;
    case 0xe3:
        track.program = static_cast<uint16_t>(a[0] & 0xff);
        break;
    case 0xe7:
        if (a[0] < track.oscillators.size())
        {
            track.oscillators[a[0]].scale = static_cast<float>(a[1]) / 16383.0f;
        }
        break;
    case 0xe8:
        if (a[0] < track.oscillators.size())
        {
            // envSet: the table in the sequence. A pitch envelope loses its release table.
            track.oscillators[a[0]].table = s.Table(a[1]);
            track.oscillator_tables[a[0]] = true;
            if (a[0] != 0)
            {
                track.oscillators[a[0]].release = {};
            }
        }
        break;
    case 0xe9:
        // simpleADSR: the volume's envelope from the track's points, with a direct release.
        track.adsr[0].time = static_cast<int16_t>(a[0]);
        track.adsr[1].time = static_cast<int16_t>(a[1]);
        track.adsr[2].time = static_cast<int16_t>(a[2]);
        track.adsr[2].value = static_cast<int16_t>(a[3]);
        track.direct_release = static_cast<uint16_t>(a[4]);
        track.oscillators[0] = kEnvOsc;
        track.oscillators[0].table = track.adsr;
        track.oscillator_tables[0] = true;
        break;
    case 0xea:
        if (a[0] < track.mix_configs.size())
        {
            track.mix_configs[a[0]] = static_cast<uint16_t>(a[1]);
        }
        break;
    case 0xeb:
        if (a[0] <= 0x7f)
        {
            for (std::size_t i = 0; i < track.iir.size(); i++)
            {
                track.iir[i] = static_cast<uint16_t>(kCutoffToIir[4 * a[0] + i]);
            }
            track.filter_mode |= 0x20;
        }
        break;
    case 0xec:
        // IIRSet: four coefficients. The game copies four more from its stack. The DSP doesn't read them. The model
        // leaves them 0.
        track.iir = {static_cast<uint16_t>(a[0]),
                     static_cast<uint16_t>(a[1]),
                     static_cast<uint16_t>(a[2]),
                     static_cast<uint16_t>(a[3]),
                     0,
                     0,
                     0,
                     0};
        track.filter_mode |= 0x20;
        break;
    case 0xed:
        for (uint32_t i = 0; i < track.fir.size(); i++)
        {
            track.fir[i] = s.Native16(a[0] + 2 * i);
        }
        track.filter_mode = static_cast<uint8_t>((track.filter_mode & 0x20) | 8);
        break;
    case 0xf0:
        track.timer = static_cast<int32_t>(ReadMidiValue(track));
        break;
    case 0xf1:
        track.timer = static_cast<int32_t>(a[0]);
        break;
    case 0xf3:
        track.interrupt_table = a[0];
        break;
    case 0xf4:
        track.mask = static_cast<uint16_t>(track.mask | a[0]);
        break;
    case 0xf5:
        track.mask = static_cast<uint16_t>(track.mask & ~a[0]);
        break;
    case 0xf6:
        if (track.interrupted)
        {
            track.cursor = track.swap;
            track.interrupted = false;
            track.swap = 0;
        }
        CheckInterrupt(track);
        break;
    case 0xf7:
        track.interrupted = false;
        track.swap = 0;
        CheckInterrupt(track);
        break;
    case 0xf8:
        track.interrupt_timer = static_cast<uint8_t>(a[0]);
        track.timer_count = a[1];
        track.timer_reload = a[1];
        break;
    case 0xf9:
        // syncCPU: the game's callbacks answer from the game's state. Without a callback, the command gets 0xffff.
        // A rip has none of that state.
        RegisterWrite(track, kRegFlag, 0xffff);
        break;
    case 0xfd:
        Printf(track);
        break;
    case 0xff:
        return -1;
    default:
        break;
    }

    return 0;
}

void Tracks::Printf(Track& track)
{
    // The format, up to 0x80 characters or its end, then a byte for each of its conversions. The game prints them.
    const Sequence& s = *track.sequence;
    uint32_t conversions = 0;
    for (uint32_t i = 0; i < 0x80; i++)
    {
        const uint32_t c = s.U8(track.cursor++);
        if (c == '%')
        {
            i++;
            if (s.U8(track.cursor++) == 0)
            {
                break;
            }
            conversions++;
        }
        else if (c == '\\')
        {
            i++;
            if (s.U8(track.cursor++) == 0)
            {
                break;
            }
        }
        else if (c == 0)
        {
            break;
        }
    }
    track.cursor += conversions;
}

void Tracks::SetParam(Track& track, uint32_t param, float value, uint32_t count)
{
    if (param >= track.params.size())
    {
        return;
    }

    TrackParam& p = track.params[param];
    p.target = value;
    if (count == 0)
    {
        p.value = value;
    }
    p.count = count;
}

void Tracks::SetTempo(Track& track, uint32_t tempo)
{
    track.tempo = static_cast<uint16_t>(tempo);
    track.flags |= Track::kInvalid;
    if (track.parent != Track::kNone)
    {
        UpdateTempo(track);
        return;
    }

    const uint32_t ticks = uint32_t{track.timebase} * (tempo & 0xffff);
    const float scaled = static_cast<float>(static_cast<int32_t>(ticks)) * kTempoScale;
    track.interval = track.tempo_rate * (scaled / voices_.DacRate());
}

void Tracks::SetLatestKey(uint32_t index, uint32_t key)
{
    tracks_[index].latest_key = static_cast<uint8_t>(key + static_cast<uint32_t>(TransposeSum(index)));
}

void Tracks::WritePort(uint32_t index, uint32_t port, uint32_t value)
{
    // A write to another track's port, as the game would: its flag, and interrupt 2 or 3 for port 0 or 1.
    Track& track = tracks_[index];
    if (port < track.ports.size())
    {
        track.import_flags = static_cast<uint16_t>(track.import_flags | 1u << port);
        track.ports[port] = static_cast<uint16_t>(value);
    }
    if (port <= 1)
    {
        const uint32_t bit = 1u << (port == 0 ? 2 : 3);
        if ((track.mask & bit) != 0)
        {
            track.pending = static_cast<uint16_t>(track.pending | bit);
        }
    }
}

uint32_t Tracks::OpenChild(uint32_t index, uint32_t slot)
{
    if (slot >= tracks_[index].children.size())
    {
        return Track::kNone;
    }

    uint32_t child = tracks_[index].children[slot];
    if (child != Track::kNone)
    {
        const uint32_t status = tracks_[child].status;
        if (status == Track::kRun)
        {
            UpdateSeq(Root(index), false, 1.0f);
            Close(child);
        }
        if (status == Track::kRun || status == Track::kStop)
        {
            Track& t = tracks_[child];
            const uint8_t owned = t.flags & Track::kOwned;
            Init(child);
            t.flags = static_cast<uint8_t>((t.flags & ~Track::kOwned) | owned);
            t.parent = index;
        }
    }
    else
    {
        child = AllocTrack();
        if (child == Track::kNone)
        {
            return Track::kNone;
        }
        Construct(child);
        tracks_[child].flags |= Track::kOwned;
        tracks_[child].parent = index;
        tracks_[index].children[slot] = child;
    }

    // As many channel managers as the parent's.
    Track& parent = tracks_[index];
    Track& t = tracks_[child];
    t.mgr_count = 1;
    for (uint32_t i = 1; i < parent.mgr_count && i < t.mgrs.size(); i++)
    {
        if (!t.mgrs[i].exists)
        {
            t.mgrs[i] = ChannelMgr{};
            t.mgrs[i].exists = true;
        }
        t.mgr_count++;
    }
    for (uint32_t i = t.mgr_count; i < t.mgrs.size(); i++)
    {
        t.mgrs[i] = ChannelMgr{};
    }

    // The child takes the parent's settings of its notes.
    t.flags = static_cast<uint8_t>((t.flags & ~Track::kGateByTempo) | (parent.flags & Track::kGateByTempo));
    t.bank = parent.bank;
    t.program = parent.program;
    t.note_on_priority = parent.note_on_priority;
    t.release_priority = parent.release_priority;
    t.skip_samples = parent.skip_samples;
    t.fir = parent.fir;
    t.iir = parent.iir;
    t.filter_mode = parent.filter_mode;
    t.mix_configs = parent.mix_configs;

    return child;
}

void Tracks::CloseChild(uint32_t index, uint32_t slot)
{
    if (slot >= tracks_[index].children.size())
    {
        return;
    }
    const uint32_t child = tracks_[index].children[slot];
    if (child == Track::kNone)
    {
        return;
    }

    UpdateSeq(Root(index), false, 1.0f);
    Close(child);
    if ((tracks_[child].flags & Track::kOwned) != 0)
    {
        FreeTrack(child);
        tracks_[index].children[slot] = Track::kNone;
    }
}

void Tracks::NoteOn(uint32_t index, uint32_t note, uint32_t key, uint32_t velocity)
{
    if (Muted(index) || note >= 8)
    {
        return;
    }

    Track& track = tracks_[index];
    const uint32_t played = key + static_cast<uint32_t>(TransposeSum(index));
    for (uint32_t i = 0; i < track.mgr_count; i++)
    {
        ChannelMgr& mgr = track.mgrs[i];
        if (!mgr.exists)
        {
            continue;
        }

        if (mgr.channels[note] != Channel::kNone)
        {
            voices_.Release(mgr.channels[note], 0);
            voices_.Free(mgr.channels[note]);
            mgr.channels[note] = Channel::kNone;
        }
        mgr.channels[note] = ChannelStart(index, i, played, velocity, 0);
    }
}

void Tracks::NoteOff(uint32_t index, uint32_t note, uint16_t direct_release)
{
    Track& track = tracks_[index];
    if (note >= 8)
    {
        return;
    }

    for (uint32_t i = 0; i < track.mgr_count; i++)
    {
        ChannelMgr& mgr = track.mgrs[i];
        if (mgr.exists && mgr.channels[note] != Channel::kNone)
        {
            voices_.Release(mgr.channels[note], direct_release);
            voices_.Free(mgr.channels[note]);
            mgr.channels[note] = Channel::kNone;
        }
    }
}

void Tracks::NoteOnGate(uint32_t index, uint32_t key, uint32_t velocity, uint32_t time, uint32_t flags)
{
    // The note's flags: bit 7 sweeps from the latest key, bit 6 ties the next gate note to this one.
    uint32_t gate = ((flags >> 5) & 2) | ((flags >> 7) & 1);
    if (time == 0)
    {
        gate |= 4;
    }
    GateOn(index, key, velocity, gate, static_cast<float>(time));

    Track& track = tracks_[index];
    if (time != 0)
    {
        track.timer = static_cast<int32_t>(time);
    }
    else
    {
        track.wait_note_finish = true;
    }
}

void Tracks::GateOn(uint32_t index, uint32_t key, uint32_t velocity, uint32_t flags, float time)
{
    Track& track = tracks_[index];
    const uint32_t played = key + static_cast<uint32_t>(TransposeSum(index));
    if (track.gate_rate != 100)
    {
        time = (static_cast<float>(track.gate_rate) / 100.0f) * time;
    }

    uint32_t length = 0;
    if ((track.flags & Track::kGateByTempo) != 0)
    {
        length = ToUint32(time / track.interval);
    }
    else
    {
        const float scaled = time * (120.0f / static_cast<float>(track.timebase));
        length = ToUint32(scaled * (static_cast<float>(kSubFrames) / 10.0f));
    }

    const uint32_t timer = (flags & 6) != 0 ? 0 : length;
    const uint32_t latest = track.latest_key;
    const uint32_t note_key = (flags & 1) != 0 ? latest : played;
    const int32_t sweep = static_cast<int32_t>(played - latest);
    for (uint32_t i = 0; i < track.mgr_count; i++)
    {
        ChannelMgr& mgr = track.mgrs[i];
        if (!mgr.exists)
        {
            continue;
        }

        uint32_t& channel = mgr.channels[0];
        if ((track.flags & Track::kGateTie) != 0)
        {
            if (channel != Channel::kNone)
            {
                Channel& c = voices_.ChannelAt(channel);
                c.velocity = static_cast<uint16_t>(velocity);
                c.update_timer = timer;
                c.key = static_cast<int16_t>(note_key - c.wave.base_key);
            }
        }
        else
        {
            if (channel != Channel::kNone)
            {
                voices_.Release(channel, 0);
                voices_.Free(channel);
                channel = Channel::kNone;
            }
            if (!Muted(index))
            {
                channel = ChannelStart(index, i, note_key, velocity, timer);
            }
        }

        if ((flags & 1) != 0 && channel != Channel::kNone)
        {
            // setKeySweepTarget: a sweep from the latest key to this one over the note's length, or at once.
            Channel& c = voices_.ChannelAt(channel);
            float delta = static_cast<float>(sweep);
            if (length != 0)
            {
                c.key_sweep_target = delta;
                delta = 0.0f;
            }
            c.key_sweep = delta;
            c.key_sweep_count = length;
        }
    }

    track.latest_key = static_cast<uint8_t>(played);
    track.flags = static_cast<uint8_t>((track.flags & ~Track::kGateTie) | ((flags << 3) & Track::kGateTie));
}

uint32_t Tracks::ChannelStart(uint32_t index, uint32_t mgr_index, uint32_t key, uint32_t velocity, uint32_t timer)
{
    Track& track = tracks_[index];
    notes_[index]++;
    ChannelMgr& mgr = track.mgrs[mgr_index];
    const InstrumentBank* bank = track.bank < banks_.size() ? banks_[track.bank] : nullptr;
    const auto priority = static_cast<uint16_t>(track.note_on_priority | track.release_priority << 8);
    const uint32_t channel = BankNoteOn(bank, track.program, key, velocity, priority, Owner(index, mgr_index));
    if (channel == Channel::kNone)
    {
        // The game asks the bank again for its debug output. The instrument's random effects step again. For a bank
        // that isn't loaded, the port reads through a null pointer there and stops. The model goes on without the note.
        if (bank != nullptr)
        {
            InstParam param;
            bank->GetInstParam(track.program, key & 0xff, velocity & 0xff, param, inst_random_);
        }
        else
        {
            missing_bank_notes_++;
        }

        return Channel::kNone;
    }

    Channel& c = voices_.ChannelAt(channel);
    c.update_timer = timer;
    c.skip_samples = track.skip_samples;
    c.vibrato.SetDelay(track.vibrato_delay);
    c.tremolo.SetDelay(track.tremolo_delay);
    c.params = mgr.params;
    c.mix_configs = track.mix_configs;

    // overwriteOsc: the track's envelopes replace the instrument's.
    for (uint32_t i = 0; i < track.oscillators.size(); i++)
    {
        if (track.oscillator_tables[i])
        {
            c.oscillators[i].InitStart(&track.oscillators[i]);
        }
    }
    if (track.direct_release != 0)
    {
        c.oscillators[0].SetDirectRelease(track.direct_release);
    }

    return channel;
}

uint32_t Tracks::BankNoteOn(const InstrumentBank* bank, uint32_t program, uint32_t key, uint32_t velocity,
                            uint16_t priority, uint32_t owner)
{
    if (program >= 0xf0)
    {
        return NoteOnOsc(program - 0xf0, key, velocity, priority, owner);
    }
    if (bank == nullptr)
    {
        return Channel::kNone;
    }

    InstParam param;
    if (!bank->GetInstParam(program, key & 0xff, velocity & 0xff, param, inst_random_))
    {
        return Channel::kNone;
    }
    const BankWave* wave = bank->Waves() != nullptr ? bank->Waves()->Find(param.wave_id) : nullptr;
    if (wave == nullptr)
    {
        return Channel::kNone;
    }

    // The game's pool of channels never runs out in its music. Without a channel, the model drops the note.
    const uint32_t channel = voices_.NewChannel(owner);
    if (channel == Channel::kNone)
    {
        return Channel::kNone;
    }

    Channel& c = voices_.ChannelAt(channel);
    c.priority = priority;
    c.wave = wave->info;
    c.wave_address = wave->address;
    c.type = param.type;
    c.sound_params.pitch = (wave->info.sample_rate / voices_.DacRate()) * param.pitch;
    if (!param.fixed_key)
    {
        c.key = static_cast<int16_t>((key & 0xff) - wave->info.base_key);
    }
    c.velocity = static_cast<uint16_t>(velocity & 0xff);
    c.sound_params.volume = param.volume;
    c.sound_params.fxmix = param.fxmix;
    c.sound_params.pan = param.pan;
    c.sound_params.dolby = param.dolby;
    for (uint32_t i = 0; i < param.oscillator_count && i < c.oscillators.size(); i++)
    {
        c.oscillators[i].InitStart(param.oscillators[i]);
    }
    c.oscillators[0].SetDirectRelease(param.direct_release);

    return voices_.Play(this, channel) ? channel : Channel::kNone;
}

uint32_t Tracks::NoteOnOsc(uint32_t wave, uint32_t key, uint32_t velocity, uint16_t priority, uint32_t owner)
{
    const uint32_t channel = voices_.NewChannel(owner);
    if (channel == Channel::kNone)
    {
        return Channel::kNone;
    }

    Channel& c = voices_.ChannelAt(channel);
    c.priority = priority;
    c.wave_address = wave;
    c.type = Channel::kOsc;
    c.key = static_cast<int16_t>((key & 0xff) - c.wave.base_key);
    c.velocity = static_cast<uint16_t>(velocity & 0xff);
    c.sound_params.pitch = kOscillatorRate / voices_.DacRate();
    c.oscillators[0].InitStart(&kOscEnv);

    return voices_.Play(this, channel) ? channel : Channel::kNone;
}

void Tracks::OnChannel(uint32_t owner, Channel::Event event, uint32_t channel, DspBlock* block)
{
    const uint32_t index = owner / 4;
    if (index >= tracks_.size())
    {
        return;
    }

    Track& track = tracks_[index];
    ChannelMgr& mgr = track.mgrs[owner % 4];
    Channel& c = voices_.ChannelAt(channel);

    switch (event)
    {
    case Channel::kEventPlay:
    case Channel::kEventStart:
        {
            c.params = mgr.params;
            c.vibrato.SetDepth(track.vibrato_depth);
            c.vibrato.SetPitch(track.vibrato_pitch);
            c.tremolo.SetDepth(track.tremolo_depth);
            c.tremolo.SetPitch(track.tremolo_pitch);
            if ((track.filter_mode & 0x20) != 0)
            {
                block->SetIirFilterParam(track.iir);
            }
            if ((track.filter_mode & 0x1f) != 0)
            {
                block->SetFir8FilterParam(track.fir);
            }
            block->SetFilterMode(track.filter_mode);
            block->SetDistFilter(ToInt32(track.params[5].value * 32767.0f));
            break;
        }
    case Channel::kEventStop:
        for (uint32_t& slot : mgr.channels)
        {
            if (slot == channel)
            {
                slot = Channel::kNone;
                break;
            }
        }
        break;
    case Channel::kEventTimer:
        voices_.Release(channel, 0);
        voices_.Free(channel);
        mgr.channels[0] = Channel::kNone;
        break;
    }
}

} // namespace hsf::jaudio
