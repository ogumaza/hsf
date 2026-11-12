// SPDX-License-Identifier: MIT

// What an archive descriptor sets up on ntdlib's model of nn::atk: the engine's options, and the effects on the aux
// buses. The player and the ripper's length analysis both set the model up with these functions. A rip's length then
// comes from the sound that the player plays.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/nx/audio/i3dl2.h"
#include "ntdlib/nx/bex/user_procedures.h"

#include "hsf/format.h"
#include "hsf/nx.h"

namespace hsf
{

// The engine's options for a set with `descriptor`: nn::atk's voice count.
atk::EngineOptions EngineOptionsFor(const ArchiveDescriptor& descriptor);

// An I3DL2 reverb's parameters as an AuxEffect holds them: the 12 fields of nn::audio's I3dl2ReverbParameterSet, in
// their order.
std::vector<float> I3dl2Values(const audio::I3dl2ReverbParameterSet& p);

// Puts `effects` on the aux buses of `engine` in their order, before the sound starts. Each effect is one that
// ArchiveDescriptor::Check accepts: an I3DL2 reverb, a delay, a light limiter or a reverb.
void AppendEffects(atk::Engine& engine, const std::vector<AuxEffect>& effects);

// Runs a sound's scripted start on a model (docs/hsf.md, "Scripted starts"). Before the model runs a frame, Apply
// takes the steps of that frame, in their order. A start starts a sequence sound, from the engine's archive or from the
// add-on archive that the engine numbers one more than the step's archive (atk::Engine::AddArchive). The script's sound
// gets its track volumes there. A write sets a global variable, or a variable or track's variable of a sound that a
// step has started: nn::atk's SetGlobalVariable, WriteVariable and WriteTrackVariable. A track's variable that a write
// sets before the sequence allocates its tracks takes effect when it does. Every variable starts at -1.
//
// A write of kind 2 waits from its frame for the next call of its user procedure by the sound's sequence, on any track.
// It takes effect inside that call, before the track's next command. A write of the game's procedure does the same. The
// writes that wait for the same call take effect in their order. The engine's hook (atk::Engine::SetUserProcedureHook)
// passes the calls to the runner. Apply sets the hook when the script has such a write, and the destructor removes it.
// The runner can't be copied or moved, and has to outlive the engine's frames.
//
// The descriptor can name the game's handler of user procedures (docs/hsf.md, "User procedures"). The runner then runs
// ntdlib's model of the handler in every call of every sound, from the state that the game's constructor gives it. The
// handler's writes come before the writes of kind 2 in the same call.
class ScriptRunner
{
public:
    // What changes as the model runs: which writes of kind 2 have taken effect, and the handler's state.
    struct State
    {
        std::vector<bool> taken;
        std::optional<bex::UserProcedures> procedures;
    };

    ScriptRunner(ScriptedStart script, const std::array<float, SoundTrackVolumes::kTracks>& volumes,
                 const UserProcedureHandler& procedures = {});

    ScriptRunner(const ScriptRunner&) = delete;
    ScriptRunner& operator=(const ScriptRunner&) = delete;

    ~ScriptRunner();

    // Takes the steps of frame `frame`. Returns why a start failed: its sound isn't a sequence whose files the archive
    // holds.
    std::optional<std::string> Apply(atk::Engine& engine, uint64_t frame);

    // Whether a step comes after frame `frame`. The set plays on until the last step.
    bool Pending(uint64_t frame) const;

    // The player of the script's sound, once a step has started it, or null.
    atk::SequenceSoundPlayer* Sound() const;

    // What has changed so far, for a snapshot of the model.
    const State& Saved() const
    {
        return state_;
    }

    // Returns to where the runner stood before frame `frame`, once it had taken the steps of the frame before: a
    // restore of the engine to a snapshot taken there. `state` is what Saved gave then. The players that the steps of
    // frame `frame` and later started are dropped.
    void Rewind(uint64_t frame, const State& state);

private:
    // Sets the variable of a write. `player` plays the step's sound, or is null before a step has started it.
    void Write(atk::Engine& engine, const ScriptStep& step, atk::SequenceSoundPlayer* player);

    // A call of user procedure `procedure` by track `track` of the sequence that `player` plays, in the frame after
    // the steps of frame current_. The game's handler runs first (RunHandler). Then the writes of kind 2 that wait for
    // the call take effect: those from current_ or before, of the procedure and the player's sound, that haven't yet.
    // The steps come in frame order.
    void OnProcedure(atk::Engine& engine, atk::SequenceSoundPlayer& player, int track, uint16_t procedure);

    // Runs the game's handler of user procedures, if the descriptor names one, on the variables that the call of
    // `procedure` by track `track` of `player` sees. The handler's changes go back to the player, the track and the
    // global variables. The game's code writes them in the same way, through the callback's pointers.
    void RunHandler(atk::Engine& engine, atk::SequenceSoundPlayer& player, int track, uint16_t procedure);

    // The player that the last start of `sound` before step `end` started, or null.
    atk::SequenceSoundPlayer* PlayerOf(uint32_t sound, std::size_t end) const;

    ScriptedStart script_;
    std::array<float, SoundTrackVolumes::kTracks> volumes_;
    std::vector<atk::SequenceSoundPlayer*> players_; // for each start, the player it started
    State state_;                                    // the writes of kind 2 taken, and the handler's state
    bool in_procedure_ = false;                      // whether the runner takes calls of user procedures
    atk::Engine* hooked_ = nullptr;                  // the engine whose hook calls the runner
    uint64_t current_ = 0;                           // the frame of the steps that Apply took last
    std::size_t first_waiting_ = 0;                  // no write of kind 2 before this step waits
};

// How long the effects ring in the output once the sound has stopped (docs/hsf.md, "The model"). The length analysis
// and the player both ask. Without a delay, the effects ring while a frame's output has a sample other than 0. The
// renderer's delay rounds its products down. Once its line holds a negative sample, its output stays at -1 or so for
// ever. With a delay, the effects therefore ring until no frame has been audible (xsflib::rip::Audible) for the delay's
// time, in whole frames rounded up, and one frame more. nn::atk holds the delay's time to the longest delay. With more
// than one delay, the one with the most frames counts. A delay's feedback is less than 1. After a whole delay without
// an audible frame, no echo can be audible. The renderer's reverb rounds its products down too. With a reverb, the
// effects ring until no frame has been audible for the reverb's lines at their longest, one after another, and one
// frame more. Those are the pre-delay line (350 ms), the longest feedback line (171 ms), its all-pass (17 ms) and the
// centre line (5 ms). Each sample in the reverb reaches an output within that time. The reverb's feedback is less than
// 1.
class EffectTail
{
public:
    // The renderer's frame: 5 ms.
    static constexpr int64_t kFrameMicroseconds = 5000;

    // The reverb's lines at their longest, one after another, rounded up to a microsecond. ntdlib's model of the reverb
    // gives the longest feedback line as 2795361 / 16384 ms.
    static constexpr int64_t kReverbMicroseconds = 542'616;

    explicit EffectTail(const std::vector<AuxEffect>& effects);

    // Takes a frame's output.
    void Frame(std::span<const int16_t> output);

    // Whether the effects may still ring after the frames taken so far.
    bool Ringing() const
    {
        return quiet_frames_ < hold_frames_;
    }

private:
    bool audible_ = false;      // with a delay or a reverb: only an audible frame rings
    uint64_t hold_frames_ = 1;  // how many frames in a row that don't ring end the ringing
    uint64_t quiet_frames_ = 1; // the frames since the last that rang
};

} // namespace hsf
