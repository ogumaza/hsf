// SPDX-License-Identifier: MIT

// JAudio1's sequences as the Switch release of Pikmin 2 builds them for AArch64: a sequence's tracks (JASTrack) with
// their control of the commands (JASSeqCtrl), ports (JASTrackPort), interrupts (JASIntrMgr), registers
// (JASRegisterParam), outer parameters (JASOuterParam) and the levels that they give their channels
// (JASChannelUpdater), the parser of the commands (JASSeqParser), and the start of a note on a bank
// (JASBankMgr::noteOn). Each sub-frame, the driver calls each playing sequence's root track (JASTrack::rootCallback).
// The root track then runs its ticks and its children's, and updates the levels. A track's notes play on channels
// (channel.h).
//
// The tracks refer to one another and to their channels by index. A channel's oscillator can point to a track's
// settings of an envelope, as in the game. The tracks therefore stay in place for the engine's life.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include "jaudio/track.h"
#include "jaudio1/channel.h"
#include "jaudio1/instruments.h"
#include "jaudio1/oscillator.h"

namespace hsf::jaudio1
{

using jaudio::Sequence;

// A track's outer parameters (JASOuterParam): levels that the code above the sequences sets, each with a switch that
// turns it on. The update flags say which levels have changed since the track last read them.
struct OuterParam
{
    // The switches and the update flags.
    static constexpr uint16_t kVolume = 0x01;
    static constexpr uint16_t kPitch = 0x02;
    static constexpr uint16_t kFxmix = 0x04;
    static constexpr uint16_t kPan = 0x08;
    static constexpr uint16_t kDolby = 0x10;
    static constexpr uint16_t kTempo = 0x40;
    static constexpr uint16_t kFir = 0x80;

    uint16_t outer_switch = 0;
    uint16_t outer_update = 0;
    float volume = 0.0f;
    float pitch = 0.0f;
    float fxmix = 0.0f;
    float dolby = 0.0f;
    float pan = 0.0f;
    float tempo = 0.0f;
    std::array<int16_t, 8> fir{};

    // initExtBuffer.
    void Init()
    {
        outer_switch = 0;
        outer_update = 0;
    }

    bool Check(uint16_t flag) const
    {
        return (outer_switch & flag) != 0;
    }

    // setParam: one level by its flag, which then counts as updated. Other flags change nothing.
    void SetParam(uint8_t flag, float value);

    // onSwitch.
    void OnSwitch(uint16_t flag)
    {
        outer_switch |= flag;
        outer_update |= flag;
    }
};

// A level of a track that moves to a target over a number of ticks (JASTrack::MoveParam_).
struct MoveParam
{
    float value = 0.0f;
    float target = 0.0f;
    float time = 0.0f;
    float amount = 0.0f;
};

// The levels that a track gives its channels, and the settings of their DSP channels' mixers and filters
// (JASChannelUpdater).
struct ChannelUpdater
{
    float volume = 1.0f;
    float pitch = 1.0f;
    float pan = 0.5f;
    float fxmix = 0.0f;
    float dolby = 0.0f;
    std::array<int16_t, 8> fir{0x7fff, 0, 0, 0, 0, 0, 0, 0};
    std::array<int16_t, 4> iir{0x7fff, 0, 0, 0};
    int16_t distortion = 0;
    std::array<uint16_t, 6> mix_configs{0x150, 0x210, 0x352, 0x412, 0, 0};
    std::array<uint8_t, 6> delays{};
    uint8_t delay_max = 0;
    uint8_t filter_mode = 0;
    uint8_t pan_calc = 13;
    uint8_t fxmix_calc = 13;
    uint8_t dolby_calc = 13;
};

// A track (JASTrack), by the game's fields.
struct Track
{
    static constexpr uint32_t kNone = 0xffffffff;

    // The status (the field at +0x43b): closed, playing, set up but not started, or to stop at its next sub-frame.
    enum Status : uint8_t
    {
        kClosed = 0,
        kPlaying = 1,
        kReady = 2,
        kStopping = 3,
    };

    // The timed levels (TimedParam_): volume, pitch, effect send, pan, surround, distortion, the oscillators' width,
    // rate and vertex, the IIR filter's 4 coefficients, the level of a pause and the delay between the outputs.
    static constexpr std::size_t kTimedParams = 18;

    // The registers (JASRegisterParam) by number: 0 to 5 general, 6 the bank and the program, 7 the bend's range, 8
    // to 10 the pan's weights, 11 and 12 the weights of the outer and the parent's levels, 13 the notes' priority, 14
    // and 15 unused, 16 to 23 the four 32-bit registers, each high half first. 24 to 31 are the 16 bytes after
    // JASRegisterParam in JASTrack. The commands read and write them as registers. Registers 14, 15 and 24 to 31 keep
    // their values through JASTrack's constructor and init: a track from the pool has the values of its last use.
    static constexpr std::size_t kRegisters = 32;

    bool allocated = false; // in use: a root track, or a child from the pool
    bool from_pool = false; // a child that goes back to the pool as it closes (the field at +0x446)

    // JASSeqCtrl: the sequence, the cursor, the wait, the stack of calls and loops, the wait that an interrupt
    // saved, and the cursor to return to from the interrupt.
    const Sequence* sequence = nullptr;
    uint32_t cursor = 0;
    int32_t wait = 0;
    uint32_t depth = 0;
    std::array<uint32_t, 8> stack{};
    std::array<uint16_t, 8> loop_counts{};
    int32_t saved_wait = 0;
    bool interrupted = false;
    uint32_t return_cursor = 0;

    // JASTrackPort. The two directions share the values.
    std::array<uint8_t, 16> import_flags{};
    std::array<uint8_t, 16> export_flags{};
    std::array<uint16_t, 16> ports{};

    // JASIntrMgr: whether interrupts are on, the pending requests, the enabled interrupts with their targets, and the
    // timer's repeats, count and reload.
    bool interrupts_on = true;
    uint8_t requests = 0;
    uint8_t enabled = 0;
    uint8_t timer_repeats = 0;
    uint32_t timer = 0;
    uint32_t timer_reload = 0;
    std::array<uint32_t, 8> interrupt_targets{};
    uint8_t targets_set = 0; // the interrupts whose targets the track has set since its init. The others' are null.

    std::array<uint32_t, 8> channels{kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone};
    int32_t note_time = 0;  // the last note's time in ticks, or -1 (the field at +0x160)
    uint8_t note_flags = 0; // the last note's gate flags (+0x164)
    uint8_t latest_key = 0; // (+0x165)
    uint8_t gate = 0;       // the next note goes on with the playing channel (+0x166)

    Vibrate vibrate;
    ChannelUpdater updater;
    std::array<MoveParam, kTimedParams> timed{};
    std::array<uint16_t, kRegisters> registers{};
    std::array<OscData, 2> oscillators{};
    std::array<uint32_t, 2> osc_routes{0xf, 0xf};
    std::array<OscPoint, 4> adsr{}; // the points of a simple ADSR (the field at +0x378)

    uint32_t parent = kNone;
    std::array<uint32_t, 16> children{kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone,
                                      kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone};
    OuterParam* outer = nullptr;
    float counter = 0.0f;    // the ticks owed (the field at +0x420)
    float tempo_rate = 1.0f; // ticks per sub-frame
    uint32_t route = 0;      // the track's place in the tree: the depth in the top 4 bits, then the slots
    uint32_t update_flags = 0;
    uint16_t direct_release = 0;
    uint16_t tempo = 120;
    uint16_t timebase = 48;
    int8_t transpose = 0;

    // Bit 0: the levels ignore the parent's levels. Bit 1: the child takes no settings from its parent.
    uint8_t inherit = 0;

    uint8_t pause_status = 0x0a;
    uint8_t volume_mode = 0;
    uint8_t note_mask = 0;
    uint8_t status = kClosed;
    uint8_t pan_calc = 0;
    uint8_t fxmix_calc = 0;
    uint8_t dolby_calc = 0;
    uint8_t parent_pan_calc = 0;
    uint8_t parent_fxmix_calc = 0;
    uint8_t parent_dolby_calc = 0;
    bool paused = false;
    bool muted = false;
    bool time_relate = true;  // a note's time counts in ticks at the tempo, not in sub-frames
    bool tempo_child = false; // a child with a separate tempo (the field at +0x445)

    // The track's list of channels (JSUList<JASChannel>): every channel that a note started and hasn't stopped, in
    // the order of the notes.
    std::vector<uint32_t> channel_list;
};

// The sequences' tracks and the start of their notes. A callback of a track's channel reaches the track through
// `OnChannel`.
class Tracks : public ChannelOwner
{
public:
    // JASTrack's pool, from JAIGlobalParameter::systemTrackMax.
    static constexpr std::size_t kPoolTracks = 256;

    // The root tracks of the sounds that play sequences, at most this many at once.
    static constexpr std::size_t kRootTracks = 8;

    // The answer of the game's code to a sequence's syncCPU command (JASTrack's sCallBackFunc). Without one, the
    // command gets 0xffff.
    using SyncCallback = std::function<uint16_t(uint32_t track, uint32_t value)>;

    explicit Tracks(Voices& voices);

    Tracks(const Tracks&) = delete;
    Tracks& operator=(const Tracks&) = delete;

    // The banks that the sequences' bank numbers name: JASBankMgr's table from a virtual number to a physical one,
    // then the physical bank. Numbers 0 to 255.
    void SetBank(uint32_t physical, const InstrumentBank* bank);
    void SetVirtualBank(uint32_t number, uint32_t physical);

    void SetSyncCallback(SyncCallback callback)
    {
        sync_ = std::move(callback);
    }

    // A root track (one of JAInter::SeqParameter's) that plays `sequence` from offset 0, with `outer` as its outer
    // parameters: JASTrack::setSeqData, then startSeq. `outer` must stay in place while the track plays. Returns the
    // root track's index, or Track::kNone without a free root track.
    uint32_t StartSequence(const Sequence& sequence, OuterParam* outer);

    // The driver's sub-frame callback of every playing root track (JASTrack::rootCallback), in the order that they
    // started. A sequence ends when its root track's commands end.
    void RunSubframe();

    // Whether root track `root` still plays.
    bool Playing(uint32_t root) const
    {
        return tracks_[root].status != Track::kClosed;
    }

    const Track& TrackAt(uint32_t index) const
    {
        return tracks_[index];
    }

    Track& TrackAt(uint32_t index)
    {
        return tracks_[index];
    }

    // The pool's free tracks, the next one first.
    const std::deque<uint32_t>& FreeTracks() const
    {
        return free_tracks_;
    }

    // JASInstRand's generator and JASPlayer::getRandomS32's two words.
    RandomFast& InstGenerator()
    {
        return inst_random_;
    }

    std::array<int32_t, 2>& SeqRandom()
    {
        return seq_random_;
    }

    // Draws `count` numbers from JASPlayer::getRandomS32's generator. They don't count in RandomDraws.
    void DrawRandom(uint32_t count)
    {
        for (uint32_t i = 0; i < count; i++)
        {
            GetRandomS32();
        }
    }

    // The numbers that the sequences' commands have drawn from the generator.
    uint32_t RandomDraws() const
    {
        return random_draws_;
    }

    // JASTrack::channelUpdateCallback.
    void OnChannel(uint32_t owner, Channel::Event event, uint32_t channel, DspBlock* block) override;

    // The code above the sequences: a track by its route (JASTrack::routeTrack), the ports (writePortApp,
    // readPortApp, readPortAppDirect and writePortAppDirect), a mute (muteTrack), a register (writeRegDirect) and the
    // tempo (setTempo).
    uint32_t RouteTrack(uint32_t root, uint32_t route) const;
    bool WritePortApp(uint32_t root, uint32_t route, uint16_t value);
    bool ReadPortApp(uint32_t root, uint32_t route, uint16_t& value);
    uint16_t ReadPortAppDirect(uint32_t index, uint32_t port);
    void WritePortAppDirect(uint32_t index, uint32_t port, uint16_t value);
    void MuteTrack(uint32_t index, bool mute);

    void WriteRegDirect(uint32_t index, uint32_t reg, uint32_t value)
    {
        WriteRegDirect(tracks_[index], reg, value);
    }

    void SetTempo(uint32_t index, uint32_t tempo);

    // A timed level of a track (JASTrack::setParam): the level moves to `value` over `time` ticks, or takes it at once
    // for a time of 0 or less.
    void SetParam(uint32_t index, uint32_t param, float value, int32_t time)
    {
        SetParam(tracks_[index], param, value, time);
    }

    // A request for interrupt `interrupt` of a track (JASTrack::setInterrupt). It counts only while the track has the
    // interrupt enabled.
    void SetInterrupt(uint32_t index, uint32_t interrupt)
    {
        Request(tracks_[index], interrupt);
    }

    // For the ripper's length analysis: a track's jump back outside a subroutine and a counted loop (command 0xc8),
    // with the jump's target and whether the track started a note since its last such jump or since it opened. And
    // whether a track has started a note since it opened, and whether its last pass, up to its last such jump,
    // started no note.
    using LoopHook = std::function<void(uint32_t track, uint32_t target, bool played)>;

    void SetLoopHook(LoopHook hook)
    {
        loop_hook_ = std::move(hook);
    }

    bool PlayedNotes(uint32_t index) const
    {
        return notes_[index] != 0;
    }

    bool Looped(uint32_t index) const
    {
        return looped_[index];
    }

    bool QuietPass(uint32_t index) const
    {
        return quiet_[index];
    }

    // The notes that asked for a bank that isn't loaded. The game's note then doesn't start.
    uint32_t MissingBankNotes() const
    {
        return missing_bank_notes_;
    }

    // What the model leaves out because the game would read or write memory outside the tracks: a register or a
    // port past the end of its array, a command byte below 0xc0 from a register, a jump to a 32-bit register's
    // address, a child that the pool can't give, or a command on a missing parent or child. The game's result
    // depends on whatever lies in that memory, or the game crashes.
    uint32_t Unmodelled() const
    {
        return unmodelled_;
    }

private:
    using Args = std::array<uint32_t, 8>;

    // JASTrack's constructor, init and inherit, and the pool's allocation and return of a child.
    void Construct(Track& track);
    void Init(uint32_t index);
    void Inherit(uint32_t index);
    uint32_t AllocTrack();
    void FreeTrack(uint32_t index);

    // rootCallback's steps: mainProc, updateSeq, updateTrack, updateTrackAll, updateTempo, updateTimedParam and close.
    int32_t RootCallback(uint32_t index);
    int32_t MainProc(uint32_t index);
    void UpdateSeq(uint32_t index, uint32_t flags, bool recursive);
    void UpdateTrack(uint32_t index, uint32_t flags);
    void UpdateTrackAll(uint32_t index);
    void UpdateTempo(uint32_t index);
    void UpdateTimedParam(Track& track);
    void Close(uint32_t index);
    void ReleaseChannels(uint32_t index);
    bool TryInterrupt(Track& track);
    void Request(Track& track, uint32_t interrupt);
    void TimerProcess(Track& track);

    // The track's start at `offset` (JASTrack::start) and openChild.
    void Start(uint32_t index, const Sequence* sequence, uint32_t offset);
    uint32_t OpenChild(uint32_t index, uint32_t slot, uint32_t inherit);

    // The parser: the commands until a wait, and the commands by their kinds.
    int32_t ParseSeq(uint32_t index);
    int32_t CmdNoteOn(uint32_t index, uint32_t key);
    int32_t CmdProcess(uint32_t index, uint32_t command, uint32_t register_mask);
    int32_t Execute(uint32_t index, uint32_t command, const Args& args);
    void CmdSetParam(uint32_t index, uint32_t flags);
    void CmdCall(uint32_t index);
    void CmdJmp(uint32_t index);
    void CmdPrintf(Track& track);
    bool Condition(const Track& track, uint32_t flag) const;

    // The reads of the sequence at the cursor and at an offset, big-endian.
    uint8_t ReadByte(Track& track);
    uint16_t Read16(Track& track);
    uint32_t Read24(Track& track);
    uint16_t Get16(const Track& track, uint32_t offset) const;
    uint32_t Get24(const Track& track, uint32_t offset) const;
    uint32_t Get32(const Track& track, uint32_t offset) const;

    // The registers: exchangeRegisterValue, readReg32, readReg16, writeRegDirect and writeRegParam.
    uint32_t ExchangeRegisterValue(uint32_t index, uint32_t reg);
    uint32_t ReadReg32(uint32_t index, uint32_t reg);
    uint32_t ReadReg16(uint32_t index, uint32_t reg);
    void WriteRegDirect(Track& track, uint32_t reg, uint32_t value);
    void WriteRegParam(uint32_t index, uint32_t command);
    uint16_t Register(const Track& track, uint32_t reg);
    void SetRegister(Track& track, uint32_t reg, uint32_t value);

    // The notes: noteOn, noteOff, gateOn and overwriteOsc, the oscillators' set-ups, and seqTimeToDspTime.
    int32_t NoteOn(uint32_t index, uint32_t note, uint32_t key, uint32_t velocity, int32_t time, uint32_t skip);
    bool NoteOff(uint32_t index, uint32_t note, uint16_t release);
    void GateOn(uint32_t index, uint32_t note, uint32_t key, uint32_t velocity, int32_t time);
    void OverwriteOsc(Track& track, uint32_t channel);
    void OscSetupFull(Track& track, uint32_t route, uint32_t attack, uint32_t release);
    void OscSetupSimpleEnv(Track& track, uint32_t kind, uint32_t offset);
    void OscSetupSimple(Track& track, uint32_t kind);
    int32_t SeqTimeToDspTime(const Track& track, int32_t time, uint32_t percent) const;
    void SetParam(Track& track, uint32_t param, float value, int32_t time);
    void SetTimebase(uint32_t index, uint32_t timebase);
    int32_t Transpose(const Track& track) const;

    // JASBankMgr::noteOn and the channel's set-up. Returns the channel, or Channel::kNone.
    uint32_t BankNoteOn(uint32_t bank, uint32_t program, uint32_t key, uint32_t velocity, uint16_t priority,
                        uint32_t owner);

    // The sequences' tables of points (an envelope's or an ADSR's) as the oscillators read them.
    OscTable SequenceTable(const Track& track, uint32_t offset) const;

    // The outer parameters that a sequence's EXTSet command places in the sequence's data, by the sequence and the
    // offset.
    OuterParam* SequenceOuter(const Sequence* sequence, uint32_t offset);

    void JumpBack(uint32_t index, uint32_t target);
    int32_t GetRandomS32();

    Voices& voices_;
    std::vector<Track> tracks_; // the pool's tracks, then the root tracks
    std::deque<uint32_t> free_tracks_;
    std::vector<uint32_t> list_;        // the playing root tracks, in the order that they started
    std::deque<OuterParam> pool_outer_; // the pool's tracks' outer parameters, allocated with the pool
    std::map<std::pair<const Sequence*, uint32_t>, OuterParam> sequence_outer_;
    std::array<uint16_t, 256> virtual_banks_{};
    std::array<const InstrumentBank*, 256> banks_{};
    RandomFast inst_random_;
    std::array<int32_t, 2> seq_random_{0x1000, 0x5555};
    uint32_t random_draws_ = 0;
    SyncCallback sync_;

    // The length analysis's count of each track's notes since it opened, the count at its last jump back, whether it
    // has jumped back, and whether the pass up to its last jump back started no note.
    LoopHook loop_hook_;
    std::vector<uint32_t> notes_;
    std::vector<uint32_t> loop_notes_;
    std::vector<bool> looped_;
    std::vector<bool> quiet_;
    uint32_t missing_bank_notes_ = 0;
    uint32_t unmodelled_ = 0;
};

} // namespace hsf::jaudio1
