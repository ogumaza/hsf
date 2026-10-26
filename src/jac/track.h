// SPDX-License-Identifier: MIT

// The sequences of the C JAudio as the Switch release of Pikmin builds them for AArch64: the tracks (seqp_) with their
// ports, registers, interrupts and outer parameters, the parser of the commands (jammain_2 and its command table,
// CMDP_LIST), the start of a note (noteon), and the pool and the root tracks (seqsetup). The game's sound code starts a
// sequence on a root track: Jaq_SetSeqData takes a track from the pool and makes it a root, Jaq_SetBankNumber gives
// it its bank, and Jaq_StartSeq registers its callback in the player's list. Each sub-frame, PlayerCallback runs the
// list in the order of its slots. A root track's callback runs its ticks and its children's, and updates the levels
// that its channels take.
//
// The tracks refer to one another, to their channels and to their outer parameters by index. A channel's oscillator
// can point to a track's settings of an envelope. The game's channel does the same. The tracks therefore stay in place
// for the model's life.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <utility>
#include <vector>

#include "jac/bank.h"
#include "jac/calc.h"
#include "jac/channel.h"
#include "jac/oscillator.h"
#include "jaudio/track.h"

namespace hsf::jac
{

using jaudio::OscPoint;
using jaudio::Sequence;

// A track's port (TrackPort_): the flags that the game and the sequence set as each side writes it, and its value.
struct Port
{
    uint8_t import_flag = 0; // set as the code above the sequences writes the port
    uint8_t export_flag = 0; // set as the sequence writes it
    uint16_t value = 0;
};

// A level of a track that moves to a target over a number of ticks (MoveParam_).
struct MoveParam
{
    float value = 0.0f;
    float target = 0.0f;
    float time = 0.0f;
    float step = 0.0f;
};

// A track's outer parameters (OuterParam_): levels that the code above the sequences sets, each with a switch that
// turns it on. The update flags say which levels have changed since the track last read them. A buffer counts the
// tracks that it's assigned to (Jam_AssignExtBuffer).
struct OuterParam
{
    // The switches and the update flags.
    static constexpr uint16_t kVolume = 0x01;
    static constexpr uint16_t kPitch = 0x02;
    static constexpr uint16_t kFxmix = 0x04;
    static constexpr uint16_t kPan = 0x08;
    static constexpr uint16_t kDolby = 0x10;
    static constexpr uint16_t kDistortion = 0x20;
    static constexpr uint16_t kTempo = 0x40;
    static constexpr uint16_t kFir = 0x80;

    bool assigned = false;
    uint32_t references = 0;
    uint16_t switches = 0;
    uint16_t updates = 0;
    float volume = 0.0f;
    float fxmix = 0.0f;
    float dolby = 0.0f;
    float pitch = 0.0f;
    float pan = 0.0f;
    float tempo = 0.0f;
    std::array<int16_t, 8> fir{};
};

// A track (seqp_), by the game's fields.
struct Track
{
    static constexpr uint32_t kNone = 0xffffffff;

    // The states (trackState): closed, playing, set up but not started, or to stop at its next sub-frame.
    enum State : uint8_t
    {
        kClosed = 0,
        kPlaying = 1,
        kReady = 2,
        kStopping = 3,
    };

    // The timed levels (TimedParam_): volume, pitch, effect send, pan, surround, distortion, the two oscillators'
    // width, rate and vertex, the IIR filter's 4 coefficients, the level of a pause and the delay between the outputs.
    static constexpr std::size_t kTimedParams = 18;

    // The registers (URegisterParam_) by number: 0 to 5 general, 6 the bank and the program, 7 the bend's range, 8 to
    // 12 the pan's weights and the weights of the outer and the parent's levels, 13 the notes' priority, 16 to 23 the
    // four 32-bit registers, each as its high half then its low half.
    static constexpr std::size_t kRegisters = 32;

    // Where the track's data comes from (dataSourceMode): a pointer, or the FAT's memory. The model holds the data as
    // the sequence.
    const Sequence* sequence = nullptr;
    uint32_t cursor = 0; // programCounter
    uint32_t depth = 0;  // callStackDepth
    std::array<uint32_t, 8> stack{};
    std::array<uint16_t, 8> loop_counts{};
    uint8_t state = kClosed;
    uint8_t source = 0;      // dataSourceMode
    uint8_t file_handle = 0; // the FAT's handle in modes 1 and 2
    uint8_t flags = 0;
    uint32_t parent = kNone;
    std::array<uint32_t, 16> children{kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone,
                                      kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone};
    uint32_t connection_id = 0;     // the ID that the track registers by (Cmd_ConnectName), or 0
    uint32_t track_id = 0;          // a root's handle, or the track's route from its root
    int32_t wait = 0;               // waitTimer
    uint32_t note_played = 0;       // wasNotePlayed
    std::array<uint8_t, 8> notes{}; // activeNotes: 0xff for none
    std::array<uint32_t, 8> channels{kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone};
    std::array<uint16_t, 8> channel_ids{}; // the channels' ids as the notes took them (activeSoundIds)
    uint8_t gate_time = 0;
    uint8_t last_velocity = 0;
    uint32_t note_time = 0; // noteDuration
    uint8_t note_flags = 0;
    uint8_t last_note = 0;
    uint8_t gate = 0; // isGateMode

    // The channel manager (parentController) is the manager of the track's index in Channels.
    std::array<MoveParam, kTimedParams> timed{};
    std::array<uint16_t, kRegisters> registers{};
    uint32_t outer = kNone; // the outer parameters, by index in Tracks' buffers, or kNone
    std::array<uint32_t, 16> child_outer{kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone,
                                         kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone};
    std::array<Port, 16> ports{};
    float tempo_counter = 0.0f; // tempoAccumulator
    float tempo_rate = 1.0f;    // tempoFactor
    uint16_t timebase = 48;
    uint16_t tempo = 120;
    uint8_t time_relate = 0;

    // The game's tracks start with their oscillators' data zeroed. InitTrack sets up oscillator 0. Oscillator 1 stays
    // zeroed until a command sets it up.
    std::array<OscData, 2> oscillators{OscData{0, 0.0f, {}, {}, 0.0f, 0.0f}, OscData{0, 0.0f, {}, {}, 0.0f, 0.0f}};

    std::array<uint8_t, 2> osc_routes{};
    std::array<int16_t, 12> attack_table{}; // adsTable: a simple envelope's attack, decay and sustain, 4 points
    std::array<int16_t, 6> release_table{}; // relTable: its release, 2 points
    std::array<OscPoint, 4> adsr_attack{};  // the two tables as the oscillator reads them
    std::array<OscPoint, 2> adsr_release{};
    int8_t transpose = 0;
    uint8_t final_transpose = 0;
    int32_t ticks = 0; // tickCounter
    bool paused = false;
    uint8_t pause_status = 0;
    bool muted = false;
    uint16_t child_mutes = 0;
    uint8_t interrupted = 0; // interruptActive: the interrupt's bit while its handler runs, or 1 from Cmd_SetI
    uint8_t requests = 0;    // pendingInterrupts
    uint8_t enabled = 0;     // interruptEnable
    uint8_t timer_count = 0;
    std::array<uint32_t, 8> interrupt_targets{};
    uint32_t saved_cursor = 0;
    int32_t saved_wait = 0;
    uint32_t timer = 0;
    uint32_t timer_reload = 0; // maxTime
    uint32_t update_flags = 0;
    std::array<uint8_t, 3> pan_calcs{};
    std::array<uint8_t, 3> parent_pan_calcs{};
    bool registered = false;
    bool tempo_sync = false;
    bool allocated = false;               // from the pool: it goes back to the free queue as it closes
    std::array<OscState, 2> osc_states{}; // oscillatorParams
};

// For the ripper's length analysis: a track's jump back. The jump's target, and whether the track has started a note
// since its last jump back at the same depth, or since it opened. Then the levels of calls and counted loops below the
// jump, outermost first: each one's place in `stack` (a call's return, a loop's start) and its count in `counts`. A
// call's count is whatever the level's last loop left there.
struct LoopJump
{
    uint32_t target = 0;
    bool played = false;
    uint32_t depth = 0;
    std::array<uint32_t, 8> stack{};
    std::array<uint16_t, 8> counts{};

    bool operator==(const LoopJump&) const = default;
};

// The tracks: the pool of 256 with its free queue, the 16 roots of the game's sequence player, the 16 root handles,
// the tracks registered by their IDs, the root handles' outer parameters and the player's list of callbacks. The pool's
// tracks are 0 to 255, and the sequence player's roots (0x7e8240 in main, one for each of its handles) 256 to 271.
class Tracks
{
public:
    static constexpr std::size_t kTracks = 256;
    static constexpr std::size_t kPlayerRoots = 16;
    static constexpr std::size_t kAllTracks = kTracks + kPlayerRoots;
    static constexpr std::size_t kRoots = 16;
    static constexpr std::size_t kPlayerCalls = 16;
    static constexpr std::size_t kRegistered = 32;

    // A slot of the player's list that holds a callback of the game's code rather than a root track's.
    static constexpr uint32_t kReserved = 0xfffffffe;

    // A callback of the game's code in the player's list. It leaves the list when it returns -1.
    using PlayerCall = std::function<int32_t()>;

    // The game's answer to a sequence's request (Jam_RegisterTrackCallback's callback, from Cmd_SyncCPU). Without one,
    // a request gets 0xffff.
    using TrackCallback = std::function<uint16_t(uint32_t track, uint16_t value)>;

    // For the ripper's length analysis: a track's jump back (commands 0xc7 and 0xc8), at any depth.
    using LoopHook = std::function<void(uint32_t track, const LoopJump& jump)>;

    // The tracks start as Jaq_Reset leaves them: every track in the free queue, in its order.
    Tracks(Channels& channels, const Banks& banks, Random& random);

    Tracks(const Tracks&) = delete;
    Tracks& operator=(const Tracks&) = delete;

    void SetTrackCallback(TrackCallback callback)
    {
        callback_ = std::move(callback);
    }

    void SetLoopHook(LoopHook hook)
    {
        loop_hook_ = std::move(hook);
    }

    // Jaq_SetSeqData: `track`, or a track from the pool for Track::kNone, becomes a root for `sequence`, with the root
    // handle's outer parameters, and waits to start. `sequence` must stay in place while the track plays. Returns the
    // root's handle, or -1 without a free track or handle.
    int32_t SetSeqData(uint32_t track, const Sequence& sequence);

    int32_t SetSeqData(const Sequence& sequence)
    {
        return SetSeqData(Track::kNone, sequence);
    }

    // Jaq_SetBankNumber: the bank in the high byte of register 6.
    void SetBankNumber(uint32_t track, uint8_t bank);

    // Jaq_StartSeq: registers the root's callback in the player's list.
    bool StartSeq(int32_t handle);

    // Jac_RegisterPlayerCallback for a callback of the game's code: takes the list's first free slot. Returns false
    // when the list is full.
    bool RegisterPlayerCall(PlayerCall call);

    // Jaq_HandleToSeq: the root track of a handle, or Track::kNone.
    uint32_t HandleToSeq(uint32_t handle) const
    {
        return handle < kRoots ? roots_[handle] : Track::kNone;
    }

    // PlayerCallback: each registered callback, in the order of the list's slots. A root track's callback that ends
    // its sequence leaves the list.
    void RunPlayerCallbacks();

    // The code above the sequences: the track that registered `id` (Jam_GetTrackHandle), its ports
    // (Jam_WritePortAppDirect, Jam_ReadPortAppDirect and Jam_CheckPortAppDirect), its mute (Jam_MuteTrack), and its
    // pause and unpause with its children's (Jam_PauseTrack and Jam_UnPauseTrack).
    uint32_t TrackHandle(uint32_t id) const;
    bool WritePortAppDirect(uint32_t track, uint32_t port, uint16_t value);
    bool ReadPortAppDirect(uint32_t track, uint32_t port, uint16_t& value);
    bool CheckPortAppDirect(uint32_t track, uint32_t port, uint16_t value);
    void MuteTrack(uint32_t track, bool mute);
    void PauseTrack(uint32_t track);
    void UnpauseTrack(uint32_t track);

    // The outer parameters. AddOuter gives a new buffer for the code above the sequences, in the state that
    // Jam_InitExtBuffer leaves, and returns its index. InitExtBuffer sets a buffer to that state again.
    // AssignExtBuffer assigns a buffer to a track (Jam_AssignExtBuffer), and AssignExtBufferP to child `child` of a
    // track (Jam_AssignExtBufferP). SetExtParam, OnExtSwitch and OffExtSwitch act on a track's buffer
    // (Jam_SetExtParam, Jam_OnExtSwitch and Jam_OffExtSwitch), SetExtParamP and OnExtSwitchP on the buffer of a
    // track's child (Jam_SetExtParamP and Jam_OnExtSwitchP), and SetExtParamD, OnExtSwitchD and OffExtSwitchD on a
    // buffer by its index. Track::kNone stands for a null track or buffer.
    uint32_t AddOuter();
    void InitExtBuffer(uint32_t outer);
    bool AssignExtBuffer(uint32_t track, uint32_t outer);
    bool AssignExtBufferP(uint32_t track, uint8_t child, uint32_t outer);
    void SetExtParam(uint32_t track, uint8_t flag, float value);
    void OnExtSwitch(uint32_t track, uint16_t switches);
    void OffExtSwitch(uint32_t track, uint16_t switches);
    void SetExtParamP(uint32_t track, uint8_t child, uint8_t flag, float value);
    void OnExtSwitchP(uint32_t track, uint8_t child, uint16_t switches);
    void SetExtParamD(uint32_t outer, uint8_t flag, float value);
    void OnExtSwitchD(uint32_t outer, uint16_t switches);
    void OffExtSwitchD(uint32_t outer, uint16_t switches);

    const Track& At(uint32_t index) const
    {
        return tracks_[index];
    }

    Track& At(uint32_t index)
    {
        return tracks_[index];
    }

    const OuterParam& Outer(uint32_t index) const
    {
        return outer_[index];
    }

    // The free queue, from the next track that the pool gives (FREE_SEQP_QUEUE from GET_P).
    std::vector<uint32_t> FreeTracks() const;

    // The player's list: each slot's root track, kReserved, or Track::kNone.
    const std::array<uint32_t, kPlayerCalls>& PlayerCalls() const
    {
        return player_calls_;
    }

    // For the length analysis: whether a track has started a note since it opened, whether it has jumped back, and
    // whether its last pass, up to its last jump back outside a subroutine and a counted loop, started no note.
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

    // For the length analysis: the notes that a track has started since it opened, and the times that the track's
    // place has opened. A track that closes and opens again in the same place counts once more.
    uint32_t NoteCount(uint32_t index) const
    {
        return notes_[index];
    }

    uint32_t Opens(uint32_t index) const
    {
        return opens_[index];
    }

    // The notes that asked for a bank or a program that the bank file lacks, or a wave that no current scene holds.
    // The game's note then doesn't start.
    uint32_t MissingNotes() const
    {
        return missing_notes_;
    }

    // What the model leaves out because the game would read or write memory outside the tracks: a register or a port
    // past the end of its array, a command byte below 0xc0 from a register, a child that the pool can't give, or a
    // command on a missing parent or child. The game's result depends on whatever lies in that memory.
    uint32_t Unmodelled() const
    {
        return unmodelled_;
    }

private:
    // A track registered by its ID (TRACK_LIST).
    struct Registration
    {
        uint32_t id = 0;
        uint32_t track = Track::kNone;
    };

    // seqsetup: GetNewTrack, BackTrack, AllocNewRoot, DeAllocRoot, Init_Track, Jaq_OpenTrack, __AllNoteOff,
    // Jaq_CloseTrack, __StopSeq and Jaq_RootCallback.
    uint32_t GetNewTrack();
    bool BackTrack(uint32_t track);
    int32_t AllocNewRoot(uint32_t track);
    void DeAllocRoot(uint32_t track);
    void InitTrack(uint32_t track, uint32_t cursor, uint32_t parent);
    int32_t OpenTrack(uint32_t track, uint32_t flags, uint32_t cursor);
    void AllNoteOff(uint32_t track);
    void CloseTrack(uint32_t track);
    void StopSequence(uint32_t track);
    int32_t RootCallback(uint32_t track);

    // jammain_2: SeqUpdate, the parser (Jam_SeqmainNote) and its commands, the registers, the ports, the outer
    // parameters, the interrupts and the levels that a track's channels take.
    void SeqUpdate(uint32_t track, uint32_t flags);
    int32_t SeqMain(uint32_t track, bool muted);
    uint32_t StepTimed(uint32_t track);
    int32_t CommandProcess(uint32_t track, uint8_t command, uint16_t mask);
    int32_t RegCommandProcess(uint32_t track, bool from_register, uint32_t count);
    int32_t Command(uint32_t track, uint8_t command);
    int32_t CallF(uint32_t track);
    void Printf(uint32_t track);
    void JumpBack(uint32_t track, uint32_t target);
    void UpdateTrackAll(uint32_t track);
    void UpdateTrack(uint32_t track, uint32_t flags);
    void UpdateTempo(uint32_t track);
    void WriteTimeParam(uint32_t track, uint8_t command);
    void WriteRegDirect(uint32_t track, uint8_t reg, uint16_t value);
    void WriteRegParam(uint32_t track, uint8_t command);
    uint32_t LoadTable(uint32_t track, uint32_t offset, uint32_t index, uint32_t type);
    uint16_t ReadRegDirect(uint32_t track, uint8_t reg);
    uint32_t ReadReg32(uint32_t track, uint8_t reg);
    uint32_t ExchangeRegisterValue(uint32_t track, uint8_t reg);
    bool Condition(uint32_t track, uint8_t condition);
    int32_t SeqTimeToDspTime(uint32_t track, int32_t time, uint8_t percent) const;
    void SetInterrupt(uint32_t track, uint16_t interrupt);
    bool TryInterrupt(uint32_t track);
    void InitRegistTrack();
    void RegistTrack(uint32_t track, uint32_t id);
    void UnRegistTrack(uint32_t track);
    uint32_t SequenceOuter(const Sequence* sequence, uint32_t offset);

    // A register by number, or 0 for one past the array.
    uint16_t Register(const Track& track, uint32_t reg);
    void SetRegister(Track& track, uint32_t reg, uint16_t value);

    // noteon: NoteON, NoteOFF_R, GateON and CheckNoteStop, by the note's slot (0 to 7).
    int32_t NoteOn(uint32_t track, uint8_t slot, uint8_t key, uint8_t velocity, int32_t time);
    bool NoteOff(uint32_t track, uint8_t slot, uint16_t release);
    int32_t GateOn(uint32_t track, uint8_t slot, uint8_t key, uint8_t velocity, int32_t time);
    bool CheckNoteStop(uint32_t track, uint8_t slot);

    // jamosc: the track's oscillators.
    void UpdateOscParam(Track& track, uint32_t param, float value);
    void SetupSimple(Track& track, uint8_t kind);
    void SetupSimpleEnv(Track& track, uint8_t kind, uint32_t offset);
    void SetupAdsr(Track& track, const std::array<int16_t, 5>& times);
    void SetupFull(Track& track, uint8_t flags, uint32_t attack, uint32_t release);
    static void ClearOverwrite(Track& track);
    static void InitEnv(Track& track);
    static OscTable SequenceTable(const Track& track, uint32_t offset);

    // The reads of the sequence at the cursor and at an offset, big-endian.
    static uint8_t ReadByte(Track& track);
    static uint16_t Read16(Track& track);
    static uint32_t Read24(Track& track);
    static uint8_t ByteAt(const Track& track, uint32_t offset);
    static uint16_t WordAt(const Track& track, uint32_t offset);
    static uint32_t Word24At(const Track& track, uint32_t offset);
    static uint32_t LongAt(const Track& track, uint32_t offset);

    Channels& channels_;
    const Banks& banks_;
    Random& random_;
    std::array<Track, kAllTracks> tracks_{};
    std::array<uint32_t, kTracks> free_queue_{};
    uint32_t get_ = 0;
    uint32_t back_ = 0;
    uint32_t remain_ = kTracks;
    std::array<uint32_t, kRoots> roots_{};
    std::deque<OuterParam> outer_; // the roots' (ROOT_OUTER), then those of the code above the sequences
    std::map<std::pair<const Sequence*, uint32_t>, uint32_t> sequence_outer_; // Cmd_EXTSet's buffers in the data
    std::array<uint32_t, kPlayerCalls> player_calls_{};
    std::array<PlayerCall, kPlayerCalls> game_calls_{};
    std::array<Registration, kRegistered> registered_{};
    uint32_t registrations_ = 0;     // T_LISTS
    std::array<uint32_t, 8> args_{}; // SEQ_ARG: a command's arguments, which stay until the next command sets them
    uint8_t command_ = 0;            // SEQ_CMD
    TrackCallback callback_;
    LoopHook loop_hook_;
    std::array<uint32_t, kAllTracks> notes_{};

    // Each track's count of notes at its last jump back, by the jump's depth.
    std::array<std::array<uint32_t, std::tuple_size_v<decltype(Track::stack)> + 1>, kAllTracks> jump_notes_{};

    std::array<bool, kAllTracks> looped_{};
    std::array<bool, kAllTracks> quiet_{};
    std::array<uint32_t, kAllTracks> opens_{};
    uint32_t missing_notes_ = 0;
    uint32_t unmodelled_ = 0;
};

} // namespace hsf::jac
