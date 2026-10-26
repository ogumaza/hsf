// SPDX-License-Identifier: MIT

#include "jaudio1/track.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include "jaudio1/calc.h"
#include "jaudio1/iir_table.h"

namespace hsf::jaudio1
{

namespace
{

// JASPlayer's tables of points, from main's data (0xb63be0 to 0xb63c4c): a simple ADSR, the release of the default
// envelope, a vibrato and a tremolo. Each point is a mode, a time and a value.
constexpr std::array<OscPoint, 4> kAdsTable = {{{0, 0, 0x7fff}, {0, 0, 0x7fff}, {0, 0, 0}, {0xe, 0, 0}}};
constexpr std::array<OscPoint, 2> kRelTable = {{{0, 10, 0}, {15, 1, 0}}};
constexpr std::array<OscPoint, 6> kVibTable = {
    {{0, 0, 0}, {0, 12, 0x7fff}, {0, 12, 0}, {0, 12, -0x4000}, {0, 12, 0}, {13, 0, 1}}};
constexpr std::array<OscPoint, 6> kTreTable = {
    {{0, 0, 0x7fff}, {0, 20, 0}, {0, 20, -0x7fff}, {0, 20, 0}, {0, 20, 0x7fff}, {13, 0, 1}}};

// JASBankMgr's release of a note on the DSP's oscillator (OSC_RELEASE_TABLE, 0x94ab1e).
constexpr std::array<OscPoint, 2> kOscReleaseTable = {{{1, 10, 0}, {15, 0, 0}}};

template <std::size_t N>
OscTable TableOf(const std::array<OscPoint, N>& points)
{
    return {points.data(), N};
}

// JASPlayer's settings of the oscillators (sEnvelopeDef, sVibratoDef, sTremoroDef and sAdsrDef), and JASBankMgr's for a
// note on the DSP's oscillator (OSC_ENV, 0xb62fc0).
const OscData kEnvelopeDef{0, 1.0f, {}, TableOf(kRelTable), 1.0f, 0.0f};
const OscData kVibratoDef{1, 0.5f, TableOf(kVibTable), TableOf(kVibTable), 0.0f, 1.0f};
const OscData kTremoroDef{0, 0.5f, TableOf(kTreTable), TableOf(kTreTable), 0.0f, 1.0f};
const OscData kAdsrDef{0, 1.0f, {}, {}, 1.0f, 0.0f};
const OscData kOscEnv{0, 1.0f, {}, TableOf(kOscReleaseTable), 1.0f, 0.0f};

// The tables of JASSeqParser::cmdPanSwSet (0x94b534 and 0x94b53b): the ways that a track's level mixes with the
// outer level and with the parent's level. Index 7 reads the byte after each table, 0.
constexpr std::array<uint8_t, 8> kCalcTypes = {0, 0, 0, 1, 1, 2, 2, 0};
constexpr std::array<uint8_t, 8> kParentCalcTypes = {0, 1, 2, 0, 2, 0, 2, 0};

// JASSeqParser's Arglist (0x94b542): for each command from 0xc0, the count of arguments, and their widths in 2 bits
// each from the low bits: 0 a byte, 1 16 bits, 2 24 bits, 3 a register's value by a byte.
struct ArgSpec
{
    uint16_t count;
    uint16_t types;
};

// clang-format off
constexpr std::array<ArgSpec, 64> kArgs = {{
    {0, 0}, {2, 8}, {2, 8}, {1, 2}, {0, 0}, {0, 0}, {1, 0}, {1, 2}, {0, 0}, {1, 1}, {0, 0}, {2, 0}, {2, 0xc}, {1, 0},
    {1, 0}, {1, 3}, {0, 0}, {2, 0xc}, {2, 0xc}, {0, 0}, {1, 0}, {1, 0}, {1, 0}, {2, 8}, {5, 0x155}, {1, 0}, {1, 0},
    {1, 0}, {1, 1}, {2, 4}, {1, 0}, {2, 8}, {1, 0}, {0, 0}, {0, 0}, {0, 0}, {2, 4}, {1, 0}, {1, 1}, {1, 1}, {0, 0},
    {0, 0}, {1, 2}, {5, 0}, {4, 0x55}, {1, 2}, {1, 2}, {3, 0}, {1, 0}, {1, 0}, {3, 0x28}, {1, 0}, {1, 0}, {0, 0},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {1, 1}, {0, 0}, {0, 0}, {1, 1}, {1, 1}, {0, 0},
}};
// clang-format on

// The commands from 0xc0 without a handler (sCmdPList's null entries).
constexpr uint64_t kNullCommands = 0x03e00000000900a9ull;

// The register numbers (Track::kRegisters).
constexpr uint32_t kRegResult = 3;
constexpr uint32_t kRegProduct = 4; // the high half of a product, then the low half in register 5
constexpr uint32_t kRegBankProgram = 6;
constexpr uint32_t kRegBendRange = 7;
constexpr uint32_t kRegPanPower = 8;
constexpr uint32_t kRegOuterWeight = 11;
constexpr uint32_t kRegParentWeight = 12;
constexpr uint32_t kRegPriority = 13;
constexpr uint32_t kRegLong = 16;
constexpr uint32_t kRegAfter = 24; // the first of the words after JASRegisterParam

// The flags of the levels' updates (JASOuterParamFlag).
constexpr uint32_t kUpdateVolume = 0x1;
constexpr uint32_t kUpdatePitch = 0x2;
constexpr uint32_t kUpdateFxmix = 0x4;
constexpr uint32_t kUpdatePan = 0x8;
constexpr uint32_t kUpdateDolby = 0x10;
constexpr uint32_t kUpdateDistortion = 0x20;
constexpr uint32_t kUpdateTempo = 0x40;
constexpr uint32_t kUpdateFir = 0x80;
constexpr uint32_t kUpdateIir = 0xf000;
constexpr uint32_t kUpdateDelay = 0x20000;

// The timed levels by index.
constexpr std::size_t kTimedVolume = 0;
constexpr std::size_t kTimedPitch = 1;
constexpr std::size_t kTimedFxmix = 2;
constexpr std::size_t kTimedPan = 3;
constexpr std::size_t kTimedDolby = 4;
constexpr std::size_t kTimedDistortion = 5;
constexpr std::size_t kTimedIir = 12;
constexpr std::size_t kTimedPause = 16;
constexpr std::size_t kTimedDelay = 17;

// 32767 and 1/32768 in single precision.
constexpr float kShortMax = 32767.0f;
constexpr float kShortScale = 1.0f / 32768.0f;

// JASPlayer::extend8to16: a byte's sign extended to 16 bits.
uint32_t Extend8To16(uint32_t value)
{
    const uint32_t byte = value & 0xff;
    return (byte & 0x80) != 0 ? (byte | 0xff00) : byte;
}

// Whether JASTrack::readReg16 reads register `reg` from the registers or from the track's state, and not from the
// memory after the registers.
bool ReadableRegister16(uint32_t reg)
{
    return reg < Track::kRegisters || reg == 0x20 || reg == 0x21 || reg == 0x22 || reg == 0x2c || reg == 0x2d ||
           reg == 0x30;
}

// Whether the result of writeRegParam's operation depends on the target's value.
bool OperationReadsTarget(uint32_t operation)
{
    switch (operation)
    {
    case 1:
    case 2:
    case 3:
    case 0xb:
    case 0x10:
    case 0x20:
    case 0x30:
    case 0x40:
    case 0x50:
    case 0x60:
        return true;
    default:
        return false;
    }
}

// JASTrack::panCalc: a level mixed with another by the weight, as the calculation type says.
float PanCalc(float a, float b, float weight, uint32_t type)
{
    switch (type)
    {
    case 0:
        return a;
    case 1:
        return b;
    case 2:
        return (1.0f - weight) * a + weight * b;
    default:
        return 0.0f;
    }
}

// A track's level of the delay between its outputs (the field at +0x2e0): 128 times the level, truncated, then one of
// the two delays. The Switch build tests the byte's sign by bit 7 of the 32-bit value.
std::array<uint8_t, 2> DelaySamples(float level)
{
    const int32_t delay = ToInt32(level * 128.0f);
    const uint32_t value = static_cast<uint32_t>(delay);

    return {static_cast<uint8_t>((0u - value) & (value >> 7)), static_cast<uint8_t>(delay < 0 ? 0 : value)};
}

} // namespace

void OuterParam::SetParam(uint8_t flag, float value)
{
    switch (flag)
    {
    case kVolume:
        volume = value;
        break;
    case kPitch:
        pitch = value;
        break;
    case kFxmix:
        fxmix = value;
        break;
    case kPan:
        pan = value;
        break;
    case kDolby:
        dolby = value;
        break;
    case kTempo:
        tempo = value;
        break;
    default:
        return;
    }
    outer_update |= flag;
}

Tracks::Tracks(Voices& voices) : voices_(voices)
{
    tracks_.resize(kPoolTracks + kRootTracks);
    notes_.resize(tracks_.size());
    loop_notes_.resize(tracks_.size());
    looped_.resize(tracks_.size());
    quiet_.resize(tracks_.size());

    // JASTrack::newMemPool: each track of the pool comes with outer parameters (their constructor zeroes them).
    for (uint32_t i = 0; i < kPoolTracks; i++)
    {
        tracks_[i].outer = &pool_outer_.emplace_back();
        free_tracks_.push_back(i);
    }

    // The root tracks are JAInter::SeqParameter's, constructed once.
    for (std::size_t i = kPoolTracks; i < tracks_.size(); i++)
    {
        Construct(tracks_[i]);
    }

    // JASBankMgr::init: no number names a bank yet. A note on such a number asks for physical bank 0xff.
    virtual_banks_.fill(0xffff);
}

void Tracks::SetBank(uint32_t physical, const InstrumentBank* bank)
{
    banks_[physical & 0xff] = bank;
}

void Tracks::SetVirtualBank(uint32_t number, uint32_t physical)
{
    // JASBankMgr::setVir2PhyTable: number 0xffff names no bank.
    if (number == 0xffff)
    {
        return;
    }

    virtual_banks_[number & 0xff] = static_cast<uint16_t>(physical);
}

void Tracks::Construct(Track& track)
{
    // JASTrack's constructor. The outer parameters stay: the pool's tracks keep theirs. So do registers 14, 15 and 24
    // to 31.
    OuterParam* const outer = track.outer;
    const std::array<uint16_t, Track::kRegisters> registers = track.registers;
    track = Track{};
    track.outer = outer;
    track.vibrate.Init();
    track.tempo = 120;
    track.timebase = 120;
    track.pause_status = 0;
    track.time_relate = false;
    track.tempo_rate = 0.0f;
    std::copy(registers.begin() + 14, registers.begin() + 16, track.registers.begin() + 14);
    std::copy(registers.begin() + kRegAfter, registers.end(), track.registers.begin() + kRegAfter);
    std::copy(kAdsTable.begin(), kAdsTable.end(), track.adsr.begin());
    track.timed.fill(MoveParam{});
}

void Tracks::Init(uint32_t index)
{
    // JASTrack::init (0x3dc23c).
    Track& track = tracks_[index];
    track.sequence = nullptr;
    track.cursor = 0;
    track.wait = 0;
    track.depth = 0;
    track.stack.fill(0);
    track.loop_counts.fill(0);
    track.saved_wait = 0;
    track.interrupted = false;
    track.return_cursor = 0;

    track.import_flags.fill(0);
    track.export_flags.fill(0);
    track.ports.fill(0);

    track.interrupts_on = true;
    track.requests = 0;
    track.enabled = 0;
    track.timer_repeats = 0;
    track.timer = 0;
    track.timer_reload = 0;
    track.interrupt_targets.fill(0);
    track.targets_set = 0;

    track.channels.fill(Track::kNone);
    track.note_time = 0;
    track.note_flags = 0;
    track.latest_key = 0;
    track.gate = 0;
    track.updater = ChannelUpdater{};

    // initTimed: the times go to 0, and each value and target to its default. The steps stay.
    for (MoveParam& param : track.timed)
    {
        param.time = 0.0f;
        param.value = 1.0f;
        param.target = 1.0f;
    }
    for (const std::size_t i : {kTimedPitch, kTimedFxmix, kTimedDolby, kTimedDistortion, kTimedIir + 1, kTimedIir + 2,
                                kTimedIir + 3, kTimedDelay})
    {
        track.timed[i].value = 0.0f;
        track.timed[i].target = 0.0f;
    }
    for (const std::size_t i : {kTimedPan, kTimedPause})
    {
        track.timed[i].value = 0.5f;
        track.timed[i].target = 0.5f;
    }

    // JASRegisterParam::init. Registers 14 and 15 keep their values.
    for (const uint32_t reg : {0u, 1u, 2u, 3u, 4u, 5u})
    {
        track.registers[reg] = 0;
    }
    track.registers[kRegBankProgram] = 0xf0;
    track.registers[kRegBendRange] = 0x0c;
    track.registers[kRegPanPower] = 0;
    track.registers[kRegPanPower + 1] = 1;
    track.registers[kRegPanPower + 2] = 1;
    track.registers[kRegOuterWeight] = 0x7fff;
    track.registers[kRegParentWeight] = 0x4000;
    track.registers[kRegPriority] = 0x40;
    std::fill(track.registers.begin() + kRegLong, track.registers.begin() + kRegAfter, uint16_t{0});

    track.osc_routes = {0xf, 0xf};
    track.oscillators = {kEnvelopeDef, kEnvelopeDef};
    track.parent = Track::kNone;
    track.children.fill(Track::kNone);
    if (track.outer != nullptr)
    {
        track.outer->Init();
    }
    track.counter = 0.0f;
    track.tempo_rate = 1.0f;
    track.route = 0;
    track.vibrate.Init();
    track.update_flags = 0;
    track.direct_release = 0;
    track.tempo = 120;
    track.timebase = 48;
    UpdateTempo(index);
    track.transpose = 0;
    track.inherit = 0;
    track.pause_status = 0x0a;
    track.volume_mode = 0;
    track.updater.pan_calc = 13;
    track.updater.fxmix_calc = 13;
    track.updater.dolby_calc = 13;
    track.note_mask = 0;
    track.status = Track::kClosed;
    track.pan_calc = 0;
    track.fxmix_calc = 0;
    track.dolby_calc = 0;
    track.parent_pan_calc = 0;
    track.parent_fxmix_calc = 0;
    track.parent_dolby_calc = 0;
    track.paused = false;
    track.muted = false;
    track.time_relate = true;
    track.tempo_child = false;
}

void Tracks::Inherit(uint32_t index)
{
    // The end of JASTrack::openChild: the child takes its parent's tempo, timing, pause and mute, and unless bit 1
    // of its inherit flags says not to, the parent's registers and calculation types.
    Track& track = tracks_[index];
    if (track.parent == Track::kNone)
    {
        return;
    }

    const Track& parent = tracks_[track.parent];
    track.tempo = parent.tempo;
    track.tempo_child = false;
    track.tempo_rate = parent.tempo_rate;
    track.timebase = parent.timebase;
    track.time_relate = parent.time_relate;
    track.paused = parent.paused;
    track.pause_status = parent.pause_status;
    track.volume_mode = parent.volume_mode;
    track.muted = parent.muted;
    if ((track.inherit & 2) != 0)
    {
        return;
    }

    // JASRegisterParam::inherit.
    for (uint32_t reg = 0; reg <= 5; reg++)
    {
        track.registers[reg] = 0;
    }
    for (const uint32_t reg : {kRegBankProgram, kRegBendRange, kRegPriority, kRegPanPower, kRegPanPower + 1,
                               kRegPanPower + 2, kRegOuterWeight, kRegParentWeight})
    {
        track.registers[reg] = parent.registers[reg];
    }
    std::fill(track.registers.begin() + kRegLong, track.registers.begin() + kRegAfter, uint16_t{0});

    track.pan_calc = parent.pan_calc;
    track.parent_pan_calc = parent.parent_pan_calc;
    track.updater.pan_calc = parent.updater.pan_calc;
    track.fxmix_calc = parent.fxmix_calc;
    track.parent_fxmix_calc = parent.parent_fxmix_calc;
    track.updater.fxmix_calc = parent.updater.fxmix_calc;
    track.dolby_calc = parent.dolby_calc;
    track.parent_dolby_calc = parent.parent_dolby_calc;
    track.updater.dolby_calc = parent.updater.dolby_calc;
}

uint32_t Tracks::AllocTrack()
{
    // JASTrack's operator new: the pool's first free track, or none.
    if (free_tracks_.empty())
    {
        return Track::kNone;
    }

    const uint32_t index = free_tracks_.front();
    free_tracks_.pop_front();

    return index;
}

void Tracks::FreeTrack(uint32_t index)
{
    // JASTrack's operator delete: the track goes to the end of the pool's list.
    tracks_[index].allocated = false;
    free_tracks_.push_back(index);
}

uint32_t Tracks::StartSequence(const Sequence& sequence, OuterParam* outer)
{
    for (uint32_t index = kPoolTracks; index < tracks_.size(); index++)
    {
        Track& track = tracks_[index];
        if (track.allocated)
        {
            continue;
        }

        // JAInter::SeqParameter's assignExtBuffer, then JASTrack::setSeqData and startSeq.
        track.allocated = true;
        track.outer = outer;
        Init(index);
        track.inherit = 3;
        track.sequence = &sequence;
        track.cursor = 0;
        notes_[index] = 0;
        loop_notes_[index] = 0;
        looped_[index] = false;
        quiet_[index] = false;
        UpdateTrackAll(index);
        track.status = Track::kPlaying;
        list_.push_back(index);

        return index;
    }

    return Track::kNone;
}

void Tracks::RunSubframe()
{
    // The driver's sub-frame callbacks run in the order that they were registered. A callback that returns -1 is
    // removed.
    for (std::size_t i = 0; i < list_.size();)
    {
        if (RootCallback(list_[i]) < 0)
        {
            tracks_[list_[i]].allocated = false;
            list_.erase(list_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        i++;
    }
}

int32_t Tracks::RootCallback(uint32_t index)
{
    // JASTrack::rootCallback (0x3de40c). The game's branch for a track that stops (JASTrack::stopSeq) is left out:
    // nothing in the model stops a sequence.
    Track& track = tracks_[index];
    if (track.status == Track::kClosed)
    {
        return -1;
    }

    track.counter = track.tempo_rate + track.counter;
    if (track.counter < 1.0f)
    {
        UpdateSeq(index, 0, true);
        return 0;
    }
    while (track.counter >= 1.0f)
    {
        track.counter = track.counter + -1.0f;
        if (MainProc(index) == -1)
        {
            UpdateSeq(index, 0, true);
            Close(index);
            return -1;
        }
    }

    return 0;
}

void Tracks::Request(Track& track, uint32_t interrupt)
{
    // JASIntrMgr::request: a request counts only for an enabled interrupt.
    const uint32_t bit = 1u << (interrupt & 31);
    if ((bit & track.enabled) != 0)
    {
        track.requests = static_cast<uint8_t>(track.requests | bit);
    }
}

void Tracks::TimerProcess(Track& track)
{
    // JASIntrMgr::timerProcess: the timer interrupt (6) at the count's end, then the reload unless the repeats run
    // out.
    if (track.timer == 0)
    {
        return;
    }

    track.timer--;
    if (track.timer != 0)
    {
        return;
    }
    if ((track.enabled & 0x40) != 0)
    {
        track.requests = static_cast<uint8_t>(track.requests | 0x40);
    }
    if (track.timer_repeats != 0)
    {
        track.timer_repeats--;
        if (track.timer_repeats == 0)
        {
            return;
        }
    }
    track.timer = track.timer_reload;
}

bool Tracks::TryInterrupt(Track& track)
{
    // JASTrack::tryInterrupt: JASIntrMgr::checkIntr takes the lowest pending interrupt, then JASSeqCtrl::callIntr
    // saves the cursor and the wait.
    if (track.interrupted || !track.interrupts_on)
    {
        return false;
    }

    const uint32_t pending = static_cast<uint32_t>(track.requests & track.enabled);
    if (pending == 0)
    {
        return false;
    }

    const uint32_t interrupt = static_cast<uint32_t>(std::countr_zero(pending));
    track.requests = static_cast<uint8_t>(track.requests & ~(1u << interrupt));

    track.interrupted = true;
    track.return_cursor = track.cursor;
    track.cursor = track.interrupt_targets[interrupt];
    track.saved_wait = track.wait;
    track.wait = 0;

    return true;
}

int32_t Tracks::MainProc(uint32_t index)
{
    // JASTrack::mainProc (0x3dc60c).
    Track& track = tracks_[index];
    if (track.tempo_child && track.parent != Track::kNone)
    {
        const float ratio =
            std::fmin(static_cast<float>(track.tempo) / static_cast<float>(tracks_[track.parent].tempo), 1.0f);
        track.counter = track.counter + ratio;
        if (track.counter < 1.0f)
        {
            return 0;
        }

        track.counter = track.counter + -1.0f;
    }

    Request(track, 7);
    TimerProcess(track);
    TryInterrupt(track);

    if (track.paused && (track.pause_status & 2) != 0)
    {
        UpdateSeq(index, 0, false);
    }
    else
    {
        int32_t result = 0;
        bool parse = true;
        if (track.wait == -1)
        {
            const uint32_t channel = track.channels[0];
            if (channel != Track::kNone && voices_.ChannelAt(channel).status != Channel::kStop)
            {
                parse = false;
            }
            else
            {
                track.wait = 0;
            }
        }
        if (parse)
        {
            if (track.saved_wait - 1 >= 0)
            {
                track.saved_wait = track.saved_wait - 1;
            }
            if (track.wait >= 1)
            {
                // JASSeqCtrl::waitCountDown.
                track.wait--;
                if (track.wait == 0)
                {
                    if (track.note_time != -1 && track.note_flags == 0)
                    {
                        track.channels[0] = Track::kNone;
                    }
                }
                else
                {
                    parse = false;
                }
            }
        }
        if (parse)
        {
            result = ParseSeq(index);
        }

        UpdateTimedParam(tracks_[index]);
        UpdateSeq(index, 0, false);
        if (result < 0)
        {
            return -1;
        }
    }

    for (std::size_t slot = 0; slot < 16; slot++)
    {
        const uint32_t child = tracks_[index].children[slot];
        if (child != Track::kNone && tracks_[child].status != Track::kClosed && MainProc(child) == -1)
        {
            Close(child);
            tracks_[index].children[slot] = Track::kNone;
        }
    }

    return 0;
}

void Tracks::UpdateTimedParam(Track& track)
{
    // The timed part of mainProc: each level that moves takes a step. A level's flag asks for an update, except for
    // the oscillators' 5 settings from index 6, which go straight into the track's settings. Index 11, the second
    // oscillator's vertex, sets its flag and changes nothing.
    for (std::size_t i = 0; i < track.timed.size(); i++)
    {
        MoveParam& param = track.timed[i];
        if (!(param.time > 0.0f))
        {
            continue;
        }

        param.time = param.time + -1.0f;
        param.value = param.amount + param.value;
        switch (i)
        {
        case 6:
            track.oscillators[0].width = param.value;
            break;
        case 7:
            track.oscillators[0].rate = param.value;
            break;
        case 8:
            track.oscillators[0].vertex = param.value;
            break;
        case 9:
            track.oscillators[1].width = param.value;
            break;
        case 10:
            track.oscillators[1].rate = param.value;
            break;
        default:
            track.update_flags |= 1u << i;
            break;
        }
    }
    track.update_flags |= kUpdatePitch;
}

void Tracks::UpdateSeq(uint32_t index, uint32_t flags, bool recursive)
{
    // JASTrack::updateSeq (0x3dcbc4).
    Track& track = tracks_[index];
    uint32_t update = flags | track.update_flags;
    if (track.outer != nullptr)
    {
        update |= track.outer->outer_update;
        track.outer->outer_update = 0;
    }
    track.vibrate.IncCounter();
    track.update_flags = 0;
    if (update != 0)
    {
        UpdateTrack(index, update);
    }

    for (const uint32_t child : tracks_[index].children)
    {
        if (child == Track::kNone || tracks_[child].status == Track::kClosed)
        {
            continue;
        }
        if (recursive)
        {
            UpdateSeq(child, update, true);
        }
        else
        {
            tracks_[child].update_flags |= update;
        }
    }
}

void Tracks::UpdateTrack(uint32_t index, uint32_t flags)
{
    // JASTrack::updateTrack (0x3ddcbc).
    Track& track = tracks_[index];
    const OuterParam* const outer = track.outer;
    if ((flags & kUpdateDelay) != 0)
    {
        const std::array<uint8_t, 2> delays = DelaySamples(track.timed[kTimedDelay].value);
        track.updater.delays[0] = delays[0];
        track.updater.delays[1] = delays[1];
    }
    if ((flags & kUpdateTempo) != 0 && track.parent == Track::kNone)
    {
        UpdateTempo(index);
    }

    float volume = 0.0f;
    if ((flags & kUpdateVolume) != 0)
    {
        volume = track.timed[kTimedVolume].value;
        if (track.volume_mode == 0)
        {
            volume = volume * volume;
        }
        if (track.muted)
        {
            volume = 0.0f;
        }
        if (outer != nullptr && outer->Check(OuterParam::kVolume))
        {
            volume = volume * outer->volume;
        }
        if (track.paused && (track.pause_status & 1) != 0)
        {
            volume = volume * track.timed[kTimedPause].value;
        }
    }

    float pitch = 1.0f;
    if ((flags & kUpdatePitch) != 0)
    {
        pitch = PitchToCent(track.timed[kTimedPitch].value, static_cast<float>(track.registers[kRegBendRange]));
        pitch = pitch * track.vibrate.Value();
        if (outer != nullptr && outer->Check(OuterParam::kPitch))
        {
            pitch = pitch * outer->pitch;
        }
    }

    const float weight = static_cast<float>(track.registers[kRegOuterWeight]) / kShortMax;
    float pan = 0.0f;
    if ((flags & kUpdatePan) != 0)
    {
        pan = track.timed[kTimedPan].value;
        if (outer != nullptr && outer->Check(OuterParam::kPan))
        {
            pan = PanCalc(pan, outer->pan, weight, track.pan_calc);
        }
    }
    float fxmix = 0.0f;
    if ((flags & kUpdateFxmix) != 0)
    {
        fxmix = track.timed[kTimedFxmix].value;
        if (outer != nullptr && outer->Check(OuterParam::kFxmix))
        {
            fxmix = PanCalc(fxmix, outer->fxmix, weight, track.fxmix_calc);
        }
    }
    float dolby = 0.0f;
    if ((flags & kUpdateDolby) != 0)
    {
        dolby = track.timed[kTimedDolby].value;
        if (outer != nullptr && outer->Check(OuterParam::kDolby))
        {
            dolby = PanCalc(dolby, outer->dolby, weight, track.dolby_calc);
        }
    }

    if ((flags & kUpdateIir) != 0)
    {
        for (std::size_t i = 0; i < 4; i++)
        {
            track.updater.iir[i] = static_cast<int16_t>(ToInt32(track.timed[kTimedIir + i].value * kShortMax));
        }
        track.updater.filter_mode = static_cast<uint8_t>(track.updater.filter_mode | 0x20);
    }
    if ((flags & kUpdateFir) != 0 && outer != nullptr && outer->Check(OuterParam::kFir))
    {
        track.updater.fir = outer->fir;
        track.updater.filter_mode = static_cast<uint8_t>((track.updater.filter_mode & 0x20) | 8);
    }
    if ((flags & kUpdateDistortion) != 0)
    {
        track.updater.distortion = static_cast<int16_t>(ToInt32(track.timed[kTimedDistortion].value * kShortMax));
    }

    if (track.parent == Track::kNone || (track.inherit & 1) != 0)
    {
        if ((flags & kUpdateVolume) != 0)
        {
            track.updater.volume = volume;
        }
        if ((flags & kUpdatePitch) != 0)
        {
            track.updater.pitch = pitch;
        }
        if ((flags & kUpdatePan) != 0)
        {
            track.updater.pan = pan;
        }
        if ((flags & kUpdateFxmix) != 0)
        {
            track.updater.fxmix = fxmix;
        }
        if ((flags & kUpdateDolby) != 0)
        {
            track.updater.dolby = dolby;
        }
        return;
    }

    const ChannelUpdater& from = tracks_[track.parent].updater;
    const float parent_weight = static_cast<float>(track.registers[kRegParentWeight]) / kShortMax;
    if ((flags & kUpdateVolume) != 0)
    {
        track.updater.volume = volume * from.volume;
    }
    if ((flags & kUpdatePitch) != 0)
    {
        track.updater.pitch = pitch * from.pitch;
    }
    if ((flags & kUpdatePan) != 0)
    {
        track.updater.pan = PanCalc(pan, from.pan, parent_weight, track.parent_pan_calc);
    }
    if ((flags & kUpdateFxmix) != 0)
    {
        track.updater.fxmix = PanCalc(fxmix, from.fxmix, parent_weight, track.parent_fxmix_calc);
    }
    if ((flags & kUpdateDolby) != 0)
    {
        track.updater.dolby = PanCalc(dolby, from.dolby, parent_weight, track.parent_dolby_calc);
    }
}

void Tracks::UpdateTrackAll(uint32_t index)
{
    // JASTrack::updateTrackAll (0x3dd770).
    Track& track = tracks_[index];
    const OuterParam* const outer = track.outer;
    const std::array<uint8_t, 2> delays = DelaySamples(track.timed[kTimedDelay].value);
    track.updater.delay_max = 16;
    track.updater.delays[0] = delays[0];
    track.updater.delays[1] = delays[1];

    float volume = track.timed[kTimedVolume].value;
    if (track.volume_mode == 0)
    {
        volume = volume * volume;
    }
    if (track.muted)
    {
        volume = 0.0f;
    }
    float pitch = PitchToCent(track.timed[kTimedPitch].value, static_cast<float>(track.registers[kRegBendRange]));
    pitch = pitch * track.vibrate.Value();
    float pan = track.timed[kTimedPan].value;
    float fxmix = track.timed[kTimedFxmix].value;
    float dolby = track.timed[kTimedDolby].value;

    if (outer != nullptr)
    {
        const float weight = static_cast<float>(track.registers[kRegOuterWeight]) / kShortMax;
        if (outer->Check(OuterParam::kVolume))
        {
            volume = volume * outer->volume;
        }
        if (outer->Check(OuterParam::kPitch))
        {
            pitch = pitch * outer->pitch;
        }
        if (outer->Check(OuterParam::kFxmix))
        {
            fxmix = PanCalc(fxmix, outer->fxmix, weight, track.fxmix_calc);
        }
        if (outer->Check(OuterParam::kDolby))
        {
            dolby = PanCalc(dolby, outer->dolby, weight, track.dolby_calc);
        }
        if (outer->Check(OuterParam::kPan))
        {
            pan = PanCalc(pan, outer->pan, weight, track.pan_calc);
        }
    }

    if (track.parent == Track::kNone || (track.inherit & 1) != 0)
    {
        track.updater.volume = volume;
        track.updater.pitch = pitch;
        track.updater.pan = pan;
        track.updater.fxmix = fxmix;
        track.updater.dolby = dolby;
        return;
    }

    const ChannelUpdater& from = tracks_[track.parent].updater;
    const float parent_weight = static_cast<float>(track.registers[kRegParentWeight]) / kShortMax;
    track.updater.volume = volume * from.volume;
    track.updater.pitch = pitch * from.pitch;
    track.updater.pan = PanCalc(pan, from.pan, parent_weight, track.parent_pan_calc);
    track.updater.fxmix = PanCalc(fxmix, from.fxmix, parent_weight, track.parent_fxmix_calc);
    track.updater.dolby = PanCalc(dolby, from.dolby, parent_weight, track.parent_dolby_calc);
    if (outer != nullptr && outer->Check(OuterParam::kFir))
    {
        track.updater.fir = outer->fir;
        track.updater.filter_mode = 8;
    }
    for (std::size_t i = 0; i < 4; i++)
    {
        track.updater.iir[i] = static_cast<int16_t>(ToInt32(track.timed[kTimedIir + i].value * kShortMax));
    }
    track.updater.filter_mode = static_cast<uint8_t>(track.updater.filter_mode | 0x20);
    track.updater.distortion = static_cast<int16_t>(ToInt32(track.timed[kTimedDistortion].value * kShortMax));
}

void Tracks::UpdateTempo(uint32_t index)
{
    // JASTrack::updateTempo (0x3dc42c).
    Track& track = tracks_[index];
    if (track.parent == Track::kNone)
    {
        float rate = static_cast<float>(track.timebase) * static_cast<float>(track.tempo);
        rate = rate / voices_.DacRate();
        rate = rate * 1.33333337f;
        if (track.outer != nullptr && track.outer->Check(OuterParam::kTempo))
        {
            rate = track.outer->tempo * rate;
        }
        track.tempo_rate = rate;
    }
    else
    {
        track.tempo_rate = tracks_[track.parent].tempo_rate;
        track.timebase = tracks_[track.parent].timebase;
    }

    for (const uint32_t child : tracks_[index].children)
    {
        if (child != Track::kNone && tracks_[child].status != Track::kClosed)
        {
            UpdateTempo(child);
        }
    }
}

void Tracks::ReleaseChannels(uint32_t index)
{
    // The release of every channel in the track's list, which then ends the channels' callbacks (~JASTrack's loop).
    Track& track = tracks_[index];
    for (const uint32_t channel : track.channel_list)
    {
        voices_.Release(channel, 0);
        voices_.Free(channel);
    }
    track.channel_list.clear();
}

void Tracks::Close(uint32_t index)
{
    // JASTrack::close (0x3dcfd8). A root track releases its notes with a direct release of 10, a child with its
    // envelopes' release.
    Track& track = tracks_[index];
    if (track.status == Track::kClosed)
    {
        return;
    }

    const uint16_t release = track.parent == Track::kNone ? 10 : 0;
    for (uint32_t& channel : track.channels)
    {
        if (channel != Track::kNone)
        {
            voices_.Release(channel, release);
            channel = Track::kNone;
        }
    }
    track.status = Track::kClosed;
    for (std::size_t slot = 0; slot < 16; slot++)
    {
        const uint32_t child = tracks_[index].children[slot];
        if (child != Track::kNone)
        {
            Close(child);
            tracks_[index].children[slot] = Track::kNone;
        }
    }

    Track& closed = tracks_[index];
    closed.muted = false;
    ReleaseChannels(index);
    if (closed.from_pool)
    {
        FreeTrack(index);
    }
}

void Tracks::Start(uint32_t index, const Sequence* sequence, uint32_t offset)
{
    // JASTrack::start (0x3de86c).
    Track& track = tracks_[index];
    track.sequence = sequence;
    track.cursor = offset;
    track.status = Track::kPlaying;
    UpdateTrackAll(index);
}

uint32_t Tracks::OpenChild(uint32_t index, uint32_t slot, uint32_t inherit)
{
    // JASTrack::openChild (0x3de8a4).
    slot &= 0xff;
    if (slot >= 16)
    {
        unmodelled_++;
        return Track::kNone;
    }
    const uint32_t old = tracks_[index].children[slot];
    if (old != Track::kNone)
    {
        Close(old);
        tracks_[index].children[slot] = Track::kNone;
    }

    const uint32_t child = AllocTrack();
    if (child == Track::kNone)
    {
        return Track::kNone;
    }

    Track& track = tracks_[child];
    Construct(track);
    track.allocated = true;
    Init(child);
    track.from_pool = true;
    track.parent = index;
    track.inherit = static_cast<uint8_t>(inherit);
    const uint32_t route = tracks_[index].route;
    track.route = (((route << 4) & 0x0ffffff0u) | (slot & 0xff)) | ((route + 0x10000000u) & 0xf0000000u);
    tracks_[index].children[slot] = child;
    notes_[child] = 0;
    loop_notes_[child] = 0;
    looped_[child] = false;
    quiet_[child] = false;
    Inherit(child);

    return child;
}

uint8_t Tracks::ReadByte(Track& track)
{
    return track.sequence->U8(track.cursor++);
}

uint16_t Tracks::Read16(Track& track)
{
    const uint32_t high = ReadByte(track);
    const uint32_t low = ReadByte(track);

    return static_cast<uint16_t>(high << 8 | low);
}

uint32_t Tracks::Read24(Track& track)
{
    const uint32_t high = ReadByte(track);
    const uint32_t middle = ReadByte(track);
    const uint32_t low = ReadByte(track);

    return high << 16 | middle << 8 | low;
}

uint16_t Tracks::Get16(const Track& track, uint32_t offset) const
{
    return static_cast<uint16_t>(track.sequence->U8(offset) << 8 | track.sequence->U8(offset + 1));
}

uint32_t Tracks::Get24(const Track& track, uint32_t offset) const
{
    return static_cast<uint32_t>(track.sequence->U8(offset)) << 16 |
           static_cast<uint32_t>(track.sequence->U8(offset + 1)) << 8 | track.sequence->U8(offset + 2);
}

uint32_t Tracks::Get32(const Track& track, uint32_t offset) const
{
    return Get24(track, offset) << 8 | track.sequence->U8(offset + 3);
}

uint16_t Tracks::Register(const Track& track, uint32_t reg)
{
    if (reg < track.registers.size())
    {
        return track.registers[reg];
    }

    // JASTrack's words after the registers on AArch64, low word first: the two oscillators' settings
    // (JASOscillator::Data, 16 words each: the target, the rate, the addresses of the two tables, the width and the
    // vertex), their routes, then the points of a simple ADSR. A table's address is 0 while the table is null.
    // Otherwise it depends on where the game's memory holds the table.
    const auto word = static_cast<uint32_t>(reg - track.registers.size());
    const auto half = [](uint32_t value, uint32_t at)
    {
        return static_cast<uint16_t>(at % 2 == 0 ? value : value >> 16);
    };
    if (word < 32)
    {
        const OscData& data = track.oscillators[word / 16];
        const uint32_t at = word % 16;
        if (at < 2)
        {
            return half(data.target, at);
        }
        if (at < 4)
        {
            return half(std::bit_cast<uint32_t>(data.rate), at);
        }
        if (at >= 12)
        {
            return half(std::bit_cast<uint32_t>(at < 14 ? data.width : data.vertex), at);
        }
        if ((at < 8 ? data.attack : data.release).Null())
        {
            return 0;
        }
    }
    else if (word < 36)
    {
        return half(track.osc_routes[(word - 32) / 2], word);
    }
    else if (word < 48)
    {
        const OscPoint& point = track.adsr[(word - 36) / 3];
        const std::array<int16_t, 3> values = {point.mode, point.time, point.value};
        return static_cast<uint16_t>(values[(word - 36) % 3]);
    }

    unmodelled_++;
    return 0;
}

void Tracks::SetRegister(Track& track, uint32_t reg, uint32_t value)
{
    if (reg < track.registers.size())
    {
        track.registers[reg] = static_cast<uint16_t>(value);
        return;
    }
    unmodelled_++;
}

uint32_t Tracks::ReadReg16(uint32_t index, uint32_t reg)
{
    // JASTrack::readReg16 (0x3debb8).
    const Track& track = tracks_[index];
    switch (reg & 0xff)
    {
    case 0x20:
        return track.registers[kRegBankProgram] >> 8;

    case 0x21:
        return track.registers[kRegBankProgram] & 0xff;

    case 0x22:
        return static_cast<uint32_t>(track.registers[0]) << 8 | track.registers[1];

    case 0x2c:
        {
            uint32_t result = 0;
            for (std::size_t slot = 16; slot-- > 0;)
            {
                const uint32_t child = track.children[slot];
                result = result << 1 | (child != Track::kNone && tracks_[child].status != Track::kClosed ? 1 : 0);
            }
            return result;
        }

    case 0x2d:
        {
            uint32_t result = 0;
            for (std::size_t note = 8; note-- > 0;)
            {
                const uint32_t channel = track.channels[note];
                result = result << 1 |
                         (channel == Track::kNone || voices_.ChannelAt(channel).status == Channel::kStop ? 1 : 0);
            }
            return result;
        }

    case 0x30:
        return track.depth == 0 ? 0 : track.loop_counts[track.depth - 1];

    default:
        return Register(track, reg & 0xff);
    }
}

uint32_t Tracks::ReadReg32(uint32_t index, uint32_t reg)
{
    // JASTrack::readReg32 (0x3deb60).
    reg &= 0xff;
    const Track& track = tracks_[index];
    if (reg >= 0x28 && reg <= 0x2b)
    {
        const uint32_t at = kRegLong + 2 * (reg - 0x28);
        return static_cast<uint32_t>(track.registers[at]) << 16 | track.registers[at + 1];
    }
    if (reg == 0x23)
    {
        return static_cast<uint32_t>(track.registers[kRegProduct]) << 16 | track.registers[kRegProduct + 1];
    }

    return ReadReg16(index, reg) & 0xffff;
}

uint32_t Tracks::ExchangeRegisterValue(uint32_t index, uint32_t reg)
{
    // JASTrack::exchangeRegisterValue (0x3deaec): registers below 0x40, ports from 0x40.
    reg &= 0xff;
    if (reg < 0x40)
    {
        return ReadReg32(index, reg);
    }

    const uint32_t port = reg - 0x40;
    if (port < 16)
    {
        return tracks_[index].ports[port];
    }

    // A higher number reads past the ports, into the fields that follow them in JASTrack. Registers 0x58 to 0x77 read
    // JASIntrMgr's targets, 4 words each, low word first: 0 for a target that the track hasn't set.
    constexpr uint32_t kTargetWords = 24;
    if (port >= kTargetWords && port < kTargetWords + 4 * 8 &&
        (tracks_[index].targets_set & (1u << ((port - kTargetWords) / 4))) == 0)
    {
        return 0;
    }
    unmodelled_++;

    return 0;
}

void Tracks::WriteRegDirect(Track& track, uint32_t reg, uint32_t value)
{
    // JASTrack::writeRegDirect (0x3def8c). Register 3 takes the value too, or the byte's sign extension for registers
    // 0 to 2.
    reg &= 0xff;
    value &= 0xffff;
    if (reg < 3)
    {
        SetRegister(track, reg, value & 0xff);
        SetRegister(track, kRegResult, Extend8To16(value));
        return;
    }
    if (reg == 0x20 || reg == 0x21)
    {
        return;
    }
    if (reg == 0x22)
    {
        SetRegister(track, 0, value >> 8);
        SetRegister(track, kRegResult, Extend8To16(value >> 8));
        SetRegister(track, 1, value & 0xff);
        SetRegister(track, kRegResult, value);
        return;
    }
    SetRegister(track, reg, value);
    SetRegister(track, kRegResult, value);
}

void Tracks::WriteRegParam(uint32_t index, uint32_t command)
{
    // JASTrack::writeRegParam (0x3df01c): an operation on a register. The command's low bits, or a byte after it,
    // give the operation and the kind of operand.
    Track& track = tracks_[index];
    uint32_t operation = command & 0xf;
    uint32_t operand_kind = 0;
    uint32_t table_kind = 0;
    if (operation == 9)
    {
        const uint32_t byte = ReadByte(track);
        operand_kind = (byte & 0xc) == 8 ? 0x10 : byte & 0xc;
        operation = byte & 0xf0;
    }
    else if (operation == 0xb)
    {
        operand_kind = 0;
    }
    else if (operation == 0xa)
    {
        const uint32_t byte = ReadByte(track);
        operand_kind = byte & 0xc;
        table_kind = (byte >> 4) + 4;
    }
    else
    {
        operand_kind = command & 0xc;
        operation = command & 3;
    }

    const uint32_t target = ReadByte(track);
    uint32_t table = 0;
    if (operation == 0xa)
    {
        table = ReadReg32(index, ReadByte(track));
    }

    uint32_t operand = 0;
    switch (operand_kind >> 2)
    {
    case 0:
        operand = ReadReg16(index, ReadByte(track));
        break;

    case 1:
        operand = ReadByte(track);
        break;

    case 2:
        {
            const uint32_t byte = ReadByte(track);
            operand = byte << 8 | ((byte & 0x80) == 0 ? byte << 1 : 0);
            break;
        }

    case 3:
        operand = Read16(track);
        break;

    default:
        operand = 0xffff;
        break;
    }

    // The game reads the target whatever the operation. A target past the registers reads the memory after them, and
    // a stored value that depends on it is unmodelled. `last` is the value that the game's code holds in its first
    // register when it reaches the store: the 32-bit value that a 32-bit register takes.
    const bool known = ReadableRegister16(target);
    const uint32_t value = known ? ReadReg16(index, target) : 0;
    const bool result_reads_target = OperationReadsTarget(operation);
    if (!known && (operation == 2 || operation == 3))
    {
        unmodelled_++;
    }
    uint32_t last = value;
    bool last_reads_target = true;
    uint32_t result = operand;
    switch (operation)
    {
    case 0:
        break;

    case 1:
        if (operand_kind == 4)
        {
            result = Extend8To16(result);
            last = result;
            last_reads_target = false;
        }
        result = result + value;
        break;

    case 2:
        {
            const uint32_t product = static_cast<uint32_t>(static_cast<int16_t>(value) * static_cast<int16_t>(operand));
            SetRegister(track, kRegProduct, product >> 16);
            SetRegister(track, kRegProduct + 1, product);
            SetRegister(track, kRegResult, product);
            return;
        }

    case 3:
        SetRegister(track, kRegResult, value - operand);
        return;

    case 0xa:
        {
            const int32_t at = static_cast<int16_t>(operand);
            switch (table_kind)
            {
            case 4:
                result = track.sequence->U8(table + static_cast<uint32_t>(at));
                break;
            case 5:
                result = Get16(track, table + static_cast<uint32_t>(at * 2));
                break;
            case 6:
                result = Get24(track, static_cast<uint32_t>(at * 3) + table);
                break;
            case 7:
                result = Get32(track, table + static_cast<uint32_t>(at * 4));
                break;
            case 8:
                result = Get32(track, table + static_cast<uint32_t>(at));
                break;
            default:
                break;
            }
            last = result;
            last_reads_target = false;
            break;
        }

    case 0xb:
        result = value - operand;
        break;

    case 0x10:
        {
            if (operand_kind == 4)
            {
                result = Extend8To16(result);
                last = result;
                last_reads_target = false;
            }
            const int32_t shift = static_cast<int16_t>(result);
            const uint32_t base = value & 0xffff;
            result = shift < 0 ? base >> ((0u - static_cast<uint32_t>(shift)) & 31)
                               : base << (static_cast<uint32_t>(shift) & 31);
            break;
        }

    case 0x20:
        {
            if (operand_kind == 4)
            {
                result = Extend8To16(result);
                last = result;
                last_reads_target = false;
            }
            const int32_t shift = static_cast<int16_t>(result);
            const int32_t base = static_cast<int16_t>(value);
            result = shift < 0 ? static_cast<uint32_t>(base >> ((0u - static_cast<uint32_t>(shift)) & 31))
                               : static_cast<uint32_t>(base) << (static_cast<uint32_t>(shift) & 31);
            break;
        }

    case 0x30:
        result = value & result;
        break;

    case 0x40:
        result = value | result;
        break;

    case 0x50:
        result = value ^ result;
        break;

    case 0x60:
        result = 0u - value;
        break;

    case 0x90:
        {
            last = static_cast<uint32_t>(GetRandomS32());
            random_draws_++;
            last_reads_target = false;
            const uint32_t divisor = result & 0xffff;
            result = divisor == 0 ? last : last % divisor;
            break;
        }

    default:
        break;
    }

    const bool long_target = target >= 0x28 && target <= 0x2b;
    if (!known && (long_target ? last_reads_target : result_reads_target))
    {
        unmodelled_++;
    }

    switch (target)
    {
    case 0:
    case 1:
    case 2:
        SetRegister(track, target, result & 0xff);
        SetRegister(track, kRegResult, Extend8To16(result));
        return;

    case 0x20:
        SetRegister(track, kRegBankProgram, (result << 8) | (track.registers[kRegBankProgram] & 0xff));
        SetRegister(track, kRegResult, 0);
        track.osc_routes = {0xf, 0xf};
        return;

    case 0x21:
        SetRegister(track, kRegBankProgram, (track.registers[kRegBankProgram] & 0xff00) | (result & 0xff));
        SetRegister(track, kRegResult, 0);
        track.osc_routes = {0xf, 0xf};
        return;

    case 0x22:
        SetRegister(track, 0, (result >> 8) & 0xff);
        SetRegister(track, kRegResult, Extend8To16(result >> 8));
        SetRegister(track, 1, result & 0xff);
        SetRegister(track, kRegResult, result & 0xff);
        return;

    case 0x28:
    case 0x29:
    case 0x2a:
    case 0x2b:
        {
            const uint32_t at = kRegLong + 2 * (target - 0x28);
            track.registers[at] = static_cast<uint16_t>(last >> 16);
            track.registers[at + 1] = static_cast<uint16_t>(last);
            return;
        }

    case 0x2e:
        SetRegister(track, kRegPriority, (track.registers[kRegPriority] & 0xff00) | (result & 0xff));
        SetRegister(track, kRegResult, 0);
        return;

    case 0x2f:
        SetRegister(track, kRegPriority, (result << 8) | (track.registers[kRegPriority] & 0xff));
        SetRegister(track, kRegResult, 0);
        return;

    default:
        break;
    }

    SetRegister(track, target, result);
    SetRegister(track, kRegResult, result);
    if (target == kRegBendRange)
    {
        track.update_flags |= kUpdatePitch;
    }
    else if (target == kRegBankProgram)
    {
        track.osc_routes = {0xf, 0xf};
    }
}

int32_t Tracks::GetRandomS32()
{
    // JASPlayer::getRandomS32 (0x3e1430).
    const uint32_t v0 = static_cast<uint32_t>(seq_random_[0]);
    const uint32_t v1 = static_cast<uint32_t>(seq_random_[1]);
    const uint32_t high = (v1 * 0x98765432u) | 1u;
    const int32_t low = static_cast<int32_t>(v0 * 0x13579bdeu) >> 4;
    const auto result = static_cast<int32_t>(high + static_cast<uint32_t>(low));
    seq_random_[0] = static_cast<int32_t>(v1);
    seq_random_[1] = result;

    return result;
}

bool Tracks::Condition(const Track& track, uint32_t flag) const
{
    // JASSeqParser::conditionCheck on register 3.
    const auto value = static_cast<int16_t>(track.registers[kRegResult]);
    switch (flag & 0xf)
    {
    case 0:
        return true;
    case 1:
        return value == 0;
    case 2:
        return value != 0;
    case 3:
        return value == 1;
    case 4:
        return value < 0;
    case 5:
        return value >= 0;
    default:
        return false;
    }
}

int32_t Tracks::Transpose(const Track& track) const
{
    // JASTrack::getTranspose: the sum of the transposes up the tree.
    int32_t sum = track.transpose;
    for (uint32_t at = track.parent; at != Track::kNone; at = tracks_[at].parent)
    {
        sum += tracks_[at].transpose;
    }

    return sum;
}

int32_t Tracks::SeqTimeToDspTime(const Track& track, int32_t time, uint32_t percent) const
{
    // JASTrack::seqTimeToDspTime (0x3de2b4).
    float value = static_cast<float>(time) * static_cast<float>(percent & 0xff);
    value = value / 100.0f;
    if (track.time_relate)
    {
        return ToInt32(value / track.tempo_rate);
    }

    value = value * (120.0f / static_cast<float>(track.timebase));
    return ToInt32(value * (static_cast<float>(Voices::kSubframes) / 10.0f));
}

void Tracks::SetParam(Track& track, uint32_t param, float value, int32_t time)
{
    // JASTrack::setParam (0x3de338).
    if (param >= track.timed.size())
    {
        unmodelled_++;
        return;
    }
    MoveParam& move = track.timed[param];
    move.target = value;
    if (time <= 0)
    {
        move.value = value;
        move.time = 1.0f;
        move.amount = 0.0f;
        return;
    }
    move.time = static_cast<float>(time);
    move.amount = (value - move.value) / move.time;
}

void Tracks::SetTempo(uint32_t index, uint32_t tempo)
{
    // JASTrack::setTempo.
    Track& track = tracks_[index];
    track.tempo = static_cast<uint16_t>(tempo);
    if (track.parent == Track::kNone)
    {
        UpdateTempo(index);
    }
    else
    {
        track.tempo_child = true;
    }
}

void Tracks::SetTimebase(uint32_t index, uint32_t timebase)
{
    // JASTrack::setTimebase.
    Track& track = tracks_[index];
    track.timebase = static_cast<uint16_t>(timebase);
    if (track.parent == Track::kNone)
    {
        UpdateTempo(index);
    }
}

OscTable Tracks::SequenceTable(const Track& track, uint32_t offset) const
{
    const std::span<const OscPoint> points = track.sequence->Table(offset);

    return {points.data(), points.size()};
}

OuterParam* Tracks::SequenceOuter(const Sequence* sequence, uint32_t offset)
{
    return &sequence_outer_[{sequence, offset}];
}

void Tracks::OscSetupFull(Track& track, uint32_t route, uint32_t attack, uint32_t release)
{
    // JASTrack::oscSetupFull (0x3dd5f0). An offset of 0 names the sequence's start, as any other offset does.
    OscData& data = track.oscillators[(route >> 4) & 1];
    if ((route & 0x80) != 0)
    {
        data = kEnvelopeDef;
        data.target = route & 0xf;
        if ((route & 0xf) == 1)
        {
            data.vertex = 1.0f;
        }
    }
    if ((route & 0x40) != 0)
    {
        data.attack = SequenceTable(track, attack);
    }
    if ((route & 0x20) != 0)
    {
        data.release = SequenceTable(track, release);
    }
}

void Tracks::OscSetupSimpleEnv(Track& track, uint32_t kind, uint32_t offset)
{
    // JASTrack::oscSetupSimpleEnv (0x3dd6b0).
    switch (kind & 0xff)
    {
    case 0:
        track.oscillators[0] = kEnvelopeDef;
        track.oscillators[0].attack = SequenceTable(track, offset);
        break;

    case 1:
        track.oscillators[0].release = SequenceTable(track, offset);
        break;

    default:
        break;
    }
}

void Tracks::OscSetupSimple(Track& track, uint32_t kind)
{
    // JASTrack::oscSetupSimple (0x3dd704).
    switch (kind & 0xff)
    {
    case 0:
        track.oscillators[1] = kVibratoDef;
        break;
    case 1:
        track.oscillators[0] = kTremoroDef;
        break;
    case 2:
        track.oscillators[1] = kTremoroDef;
        break;
    default:
        break;
    }
}

void Tracks::OverwriteOsc(Track& track, uint32_t channel)
{
    // JASTrack::overwriteOsc (0x3dd4c4): a route other than 0xf gives the channel's oscillator `route & 3` the
    // track's settings. With bit 3, the track first takes the oscillator's settings. With bit 2, it takes them but
    // keeps its release.
    Channel& ch = voices_.ChannelAt(channel);
    for (std::size_t i = 0; i < 2; i++)
    {
        const uint32_t route = track.osc_routes[i];
        if (route == 0xf)
        {
            continue;
        }

        Oscillator& oscillator = ch.oscillators[route & 3];
        OscData& data = track.oscillators[i];
        if ((route & 8) != 0)
        {
            if (oscillator.Data() != nullptr)
            {
                data = *oscillator.Data();
            }
        }
        else if ((route & 4) != 0)
        {
            const OscTable release = data.release;
            if (oscillator.Data() != nullptr)
            {
                data = *oscillator.Data();
            }
            data.release = release;
        }
        oscillator.InitStart(&data, voices_.DacRate());
    }
}

uint32_t Tracks::BankNoteOn(uint32_t bank, uint32_t program, uint32_t key, uint32_t velocity, uint16_t priority,
                            uint32_t owner)
{
    // JASBankMgr::noteOn (0x3d0c90).
    if (program >= 0xf0)
    {
        // noteOnOsc: a note on the DSP's oscillator, waveform `program - 0xf0`.
        const uint32_t channel = voices_.NewChannel(owner);
        if (channel == Channel::kNone)
        {
            return Channel::kNone;
        }

        voices_.SetPanPower(channel, 1.0f, 1.0f, 1.0f);
        Channel& ch = voices_.ChannelAt(channel);
        ch.priority = priority;
        ch.wave_address = program - 0xf0;
        ch.wave_format = Channel::kOsc;
        ch.active_pitch = 16736.0156f / voices_.DacRate();
        ch.modified_pitch = ch.active_pitch;
        ch.modified_pitch = Key2Pitch(static_cast<int32_t>(key & 0xff)) * ch.modified_pitch;
        ch.volume = 1.0f;
        const float v = static_cast<float>(velocity & 0xff) / 127.0f;
        ch.velocity_volume = v * v;
        ch.oscillators[0].InitStart(&kOscEnv, voices_.DacRate());

        return voices_.Play(this, channel) ? channel : Channel::kNone;
    }

    const InstrumentBank* const instruments = banks_[bank & 0xff];
    if (instruments == nullptr)
    {
        missing_bank_notes_++;
        return Channel::kNone;
    }
    if (!instruments->HasProgram(program))
    {
        return Channel::kNone;
    }

    InstParam param;
    if (!instruments->GetInstParam(program, key & 0xff, velocity & 0xff, param, inst_random_))
    {
        return Channel::kNone;
    }

    const WaveBank* const waves = instruments->Waves();
    if (waves == nullptr)
    {
        return Channel::kNone;
    }

    const BankWave* const wave = waves->Find(param.wave_id);
    if (wave == nullptr)
    {
        return Channel::kNone;
    }

    const uint32_t channel = voices_.NewChannel(owner);
    if (channel == Channel::kNone)
    {
        return Channel::kNone;
    }

    voices_.SetPanPower(channel, 1.0f, 1.0f, 1.0f);
    Channel& ch = voices_.ChannelAt(channel);
    ch.priority = priority;
    ch.has_wave = true;
    ch.wave = wave->info;
    ch.wave_address = wave->address;
    ch.wave_format = param.wave_format;
    ch.active_pitch = (wave->info.sample_rate / voices_.DacRate()) * param.pitch;
    ch.modified_pitch = ch.active_pitch;
    if (!param.percussion)
    {
        const int32_t semitones = static_cast<int32_t>(key & 0xff) - wave->info.base_key + 60;
        ch.modified_pitch = Key2Pitch(semitones) * ch.modified_pitch;
    }
    const float v = static_cast<float>(velocity & 0xff) / 127.0f;
    ch.volume = param.volume;
    ch.velocity_volume = (v * v) * param.volume;
    ch.pan_sound = param.pan;
    ch.fxmix_sound = param.fxmix;
    ch.dolby_sound = param.dolby;
    for (uint32_t i = 0; i < param.oscillator_count; i++)
    {
        ch.oscillators[i].InitStart(param.oscillators[i], voices_.DacRate());
    }
    ch.oscillators[0].SetDirectRelease(param.release);

    return voices_.Play(this, channel) ? channel : Channel::kNone;
}

int32_t Tracks::NoteOn(uint32_t index, uint32_t note, uint32_t key, uint32_t velocity, int32_t time, uint32_t skip)
{
    // JASTrack::noteOn (0x3dd330).
    Track& track = tracks_[index];
    note &= 0xff;
    if (note >= 8)
    {
        unmodelled_++;
        return -1;
    }
    if ((track.muted && (track.pause_status & 0x40) != 0) || ((track.note_mask >> note) & 1) != 0)
    {
        return -1;
    }
    if (track.channels[note] != Track::kNone)
    {
        voices_.Release(track.channels[note], 0);
        track.channels[note] = Track::kNone;
    }

    const uint32_t bank_program = track.registers[kRegBankProgram];
    const uint32_t physical = virtual_banks_[bank_program >> 8];
    const uint32_t channel =
        BankNoteOn(physical, bank_program & 0xff, key, velocity, track.registers[kRegPriority], index);
    if (channel == Channel::kNone)
    {
        return -1;
    }

    notes_[index]++;
    Channel& ch = voices_.ChannelAt(channel);
    ch.update_timer = static_cast<uint32_t>(time);
    tracks_[index].channel_list.push_back(channel);
    tracks_[index].channels[note] = channel;
    ch.skip_samples = skip;
    voices_.SetPanPower(channel, static_cast<float>(track.registers[kRegPanPower]),
                        static_cast<float>(track.registers[kRegPanPower + 1]),
                        static_cast<float>(track.registers[kRegPanPower + 2]));
    OverwriteOsc(tracks_[index], channel);
    if (track.direct_release != 0)
    {
        ch.oscillators[0].SetDirectRelease(track.direct_release);
    }

    return 0;
}

bool Tracks::NoteOff(uint32_t index, uint32_t note, uint16_t release)
{
    // JASTrack::noteOff (0x3dd48c).
    Track& track = tracks_[index];
    if (track.channels[note & 7] == Track::kNone)
    {
        return false;
    }

    voices_.Release(track.channels[note & 7], release);
    track.channels[note & 7] = Track::kNone;

    return true;
}

void Tracks::GateOn(uint32_t index, uint32_t note, uint32_t key, uint32_t velocity, int32_t time)
{
    // JASTrack::gateOn (0x3dd5a0) and JASBankMgr::gateOn (0x3d1040): the playing channel takes the new key and
    // velocity.
    Track& track = tracks_[index];
    note &= 0xff;
    if (note >= 8)
    {
        unmodelled_++;
        return;
    }
    const uint32_t channel = track.channels[note];
    if (channel == Track::kNone)
    {
        return;
    }

    Channel& ch = voices_.ChannelAt(channel);
    int32_t semitones = static_cast<int32_t>(key & 0xff);
    if (ch.wave_format != Channel::kOsc)
    {
        semitones = semitones - ch.wave.base_key + 60;
    }
    ch.modified_pitch = Key2Pitch(semitones) * ch.active_pitch;
    const float v = static_cast<float>(velocity & 0xff) / 127.0f;
    ch.velocity_volume = (v * v) * ch.volume;
    ch.update_timer = static_cast<uint32_t>(time);
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

int32_t Tracks::CmdNoteOn(uint32_t index, uint32_t key)
{
    // JASSeqParser::cmdNoteOn (0x3e0de4). A note id of 0 times the note by its gate and length, and the track waits
    // for it. Notes 1 to 7 last until a note-off.
    Track& track = tracks_[index];
    const uint32_t flags = ReadByte(track);
    if ((flags & 0x80) != 0)
    {
        key = ExchangeRegisterValue(index, key);
    }
    const uint32_t original = key + static_cast<uint32_t>(Transpose(tracks_[index]));
    uint32_t play_key = original;
    if ((flags & 0x40) != 0)
    {
        play_key = tracks_[index].latest_key;
    }

    uint32_t velocity = ReadByte(tracks_[index]);
    uint32_t gate_flags = (flags >> 5) & 3;
    if ((velocity & 0x80) != 0)
    {
        velocity = ExchangeRegisterValue(index, velocity & 0x7f);
    }

    uint32_t note = flags & 7;
    const uint32_t count = (flags >> 3) & 3;
    uint32_t skip = 0;
    uint32_t percent = 100;
    auto time = static_cast<int32_t>(-1);
    if (note != 0)
    {
        if (count != 0)
        {
            note = ExchangeRegisterValue(index, note - 1);
        }
        if ((gate_flags & 1) != 0)
        {
            skip = ExchangeRegisterValue(index, ReadByte(tracks_[index]));
            gate_flags ^= 1;
        }
    }
    else
    {
        percent = ReadByte(tracks_[index]);
        if ((percent & 0x80) != 0)
        {
            percent = ExchangeRegisterValue(index, percent & 0x7f);
        }
        uint32_t length = 0;
        for (uint32_t i = 0; i < count; i++)
        {
            length = length << 8 | ReadByte(tracks_[index]);
        }
        if (count == 1 && (length & 0x80) != 0)
        {
            length = ExchangeRegisterValue(index, length & 0x7f);
        }
        time = static_cast<int32_t>(length);
    }

    Track& t = tracks_[index];
    t.note_flags = static_cast<uint8_t>(gate_flags);
    const int32_t length = time;
    int32_t dsp_time = -1;
    const bool blocked = t.paused && (t.pause_status & 0x10) != 0;
    if (t.gate != 0)
    {
        if (length != -1 && (gate_flags & 1) == 0)
        {
            dsp_time = SeqTimeToDspTime(t, length, percent);
        }
        if (!blocked)
        {
            GateOn(index, note, play_key & 0xff, velocity & 0xff, dsp_time);
        }
    }
    else
    {
        if (length != -1)
        {
            dsp_time = SeqTimeToDspTime(t, length, percent);
        }
        if ((tracks_[index].note_flags & 1) != 0)
        {
            dsp_time = -1;
        }
        if (!blocked)
        {
            NoteOn(index, note, play_key & 0xff, velocity & 0xff, dsp_time, skip);
        }
    }

    Track& after = tracks_[index];
    const uint32_t note_flags = after.note_flags;
    after.note_time = length;
    after.gate = static_cast<uint8_t>(note_flags & 1);
    uint32_t latest = play_key;
    if ((note_flags & 2) != 0)
    {
        if (dsp_time == -1)
        {
            dsp_time = SeqTimeToDspTime(after, length, percent);
        }
        if (after.channels[0] != Track::kNone)
        {
            voices_.SetKeySweepTarget(after.channels[0], original & 0xff, static_cast<uint32_t>(dsp_time));
        }
        latest = original;
    }
    after.latest_key = static_cast<uint8_t>(latest);
    if (length == -1)
    {
        return 0;
    }

    after.wait = length != 0 ? length : -1;

    return 1;
}

void Tracks::CmdSetParam(uint32_t index, uint32_t flags)
{
    // JASSeqParser::cmdSetParam (0x3e0c54): a timed level, its target as a fraction of 32767 and its time.
    Track& track = tracks_[index];
    const uint32_t param = ReadByte(track);
    uint32_t data = 0;
    switch ((flags >> 2) & 3)
    {
    case 0:
        data = ReadReg16(index, ReadByte(track));
        break;

    case 1:
        data = ReadByte(track);
        break;

    case 2:
        {
            const uint32_t byte = ReadByte(track);
            data = byte << 8 | ((byte & 0x80) == 0 ? byte << 1 : 0);
            break;
        }

    default:
        data = Read16(track);
        break;
    }

    int32_t time = -1;
    switch (flags & 3)
    {
    case 1:
        time = static_cast<int32_t>(ReadReg16(index, ReadByte(track)) & 0xffff);
        break;
    case 2:
        time = ReadByte(track);
        break;
    case 3:
        time = Read16(track);
        break;
    default:
        break;
    }

    const float value = static_cast<float>(static_cast<int16_t>(data)) / kShortMax;
    SetParam(tracks_[index], param, value, time);
}

void Tracks::CmdCall(uint32_t index)
{
    // JASSeqParser::cmdCall (0x3dfe68).
    Track& track = tracks_[index];
    const uint32_t flag = ReadByte(track);
    uint32_t target = 0;
    if ((flag & 0x80) == 0)
    {
        target = Read24(track);
    }
    else
    {
        target = ReadReg16(index, ReadByte(track)) & 0xffff;
        if ((flag & 0x40) != 0)
        {
            const uint32_t table = (flag & 0x20) != 0 ? ReadReg16(index, ReadByte(track)) & 0xffff : Read24(track);
            target = Get24(track, table + target * 3);
        }
    }

    if (Condition(track, flag))
    {
        if (track.depth >= track.stack.size())
        {
            unmodelled_++;
            return;
        }
        track.stack[track.depth++] = track.cursor;
        track.cursor = target;
    }
}

void Tracks::CmdJmp(uint32_t index)
{
    // JASSeqParser::cmdJmp (0x3e0020). A jump to a 32-bit register's value takes it as an address in memory: the
    // game asserts "not support" and jumps there anyway.
    Track& track = tracks_[index];
    const uint32_t flag = ReadByte(track);
    uint32_t target = 0;
    bool address = false;
    if ((flag & 0x80) == 0)
    {
        target = Read24(track);
    }
    else
    {
        const uint32_t reg = ReadByte(track);
        if ((flag & 0x40) != 0)
        {
            const uint32_t entry = ReadReg16(index, reg) & 0xffff;
            const uint32_t table = (flag & 0x20) != 0 ? ReadReg16(index, ReadByte(track)) & 0xffff : Read24(track);
            target = Get24(track, table + entry * 3);
        }
        else
        {
            address = (reg & 0xfc) == 0x28;
            target = ReadReg32(index, reg);
        }
    }

    if (Condition(track, flag))
    {
        if (address)
        {
            unmodelled_++;
            return;
        }
        JumpBack(index, target);
        tracks_[index].cursor = target;
    }
}

void Tracks::CmdPrintf(Track& track)
{
    // JASSeqParser::cmdPrintf (0x3e090c): the format, at most 128 characters, then a byte for each conversion. The
    // model prints nothing.
    uint32_t conversions = 0;
    for (uint32_t i = 0; i < 128; i++)
    {
        const uint32_t c = ReadByte(track);
        if (c == 0)
        {
            break;
        }
        if (c == '\\')
        {
            if (ReadByte(track) == 0)
            {
                break;
            }
            continue;
        }
        if (c != '%')
        {
            continue;
        }

        i++;
        if (ReadByte(track) == 0)
        {
            break;
        }

        conversions++;
    }
    for (uint32_t i = 0; i < conversions; i++)
    {
        ReadByte(track);
    }
}

int32_t Tracks::CmdProcess(uint32_t index, uint32_t command, uint32_t register_mask)
{
    // JASSeqParser::Cmd_Process (0x3e0ae4): the arguments by Arglist's widths, or a register's value where the mask
    // says so, then the command.
    command &= 0xff;
    if (command < 0xc0)
    {
        // The game reads the tables before their start.
        unmodelled_++;
        return 0;
    }
    const ArgSpec& spec = kArgs[command - 0xc0];
    Args args{};
    if (spec.count != 0)
    {
        uint32_t types = spec.types | register_mask;
        const uint32_t count = std::max<uint32_t>(spec.count, 1);
        Track& track = tracks_[index];
        for (uint32_t i = 0; i < count; i++)
        {
            switch (types & 3)
            {
            case 0:
                args[i] = ReadByte(track);
                break;
            case 1:
                args[i] = Read16(track);
                break;
            case 2:
                args[i] = Read24(track);
                break;
            default:
                args[i] = ExchangeRegisterValue(index, ReadByte(track));
                break;
            }
            types = (types >> 2) & 0x3fff;
        }
    }
    if (((kNullCommands >> (command - 0xc0)) & 1) != 0)
    {
        return 0;
    }

    return Execute(index, command, args);
}

int32_t Tracks::Execute(uint32_t index, uint32_t command, const Args& a)
{
    Track& track = tracks_[index];
    switch (command)
    {
    case 0xc1: // openTrack
    case 0xc2: // openTrackBros
        {
            uint32_t parent = index;
            if (command == 0xc2)
            {
                parent = track.parent;
                if (parent == Track::kNone)
                {
                    return 0;
                }
            }
            const uint32_t child = OpenChild(parent, a[0] & 0xf, (a[0] >> 6) & 3);
            if (child == Track::kNone)
            {
                // The game starts the track that it didn't get: a write through a null pointer.
                unmodelled_++;
                return 0;
            }
            Start(child, tracks_[index].sequence, a[1]);
            return 0;
        }

    case 0xc4: // call
        CmdCall(index);
        return 0;

    case 0xc6: // ret
        if (Condition(track, a[0]))
        {
            if (track.depth == 0)
            {
                return 3;
            }

            track.cursor = track.stack[--track.depth];
        }
        return 0;

    case 0xc8: // jmp
        CmdJmp(index);
        return 0;

    case 0xc9: // loopS
        if (track.depth >= track.stack.size())
        {
            unmodelled_++;
            return 0;
        }
        track.stack[track.depth] = track.cursor;
        track.loop_counts[track.depth] = static_cast<uint16_t>(a[0]);
        track.depth++;
        return 0;

    case 0xca: // loopE (JASSeqCtrl::loopEnd)
        {
            if (track.depth == 0)
            {
                return 0;
            }

            const uint32_t at = track.depth - 1;
            const uint32_t count = track.loop_counts[at];
            if (count != 0 && ((count - 1) & 0xffff) != 0)
            {
                track.loop_counts[at] = static_cast<uint16_t>(count - 1);
                track.cursor = track.stack[at];
            }
            else
            {
                track.depth = at;
            }
            return 0;
        }

    case 0xcb: // readPort
        {
            const uint32_t port = a[0];
            if (port >= 16)
            {
                unmodelled_++;
                return 0;
            }
            track.import_flags[port] = 0;
            WriteRegDirect(track, a[1], track.ports[port]);
            return 0;
        }

    case 0xcc: // writePort
        if (a[0] >= 16)
        {
            unmodelled_++;
            return 0;
        }
        track.export_flags[a[0]] = 1;
        track.ports[a[0]] = static_cast<uint16_t>(a[1]);
        return 0;

    case 0xcd: // checkPortImport
    case 0xce: // checkPortExport
        if (a[0] >= 16)
        {
            unmodelled_++;
            return 0;
        }
        SetRegister(track, kRegResult, command == 0xcd ? track.import_flags[a[0]] : track.export_flags[a[0]]);
        return 0;

    case 0xcf: // wait
    case 0xea:
        track.wait = static_cast<int32_t>(a[0]);
        return a[0] != 0 ? 1 : 0;

    case 0xd1: // parentWritePort
        if (track.parent == Track::kNone)
        {
            unmodelled_++;
            return 0;
        }
        WritePortAppDirect(track.parent, a[0] & 0xf, static_cast<uint16_t>(a[1]));
        return 0;

    case 0xd2: // childWritePort
        {
            const uint32_t child = track.children[(a[0] >> 4) & 0xf];
            if (child == Track::kNone)
            {
                unmodelled_++;
                return 0;
            }
            WritePortAppDirect(child, a[0] & 0xf, static_cast<uint16_t>(a[1]));
            return 0;
        }

    case 0xd4: // setLastNote
        track.latest_key = static_cast<uint8_t>(a[0] + static_cast<uint32_t>(Transpose(track)));
        return 0;

    case 0xd5: // timeRelate
        track.time_relate = a[0] != 0;
        return 0;

    case 0xd6: // simpleOsc
        OscSetupSimple(track, a[0]);
        return 0;

    case 0xd7: // simpleEnv
        OscSetupSimpleEnv(track, a[0], a[1]);
        return 0;

    case 0xd8: // simpleADSR
        track.oscillators[0] = kAdsrDef;
        track.oscillators[0].attack = {track.adsr.data(), track.adsr.size()};
        track.adsr[0].time = static_cast<int16_t>(a[0]);
        track.adsr[1].time = static_cast<int16_t>(a[1]);
        track.adsr[2].time = static_cast<int16_t>(a[2]);
        track.adsr[2].value = static_cast<int16_t>(a[3]);
        track.direct_release = static_cast<uint16_t>(a[4]);
        return 0;

    case 0xd9: // transpose
        track.transpose = static_cast<int8_t>(a[0]);
        return 0;

    case 0xda: // closeTrack
        {
            const uint32_t slot = a[0] & 0xff;
            if (slot >= 16)
            {
                unmodelled_++;
                return 0;
            }
            const uint32_t child = track.children[slot];
            if (child != Track::kNone)
            {
                Close(child);
                tracks_[index].children[slot] = Track::kNone;
            }
            return 0;
        }

    case 0xdb: // outSwitch
        if (track.outer != nullptr)
        {
            track.outer->outer_switch = static_cast<uint16_t>(a[0]);
            track.outer->outer_update = 0xffff;
        }
        return 0;

    case 0xdc: // updateSync
        UpdateTrack(index, a[0]);
        return 0;

    case 0xdd: // busConnect
        if (a[0] >= track.updater.mix_configs.size())
        {
            unmodelled_++;
            return 0;
        }
        track.updater.mix_configs[a[0]] = static_cast<uint16_t>(a[1]);
        return 0;

    case 0xde: // pauseStatus
        track.pause_status = static_cast<uint8_t>(a[0]);
        return 0;

    case 0xdf: // setInterrupt
        track.enabled = static_cast<uint8_t>(track.enabled | (1u << (a[0] & 31)));
        if (a[0] < 8)
        {
            track.interrupt_targets[a[0]] = a[1];
            track.targets_set = static_cast<uint8_t>(track.targets_set | (1u << a[0]));
        }
        else
        {
            unmodelled_++;
        }
        return 0;

    case 0xe0: // disInterrupt
        track.enabled = static_cast<uint8_t>(track.enabled & ~(1u << (a[0] & 31)));
        return 0;

    case 0xe1: // clrI
        track.interrupted = false;
        track.return_cursor = 0;
        track.interrupts_on = true;
        return 0;

    case 0xe2: // setI
        track.interrupts_on = false;
        return 0;

    case 0xe3: // retI
        track.interrupts_on = true;
        if (track.interrupted)
        {
            track.wait = track.saved_wait;
            track.cursor = track.return_cursor;
            track.interrupted = false;
            track.return_cursor = 0;
        }
        TryInterrupt(track);
        return track.wait > 0 ? 1 : 0;

    case 0xe4: // intTimer
        track.timer_repeats = static_cast<uint8_t>(a[0]);
        track.timer = a[1];
        track.timer_reload = a[1];
        return 0;

    case 0xe5: // vibDepth
        {
            const uint32_t byte = a[0] & 0xff;
            track.vibrate.depth = static_cast<float>(static_cast<int32_t>(byte << 8 | byte << 1)) / 393204.0f;
            return 0;
        }

    case 0xe6: // vibDepthMidi
        track.vibrate.depth = static_cast<float>(a[0]) / 393204.0f;
        return 0;

    case 0xe7: // syncCPU
        SetRegister(track, kRegResult, sync_ ? sync_(index, a[0]) : 0xffff);
        return 0;

    case 0xe8: // flushAll
    case 0xe9: // flushRelease
        return 0;

    case 0xeb: // panPowSet
        track.registers[kRegPanPower] = static_cast<uint16_t>(a[0]);
        track.registers[kRegPanPower + 1] = static_cast<uint16_t>(a[1]);
        track.registers[kRegPanPower + 2] = static_cast<uint16_t>(a[2]);
        track.registers[kRegOuterWeight] = static_cast<uint16_t>(ToInt32(static_cast<float>(a[3]) * 327.67f));
        track.registers[kRegParentWeight] = static_cast<uint16_t>(ToInt32(static_cast<float>(a[4]) * 327.67f));
        return 0;

    case 0xec: // IIRSet
        for (std::size_t i = 0; i < 4; i++)
        {
            MoveParam& param = track.timed[kTimedIir + i];
            const float value = static_cast<float>(static_cast<int16_t>(a[i])) * kShortScale;
            param.value = value;
            param.target = value;
            param.time = 1.0f;
            param.amount = 0.0f;
        }
        return 0;

    case 0xed: // FIRSet: the outer parameters' FIR filter from the sequence, in the host's order
        if (track.outer == nullptr)
        {
            unmodelled_++;
            return 0;
        }
        track.outer->outer_update |= OuterParam::kFir;
        track.outer->outer_switch |= OuterParam::kFir;
        for (uint32_t i = 0; i < 8; i++)
        {
            track.outer->fir[i] = static_cast<int16_t>(track.sequence->Native16(a[0] + 2 * i));
        }
        return 0;

    case 0xee: // EXTSet
        {
            OuterParam* const outer = SequenceOuter(track.sequence, a[0]);
            outer->Init();
            track.outer = outer;
            return 0;
        }

    case 0xef: // panSwSet
        track.pan_calc = kCalcTypes[(a[0] >> 5) & 7];
        track.parent_pan_calc = kParentCalcTypes[(a[0] >> 5) & 7];
        track.updater.pan_calc = static_cast<uint8_t>(a[0] & 0x1f);
        track.fxmix_calc = kCalcTypes[(a[1] >> 5) & 7];
        track.parent_fxmix_calc = kParentCalcTypes[(a[1] >> 5) & 7];
        track.updater.fxmix_calc = static_cast<uint8_t>(a[1] & 0x1f);
        track.dolby_calc = kCalcTypes[(a[2] >> 5) & 7];
        track.parent_dolby_calc = kParentCalcTypes[(a[2] >> 5) & 7];
        track.updater.dolby_calc = static_cast<uint8_t>(a[2] & 0x1f);
        track.update_flags |= kUpdateFxmix | kUpdatePan | kUpdateDolby;
        return 0;

    case 0xf0: // oscRoute
        {
            const uint32_t slot = (a[0] >> 4) & 0xf;
            if (slot >= 2)
            {
                unmodelled_++;
                return 0;
            }
            track.osc_routes[slot] = a[0] & 0xf;
            return 0;
        }

    case 0xf1: // IIRCutOff
        {
            const uint32_t cutoff = a[0] & 0xff;
            if (cutoff >= 128)
            {
                unmodelled_++;
                return 0;
            }
            for (std::size_t i = 0; i < 4; i++)
            {
                MoveParam& param = track.timed[kTimedIir + i];
                const float value = static_cast<float>(kCutoffToIir[cutoff * 4 + i]) / kShortMax;
                param.value = value;
                param.target = value;
                param.time = 1.0f;
                param.amount = 0.0f;
            }
            return 0;
        }

    case 0xf2: // oscFull
        OscSetupFull(track, a[0], a[1], a[2]);
        return 0;

    case 0xf3: // volumeMode
        track.volume_mode = static_cast<uint8_t>(a[0]);
        return 0;

    case 0xf4: // vibPitch
        {
            const uint32_t byte = a[0] & 0xff;
            track.vibrate.pitch = static_cast<float>(byte << 8 | byte << 1) / 294903.0f;
            return 0;
        }

    case 0xfa: // checkWave
        SetRegister(track, kRegResult, 0);
        return 0;

    case 0xfb: // printf
        CmdPrintf(track);
        return 0;

    case 0xfc: // nop
        return 0;

    case 0xfd: // tempo
        SetTempo(index, a[0]);
        return 0;

    case 0xfe: // timeBase
        SetTimebase(index, a[0]);
        return 0;

    case 0xff: // finish
        return 3;

    default:
        return 0;
    }
}

int32_t Tracks::ParseSeq(uint32_t index)
{
    // JASSeqParser::parseSeq (0x3e1108): the commands up to a wait (0) or the sequence's end (-1).
    for (;;)
    {
        Track& track = tracks_[index];
        const uint32_t command = ReadByte(track);
        int32_t result = 0;
        if ((command & 0x80) == 0)
        {
            result = CmdNoteOn(index, command);
        }
        else if ((command & ~8u) == 0x80)
        {
            // cmdWait: a byte or 16 bits.
            uint32_t value = ReadByte(track);
            if (command != 0x80)
            {
                value = value << 8 | ReadByte(track);
            }
            track.wait = static_cast<int32_t>(value);
            result = value != 0 ? 1 : 0;
        }
        else if (command == 0xf9 || (command & 0xf0) == 0x80)
        {
            // cmdNoteOff: 0xf9 takes the note id from a register.
            uint32_t flags = command;
            if (command == 0xf9)
            {
                const uint32_t byte = ReadByte(track);
                flags = ExchangeRegisterValue(index, byte & 7) ^ 0xffffff80u;
                if ((byte & 0x80) != 0)
                {
                    flags |= 8;
                }
            }
            uint32_t release = 0;
            if ((flags & 8) != 0)
            {
                release = ReadByte(tracks_[index]);
                if (release > 100)
                {
                    release = (release - 98) * 20;
                }
            }
            NoteOff(index, flags & 7, static_cast<uint16_t>(release));
            continue;
        }
        else
        {
            switch (command & 0xf0)
            {
            case 0x90:
                CmdSetParam(index, command & 0xf);
                continue;

            case 0xa0:
                WriteRegParam(index, command & 0xf);
                continue;

            case 0xb0:
                {
                    // RegCmd_Process: the command from the next byte or a register, and a mask of the arguments that
                    // come from registers.
                    uint32_t inner = ReadByte(track);
                    const uint32_t mask_count = command & 7;
                    uint32_t mask = 0;
                    if ((command & 8) != 0)
                    {
                        inner = ExchangeRegisterValue(index, inner);
                    }
                    if ((command & 8) == 0 || mask_count != 0)
                    {
                        const uint32_t bits = ReadByte(tracks_[index]);
                        for (uint32_t i = 0; i <= mask_count; i++)
                        {
                            if (((bits << i) & 0x80) != 0)
                            {
                                mask |= 3u << (2 * i);
                            }
                        }
                    }
                    result = CmdProcess(index, inner, mask);
                    break;
                }

            default:
                result = CmdProcess(index, command, 0);
                break;
            }
        }

        if (result == 3)
        {
            return -1;
        }
        if (result == 1)
        {
            return 0;
        }
    }
}

void Tracks::OnChannel(uint32_t owner, Channel::Event event, uint32_t channel, DspBlock* block)
{
    // JASTrack::channelUpdateCallback (0x3dfb0c) and JASChannelUpdater's two updates (0x3e15c4 and 0x3e17b4).
    Track& track = tracks_[owner];
    Channel& ch = voices_.ChannelAt(channel);
    const ChannelUpdater& updater = track.updater;
    switch (event)
    {
    case Channel::kEventStart:
        ch.mix_configs = updater.mix_configs;
        if (ch.DolbyMode())
        {
            block->InitAutoMixer();
        }
        else
        {
            block->SetU16(0x0e, updater.delay_max);
            for (std::size_t i = 0; i < 6; i++)
            {
                block->SetU16(0x16 + 8 * i, static_cast<uint32_t>(updater.delays[i]) << 8 | updater.delays[i]);
            }
        }
        ch.pan_calc = updater.pan_calc;
        ch.fxmix_calc = updater.fxmix_calc;
        ch.dolby_calc = updater.dolby_calc;
        [[fallthrough]];

    case Channel::kEventPlay:
        if (event == Channel::kEventPlay && ch.status == Channel::kRelease)
        {
            return;
        }

        ch.volume_channel = updater.volume;
        ch.pitch_channel = updater.pitch;
        ch.pan_channel = updater.pan;
        ch.fxmix_channel = updater.fxmix;
        ch.dolby_channel = updater.dolby;
        for (std::size_t i = 0; i < 6; i++)
        {
            const std::size_t at = 0x16 + 8 * i;
            block->SetU16(at, (block->U16(at) & 0xff) | static_cast<uint32_t>(updater.delays[i]) << 8);
        }
        if ((updater.filter_mode & 0x20) != 0)
        {
            for (std::size_t i = 0; i < 4; i++)
            {
                block->SetU16(0x148 + 2 * i, static_cast<uint16_t>(updater.iir[i]));
            }
        }
        if ((updater.filter_mode & 0x1f) != 0)
        {
            std::array<uint16_t, 8> fir{};
            for (std::size_t i = 0; i < 8; i++)
            {
                fir[i] = static_cast<uint16_t>(updater.fir[i]);
            }
            block->SetFir8FilterParam(fir);
        }
        block->SetFilterMode(updater.filter_mode);
        block->SetDistFilter(updater.distortion);
        return;

    case Channel::kEventStop:
        {
            for (uint32_t& slot : track.channels)
            {
                if (slot == channel)
                {
                    slot = Track::kNone;
                    break;
                }
            }
            const auto at = std::find(track.channel_list.begin(), track.channel_list.end(), channel);
            if (at != track.channel_list.end())
            {
                track.channel_list.erase(at);
            }
            return;
        }

    case Channel::kEventTimer:
        voices_.Release(channel, 0);
        return;

    default:
        return;
    }
}

uint32_t Tracks::RouteTrack(uint32_t root, uint32_t route) const
{
    // JASTrack::routeTrack: the depth in the top 4 bits, then a slot of 4 bits for each level from the low bits.
    uint32_t at = root;
    const uint32_t depth = route >> 28;
    for (uint32_t i = 0; i < depth; i++)
    {
        at = tracks_[at].children[route & 0xf];
        if (at == Track::kNone)
        {
            return Track::kNone;
        }

        route >>= 4;
    }

    return at;
}

void Tracks::WritePortAppDirect(uint32_t index, uint32_t port, uint16_t value)
{
    // JASTrack::writePortAppDirect: a write to ports 0 and 1 requests interrupts 3 and 4.
    Track& track = tracks_[index];
    if (port >= 16)
    {
        unmodelled_++;
        return;
    }
    track.import_flags[port] = 1;
    track.ports[port] = value;
    if (port <= 1)
    {
        Request(track, port == 0 ? 3 : 4);
    }
}

bool Tracks::WritePortApp(uint32_t root, uint32_t route, uint16_t value)
{
    const uint32_t index = RouteTrack(root, route);
    if (index == Track::kNone)
    {
        return false;
    }

    WritePortAppDirect(index, (route >> 16) & 0xff, value);

    return true;
}

uint16_t Tracks::ReadPortAppDirect(uint32_t index, uint32_t port)
{
    // JASTrack::readPortAppDirect (0x3df4c8).
    if (port >= 16)
    {
        unmodelled_++;
        return 0;
    }
    tracks_[index].export_flags[port] = 0;
    return tracks_[index].ports[port];
}

bool Tracks::ReadPortApp(uint32_t root, uint32_t route, uint16_t& value)
{
    const uint32_t index = RouteTrack(root, route);
    if (index == Track::kNone)
    {
        return false;
    }

    const uint32_t port = (route >> 16) & 0xff;
    if (port >= 16)
    {
        unmodelled_++;
        return true;
    }
    tracks_[index].export_flags[port] = 0;
    value = tracks_[index].ports[port];

    return true;
}

void Tracks::MuteTrack(uint32_t index, bool mute)
{
    // JASTrack::muteTrack (0x3de684).
    Track& track = tracks_[index];
    track.muted = mute;
    track.update_flags |= kUpdateVolume;
    if (mute && (track.pause_status & 0x20) != 0)
    {
        for (uint32_t& channel : track.channels)
        {
            if (channel != Track::kNone)
            {
                voices_.Release(channel, 10);
                channel = Track::kNone;
            }
        }
    }
    for (const uint32_t child : tracks_[index].children)
    {
        if (child != Track::kNone)
        {
            MuteTrack(child, mute);
        }
    }
}

} // namespace hsf::jaudio1
