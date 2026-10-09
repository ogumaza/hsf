// SPDX-License-Identifier: MIT

// nn::atk's sequence playback (driver::SequenceTrack with its MmlParser, and driver::SequenceSoundPlayer). The playback
// is nw::snd's sequence playback with little-endian arguments, 5 ms frames and one output. The comment on a method that
// follows one of the game's routines gives the routine's address in WarioWare: Get It Together!'s main. A field comment
// gives the field's offset in the game's object.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "atk/channel.h"
#include "atk/envelope.h"
#include "atk/hardware.h"
#include "atk/voice.h"
#include "fsar/formats.h"

namespace hsf::atk
{

class Engine;
class SequenceSoundPlayer;

// MoveValue<T, s16>: a linear ramp to a target over a number of sequence ticks.
template <typename T>
struct MoveValue
{
    void Init(T value)
    {
        start = value;
        target = value;
        duration = 0;
        counter = 0;
    }

    void Update()
    {
        if (counter < duration)
        {
            ++counter;
        }
    }

    T Get() const
    {
        if (counter >= duration)
        {
            return target;
        }

        return static_cast<T>(static_cast<int>(start) +
                              (static_cast<int>(target) - static_cast<int>(start)) * counter / duration);
    }

    void SetTarget(T value, int16_t ticks)
    {
        start = Get();
        duration = ticks;
        counter = 0;
        target = value;
    }

    bool Moving() const
    {
        return counter < duration;
    }

    T start{};
    T target{};
    int16_t duration = 0;
    int16_t counter = 0;
};

class SequenceTrack
{
public:
    // Track `index`, 0 to 15, of `player`.
    SequenceTrack(SequenceSoundPlayer& player, int index);

    SequenceTrack(const SequenceTrack&) = delete;
    SequenceTrack& operator=(const SequenceTrack&) = delete;

    // Clears the callbacks of the channels that still point at the track.
    ~SequenceTrack();

    void SetSeqData(std::span<const uint8_t> data, uint32_t offset); // 0xc0a2b0
    void Open();                                                     // 0xc0a2c0
    void Close();                                                    // 0xc0a240

    bool IsOpen() const
    {
        return open_;
    }

    // The channels the track plays, the newest first. Each channel points at the next with next_in_track.
    const Channel* ChannelList() const
    {
        return channel_list_;
    }

    // Whether the track won't read on while its notes sound. Either the track waits for the notes to end (after a note
    // of length 0 with note wait on) and some still sound, or the track waits for ever. A track whose notes have all
    // ended reads on at its next tick.
    bool WaitsForNotes() const
    {
        return (note_finish_wait_ && channel_list_ != nullptr) || WaitsForEver();
    }

    // Whether the track will never read on, or not before the game stops the sound. The track waits for a negative
    // number of ticks, waits out a held note (see kHeldTicks), or waits for its notes to end while one never will. A
    // note or wait whose length comes from a variable left at its default of -1 makes a negative wait.
    bool WaitsForEver() const
    {
        return wait_ < 0 || held_wait_ || WaitsOnEndlessNote();
    }

    // Whether the track waits for its notes to end while one of them has no length and plays a looping wave. That note
    // ends only when the game stops it.
    bool WaitsOnEndlessNote() const;

    // Whether the track's last pass of a loop changed nothing in the sound, and nothing has changed since (for the
    // ripper's length analysis, while the player has on_loop set). A track that polls or counts a variable once a tick
    // makes such idle passes. See CountLoop.
    bool IsIdle() const
    {
        return idle_ && changed_at_ < looped_at_;
    }

    // Whether the track is idle, its last pass left the variables that tracks read as the pass before did, and the
    // track has drawn no random number since that pass began. Such a track does the same on every pass until the game
    // changes a variable.
    bool IsStill() const
    {
        return IsIdle() && still_ && drew_at_ < looped_at_;
    }

    // Whether the track's last pass of a loop played no note. A track after track 0 whose passes play no notes doesn't
    // keep a later track from being the main track (see SequenceSoundPlayer::IsMainTrack). A still track whose passes
    // play notes can change the sound again: once its notes end, its next pass starts a new one.
    bool IsQuiet() const
    {
        return quiet_;
    }

    // Whether a note of the track will end when its length runs out. A held note (see kHeldTicks) doesn't count. The
    // game stops it first. Nor does a note while the damper is on.
    bool HasEndingNote() const;

    // Whether the track's volume, pan or pitch bend is moving to a new value.
    bool IsMoving() const
    {
        return vol_.Moving() || volume2_.Moving() || pan_.Moving() || span_.Moving() || bend_.Moving();
    }

    // A tick: the ramps advance and the commands up to the next wait run. Returns -1 when the track reaches its end, 1
    // otherwise, or 0 if it's closed (ParseNextTick, 0xc0a430).
    int ParseNextTick(bool do_note_on);

    void UpdateChannelLength(); // 0xc0a3a0
    void UpdateChannelParam();  // 0xc0a630

private:
    enum ArgType
    {
        kArgNone = 0,
        kArgU8 = 1,
        kArgS16 = 2,
        kArgVmidi = 3,
        kArgRandom = 4,
        kArgVariable = 5,
    };

    // A call or a loop: where it returns to, and the loop's count.
    struct CallStackEntry
    {
        bool is_loop = false;
        uint8_t loop_count = 0;
        uint32_t address = 0;
    };

    static constexpr int kCallStackDepth = 10;

    // What a command can change in the track's sound, for the ripper's length analysis: the track's settings, its
    // ramps' places and targets, its newest channel, and the player's tempo and main volume.
    struct SoundState
    {
        uint32_t prg_no;
        uint8_t bank_index;
        int8_t transpose;
        uint8_t velocity_range, bend_range, priority, porta_key, porta_time, main_send;
        int8_t init_pan, biquad_type;
        std::array<uint8_t, 4> envelope;
        int16_t hold;
        std::array<uint8_t, 3> fx_send;
        float lpf, biquad_value, sweep_pitch;
        int32_t output_line;
        std::array<CurveLfoParam, 4> lfo_params;
        std::array<uint8_t, 4> lfo_types;
        bool mute, silence, tie, mono, porta, damper, front_bypass, note_wait;
        std::array<int32_t, 18> ramps; // the value, target and ticks to go of the track's five ramps and main volume
        const Channel* newest;
        uint8_t timebase;
        uint16_t tempo;

        bool operator==(const SoundState&) const = default;
    };

    SoundState State() const;

    // Parse, with what the ripper's length analysis follows while the player has on_loop set: each command's place in
    // the order the player's tracks run them, and whether the command changes the track's sound.
    int RunCommand(bool do_note_on);

    void InitParam();                                               // 0xc0a0a0
    int Parse(bool do_note_on);                                     // MmlParser::Parse, 0xc20cf0
    int32_t ReadArg(int type);                                      // MmlParser::ReadArg, 0xc211a0
    void CommandProc(uint32_t command, int32_t arg1, int32_t arg2); // MmlParser::CommandProc, 0xc21330
    void ReleaseAllChannel(int release);                            // 0xc0a2e0
    void FreeAllChannel();                                          // 0xc0a360
    void StopAllChannel();
    void SetMute(int mode);                // 0xc0ac10
    void OnChannelEvent(Channel& channel); // ChannelCallbackFunc, 0xc0ab90
    uint8_t ReadByte();
    uint32_t ReadOffset(); // a jump's, a call's or an opened track's offset
    Channel::Callback MakeCallback();

    // 0xc0ae40. Returns whether the note changes the sound (for the ripper's length analysis). A note changes the sound
    // unless it continues the newest note at the same key and velocity, with no sweep. Only a tie or a monophonic note
    // continues the newest note.
    bool NoteOn(int key, int velocity, int32_t length, bool tie);

    // The track went back to `target`, the start of a loop. The pass that ended there began at the track's last run of
    // `target`. A pass that changed nothing in the sound is idle, such as a pass that only polls or counts a variable.
    // An idle pass is never a loop of the sequence (SequenceSoundPlayer::on_loop). Any other pass is one if the track
    // is track 0, or if it's the main track (SequenceSoundPlayer::IsMainTrack) and played a note on the way round. A
    // pass that played no note is one too while the track holds a note that sounds until the game stops it
    // (HoldsSustainedNote) and every other open track rests (SequenceSoundPlayer::OthersRest). The pass then changes
    // that note, such as its volume or pitch.
    void CountLoop(uint32_t target);

    // The command being run changed the track's sound.
    void MarkChange();

    // The track read variable `index` in a command's argument or in a comparison. Whether a track has read variable
    // `index` so far: any track for the player's and the global variables, this one for the track's variables.
    void MarkRead(int index);
    bool IsRead(int index) const;

    // Whether the track holds a note that sounds until the game stops it on a looping wave, settled at its envelope's
    // sustain level. Such a note is a held note (see kHeldTicks) or a note of length 0.
    bool HoldsSustainedNote() const;

    // A variable by its command's index (GetVariablePtr, 0xc0ae20): 0 to 15 the player's variables, 16 to 31 the global
    // ones, 32 to 47 the track's variables.
    int16_t* Variable(int index);

    SequenceSoundPlayer* player_; // +0x1d8
    int index_;
    bool open_ = false; // +0x9

    // SetTrackVolume, SetTrackPitch and the rest: 1 and the defaults. The game calls them, and archive mode doesn't.
    float volume_ = 1.0f;    // +0xc
    float pitch_ = 1.0f;     // +0x10
    float pan_range_ = 1.0f; // +0x14
    OutputParam output_;     // +0x18

    std::span<const uint8_t> data_; // +0x70
    uint32_t current_ = 0;
    bool has_data_ = false;
    bool cmp_flag_ = true;                                                   // +0x78
    bool note_wait_ = true;                                                  // +0x79
    bool tie_ = false;                                                       // +0x7a
    bool mono_ = false;                                                      // +0x7b
    std::array<CallStackEntry, kCallStackDepth> call_stack_;                 // +0x7c
    uint8_t call_depth_ = 0;                                                 // +0x120
    bool front_bypass_ = false;                                              // +0x121 (InitParam leaves it)
    bool mute_ = false;                                                      // +0x122
    bool silence_ = false;                                                   // +0x123
    int32_t wait_ = 0;                                                       // +0x124
    bool note_finish_wait_ = false;                                          // +0x128
    bool porta_ = false;                                                     // +0x129
    bool damper_ = false;                                                    // +0x12a
    uint8_t bank_index_ = 0;                                                 // +0x12b
    uint32_t prg_no_ = 0;                                                    // +0x12c
    float sweep_pitch_ = 0.0f;                                               // +0x130
    MoveValue<uint8_t> vol_;                                                 // +0x134
    MoveValue<uint8_t> volume2_;                                             // +0x13a
    MoveValue<int8_t> pan_;                                                  // +0x140
    MoveValue<int8_t> span_;                                                 // +0x146
    MoveValue<int8_t> bend_;                                                 // +0x14c
    std::array<CurveLfoParam, 4> lfo_params_;                                // +0x154
    std::array<uint8_t, 4> lfo_types_{};                                     // +0x194
    uint8_t velocity_range_ = 127;                                           // +0x198
    uint8_t bend_range_ = 2;                                                 // +0x199
    int8_t init_pan_ = 0;                                                    // +0x19a
    int8_t transpose_ = 0;                                                   // +0x19c
    uint8_t priority_ = 64;                                                  // +0x19d
    uint8_t porta_key_ = 60;                                                 // +0x19e
    uint8_t porta_time_ = 0;                                                 // +0x19f
    uint8_t attack_ = 0xFF, decay_ = 0xFF, sustain_ = 0xFF, release_ = 0xFF; // +0x1a0
    int16_t hold_ = 0xFF;                                                    // +0x1a4
    int8_t biquad_type_ = 0;                                                 // +0x1a6
    uint8_t main_send_ = 127;                                                // +0x1a7
    std::array<uint8_t, 3> fx_send_{};                                       // +0x1a8
    float lpf_ = 0.0f;                                                       // +0x1ac
    float biquad_value_ = 0.0f;                                              // +0x1b0
    int32_t output_line_ = -1;                                               // +0x1b4: the player's output line when -1
    std::array<int16_t, 16> vars_{};                                         // +0x1b8
    Channel* channel_list_ = nullptr;                                        // +0x1e0

    // What the ripper's length analysis follows while the player has on_loop set. Times are counts of the commands that
    // the player's tracks have run (SequenceSoundPlayer::commands_run_).
    std::vector<uint64_t> run_;                // when the track last ran each command, or 0 if it hasn't
    uint64_t changed_at_ = 0;                  // when the track last changed the sound
    uint64_t drew_at_ = 0;                     // when the track last drew a random number
    uint64_t conditional_at_ = 0;              // when the track last ran a conditional command
    uint64_t looped_at_ = 0;                   // when the track last went back to the start of a loop
    uint64_t noted_at_ = 0;                    // when the track last played a note
    uint16_t variables_read_ = 0;              // the track's variables it has read, a bit each
    std::array<int16_t, 48> loop_variables_{}; // the variables that tracks read, at the last loop (0 for the others)
    bool idle_ = false;                        // whether the track's last pass of a loop changed nothing in the sound
    bool still_ = false; // whether that pass was idle, drew no random number and left the variables as the one before
    bool quiet_ = false; // whether that pass played no note
    bool conditional_ = false; // whether the command being run is conditional
    bool holds_ = false;       // whether the track's last note is a held one (see kHeldTicks)
    bool held_wait_ = false;   // whether the track waits out a held note
};

// What a player gets from its sound: the sound's info in the archive, the sequence's bytecode, and the banks by slot.
// The player's voices output to `receiver`.
class SequenceSoundPlayer
{
public:
    SequenceSoundPlayer(Engine& engine, const fsar::SoundInfo& info, std::span<const uint8_t> data,
                        const OutputReceiver* receiver);

    SequenceSoundPlayer(const SequenceSoundPlayer&) = delete;
    SequenceSoundPlayer& operator=(const SequenceSoundPlayer&) = delete;

    ~SequenceSoundPlayer();

    // Opens the tracks that the sound's allocation flags name. The first track opens at the sound's start.
    void Start();

    // Each sound frame: the ticks that come due in `frames` renderer frames of 5 ms, then each track's parameters reach
    // its channels (Update, 0xc007f0).
    void Update(int frames);

    bool IsFinished() const
    {
        return finished_;
    }

    // Track `index`, 0 to 15, or nullptr if it isn't allocated.
    const SequenceTrack* Track(int index) const
    {
        return index >= 0 && index < 16 ? tracks_[static_cast<std::size_t>(index)].get() : nullptr;
    }

    // Whether the sequence has started and hasn't ended.
    bool IsActive() const
    {
        return started_ && !finished_;
    }

    // Whether no open track will change the sound while the notes sound, or the tempo is 0 (for the ripper's length
    // analysis). Each open track waits for its notes to end, waits for ever, or only makes still passes
    // (SequenceTrack::IsStill) and has no note whose length will run out (SequenceTrack::HasEndingNote). Then the
    // sequence plays on unchanged while the notes sound.
    bool IsSettled() const;

    // Whether no open track will change the sound again, or the tempo is 0. Each open track waits for ever, or only
    // makes still passes that play no notes (SequenceTrack::IsQuiet).
    bool IsStuck() const;

    // Whether the main volume, or any open track's volume, pan or pitch bend, is moving to a new value.
    bool IsMoving() const;

    // When a track last changed the sound, counted in the commands that the tracks have run while on_loop is set (for
    // the ripper's length analysis). A track changes the sound when it plays a note, changes a setting or a ramp, or
    // opens a track. 0 if no track has.
    uint64_t ChangedAt() const
    {
        return changed_at_;
    }

    // A pass of track `track` that changed the sound ended with a jump back to `target`, a command the track has
    // already run, or at the end of a loop that repeats for ever and starts at `target`: a loop of the sequence (for
    // the ripper's length analysis). The track is track 0, or the main track if it played a note on the way round (see
    // IsMainTrack). A later track also counts if it holds a note that sounds until the game stops it while every other
    // open track rests (see OthersRest). A jump back into code that the track hasn't run isn't a loop. Sequences that
    // share their start make such jumps.
    std::function<void(int track, uint32_t target)> on_loop;

private:
    friend class SequenceTrack;

    Engine& GetEngine()
    {
        return *engine_;
    }

    // GetPlayerTrack, 0xbffa30.
    SequenceTrack* GetTrack(int index)
    {
        return index >= 0 && index < 16 ? tracks_[static_cast<std::size_t>(index)].get() : nullptr;
    }

    int16_t* Variable(int index); // GetVariablePtr, 0xbffca0

    // Whether track `index` is the main track: every track before it is closed, idle (SequenceTrack::IsIdle) or waiting
    // for ever (SequenceTrack::WaitsForEver), or comes after track 0 and plays no notes in its passes
    // (SequenceTrack::IsQuiet). The main track's loops are the sequence's loops. Track 0 keeps every later track from
    // the loops while its passes aren't idle.
    bool IsMainTrack(int index) const;

    // Whether every open track but track `index` is idle (SequenceTrack::IsIdle) or waits for ever
    // (SequenceTrack::WaitsForEver).
    bool OthersRest(int index) const;

    // Whether `test` holds for every open track (and true when none is open).
    bool EveryOpenTrack(const std::function<bool(const SequenceTrack& track)>& test) const;

    // A tick of every track. Returns whether none is open any more (part of UpdateTick).
    bool ParseNextTick(bool do_note_on);
    void UpdateTick(int frames); // 0xc010c0
    void FinishPlayer();         // 0xbff6d0
    void CloseTrack(int index);

    Engine* engine_;
    std::span<const uint8_t> data_;
    uint32_t allocate_track_flags_;
    uint32_t start_offset_;
    std::vector<uint32_t> bank_items_;                      // the bank in each slot, by item ID
    std::array<std::unique_ptr<SequenceTrack>, 16> tracks_; // +0x140
    std::array<int16_t, 16> local_vars_;                    // +0x1c0
    bool started_ = false;
    bool finished_ = false;

    // What the sound gives the player from its info and what the game sets (BasicSoundPlayer's parameters).
    const OutputReceiver* receiver_ = nullptr; // +0x30
    float volume_ = 1.0f;                      // +0x40
    float pitch_ = 1.0f;                       // +0x44
    float lpf_ = 0.0f;                         // +0x48
    float biquad_value_ = 0.0f;                // +0x4c
    int8_t biquad_type_ = -1;                  // +0x50: the tracks' biquad types unless 0 or more
    uint32_t pan_mode_ = 0;                    // +0x54
    uint32_t pan_curve_ = 0;                   // +0x58
    uint32_t output_line_ = 1;                 // +0x5c
    OutputParam output_;                       // +0x60

    bool release_priority_fix_ = false; // +0xf0
    float pan_range_ = 1.0f;            // +0xf4
    float tempo_ratio_ = 1.0f;          // +0xf8
    float tick_fraction_ = 0.0f;        // +0xfc
    uint8_t channel_priority_ = 64;     // +0x110
    uint8_t timebase_ = 48;             // +0x111
    uint16_t tempo_ = 120;              // +0x112
    MoveValue<uint8_t> main_volume_;    // +0x114

    // What the ripper's length analysis follows while on_loop is set: the commands that the tracks have run, when a
    // track last changed the sound (ChangedAt), and the player's and the global variables that a track has read, a bit
    // each.
    uint64_t commands_run_ = 0;
    uint64_t changed_at_ = 0;
    uint32_t variables_read_ = 0;
};

} // namespace hsf::atk
