// SPDX-License-Identifier: MIT

// What an archive descriptor sets up on ntdlib's model of nn::atk: the engine's options, and the effects on the aux
// buses. The player and the ripper's length analysis both set the model up with these functions. A rip's length then
// comes from the sound that the player plays.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/nx/atk/effect.h"
#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/nx/audio/i3dl2.h"
#include "ntdlib/nx/bex/user_procedures.h"
#include "xsflib/rip/length.h"
#include "xsflib/text.h"

#include "hsf/format.h"
#include "hsf/nx.h"
#include "hsf/sar.h"

namespace hsf
{

// The engine's options for a set with `descriptor`: nn::atk's voice count.
inline atk::EngineOptions EngineOptionsFor(const ArchiveDescriptor& descriptor)
{
    atk::EngineOptions options;
    options.voice_count = static_cast<int>(descriptor.voice_count);

    return options;
}

// An I3DL2 reverb's parameters as an AuxEffect holds them: the 12 fields of nn::audio's I3dl2ReverbParameterSet, in
// their order.
inline std::vector<float> I3dl2Values(const audio::I3dl2ReverbParameterSet& p)
{
    return {p.room_gain,
            p.room_hf_gain,
            p.late_reverb_decay_time,
            p.late_reverb_hf_decay_ratio,
            p.reflections_gain,
            p.reflections_delay,
            p.reverb_gain,
            p.late_reverb_delay,
            p.late_reverb_diffusion,
            p.late_reverb_density,
            p.hf_reference,
            p.dry_gain};
}

// The parameter set that an AuxEffect's 12 values give (I3dl2Values).
inline audio::I3dl2ReverbParameterSet I3dl2Parameters(const std::vector<float>& v)
{
    return {v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]};
}

// The nanoseconds that nn::atk takes for a time parameter of a delay, a light limiter or a reverb, given in whole
// microseconds.
inline int64_t Nanoseconds(float microseconds)
{
    return static_cast<int64_t>(microseconds) * 1000;
}

// The channel mode that the last parameter of a delay, a light limiter or a reverb gives.
inline atk::EffectBase::ChannelMode ChannelMode(float mode)
{
    return static_cast<atk::EffectBase::ChannelMode>(static_cast<uint8_t>(mode));
}

// nn::atk's delay with a delay's parameters (DelayParameter). The longest delay and the channel mode come first.
// nn::atk takes them only before the delay is added, and holds the delay to the longest delay.
inline std::unique_ptr<atk::EffectDelay> MakeDelay(const std::vector<float>& p)
{
    auto delay = std::make_unique<atk::EffectDelay>();
    delay->SetDelayTimeMax(Nanoseconds(p[kDelayTimeMax]));
    delay->SetChannelMode(ChannelMode(p[kDelayChannelMode]));
    delay->SetDelayTime(Nanoseconds(p[kDelayTime]));
    delay->SetInGain(p[kDelayInGain]);
    delay->SetFeedbackGain(p[kDelayFeedbackGain]);
    delay->SetDryGain(p[kDelayDryGain]);
    delay->SetLowPassAmount(p[kDelayLowPassAmount]);
    delay->SetChannelSpread(p[kDelayChannelSpread]);

    return delay;
}

// nn::atk's light limiter with a light limiter's parameters (LightLimiterParameter). The longest look-ahead and the
// channel mode come first. They work as a delay's longest delay and channel mode do.
inline std::unique_ptr<atk::EffectLightLimiter> MakeLightLimiter(const std::vector<float>& p)
{
    auto limiter = std::make_unique<atk::EffectLightLimiter>();
    limiter->SetLookAheadTimeMax(Nanoseconds(p[kLimiterLookAheadTimeMax]));
    limiter->SetChannelMode(ChannelMode(p[kLimiterChannelMode]));
    limiter->SetAttackTime(Nanoseconds(p[kLimiterAttackTime]));
    limiter->SetReleaseTime(Nanoseconds(p[kLimiterReleaseTime]));
    limiter->SetLookAheadTime(Nanoseconds(p[kLimiterLookAheadTime]));
    limiter->SetThreshold(p[kLimiterThreshold]);
    limiter->SetInputGain(p[kLimiterInputGain]);
    limiter->SetOutputGain(p[kLimiterOutputGain]);

    return limiter;
}

// nn::atk's reverb with a reverb's parameters (ReverbParameter). The channel mode comes first. nn::atk takes it only
// before the reverb is added. The setters hold the gains, the ratio and the times as Clubhouse Games' nn::atk does.
inline std::unique_ptr<atk::EffectReverb> MakeReverb(const std::vector<float>& p)
{
    auto reverb = std::make_unique<atk::EffectReverb>();
    reverb->SetChannelMode(ChannelMode(p[kReverbChannelMode]));
    reverb->SetEarlyMode(static_cast<int>(p[kReverbEarlyMode]));
    reverb->SetEarlyGain(p[kReverbEarlyGain]);
    reverb->SetPredelayTime(Nanoseconds(p[kReverbPredelay]));
    reverb->SetLateMode(static_cast<int>(p[kReverbLateMode]));
    reverb->SetLateGain(p[kReverbLateGain]);
    reverb->SetDecayTime(Nanoseconds(p[kReverbDecay]));
    reverb->SetHighFrequencyDecayRatio(p[kReverbHighFrequencyDecayRatio]);
    reverb->SetColoration(p[kReverbColoration]);
    reverb->SetReverbGain(p[kReverbGain]);
    reverb->SetOutGain(p[kReverbOutGain]);
    reverb->SetDryGain(p[kReverbDryGain]);

    return reverb;
}

// Puts `effects` on the aux buses of `engine` in their order, before the sound starts. Each effect is one that
// ArchiveDescriptor::Check accepts: an I3DL2 reverb, a delay, a light limiter or a reverb.
inline void AppendEffects(atk::Engine& engine, const std::vector<AuxEffect>& effects)
{
    for (const AuxEffect& effect : effects)
    {
        const int bus = static_cast<int>(effect.bus);
        if (effect.type == AuxEffect::kDelay)
        {
            engine.AppendEffect(bus, MakeDelay(effect.parameters));
        }
        else if (effect.type == AuxEffect::kLightLimiter)
        {
            engine.AppendEffect(bus, MakeLightLimiter(effect.parameters));
        }
        else if (effect.type == AuxEffect::kReverb)
        {
            engine.AppendEffect(bus, MakeReverb(effect.parameters));
        }
        else
        {
            engine.AppendEffect(bus, std::make_unique<atk::I3dl2AuxEffect>(I3dl2Parameters(effect.parameters)));
        }
    }
}

// Gives the tracks of the sequence that `player` plays `volumes`, as the game's code does through the sound's handle
// right after the start (nn::atk's SetTrackVolume). The volumes take effect before the sequence's first tick. The
// tracks start at 1. When every volume is 1, the function leaves the player as it is.
inline void SetTrackVolumes(atk::SequenceSoundPlayer& player,
                            const std::array<float, SoundTrackVolumes::kTracks>& volumes)
{
    const auto full = [](float volume)
    {
        return volume == 1.0f;
    };
    if (std::all_of(volumes.begin(), volumes.end(), full))
    {
        return;
    }

    for (std::size_t t = 0; t < volumes.size(); ++t)
    {
        player.SetTrackVolume(1u << t, volumes[t]);
    }
}

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
                 const UserProcedureHandler& procedures = {})
        : script_(std::move(script)), volumes_(volumes), players_(script_.steps.size(), nullptr)
    {
        state_.taken.assign(script_.steps.size(), false);
        in_procedure_ = std::any_of(script_.steps.begin(), script_.steps.end(),
                                    [](const ScriptStep& step) { return step.kind == ScriptStep::kWriteInProcedure; });

        if (procedures.handler != UserProcedureHandler::kNone)
        {
            auto& model = state_.procedures.emplace(static_cast<bex::ProcedureHandler>(procedures.handler));
            for (const UserProcedureTable& table : procedures.tables)
            {
                model.SetTable(table.number, table.values);
            }

            in_procedure_ = true;
        }
    }

    ScriptRunner(const ScriptRunner&) = delete;
    ScriptRunner& operator=(const ScriptRunner&) = delete;

    ~ScriptRunner()
    {
        if (hooked_ != nullptr)
        {
            hooked_->SetUserProcedureHook(nullptr);
        }
    }

    const ScriptedStart& Script() const
    {
        return script_;
    }

    // Takes the steps of frame `frame`. Returns why a start failed: its sound isn't a sequence whose files the archive
    // holds.
    std::optional<std::string> Apply(atk::Engine& engine, uint64_t frame)
    {
        current_ = frame;
        if (in_procedure_ && hooked_ != &engine)
        {
            engine.SetUserProcedureHook([this, &engine](atk::SequenceSoundPlayer& player, int track, uint16_t procedure)
                                        { OnProcedure(engine, player, track, procedure); });
            hooked_ = &engine;
        }

        for (std::size_t i = 0; i < script_.steps.size(); ++i)
        {
            const ScriptStep& step = script_.steps[i];
            if (step.frame != frame || step.kind == ScriptStep::kWriteInProcedure)
            {
                continue;
            }

            if (step.kind == ScriptStep::kStart)
            {
                const int archive = step.variable == ScriptStep::kNone ? 0 : step.variable + 1;
                players_[i] = fsar::ItemType(step.sound) == fsar::kSoundItem
                                  ? engine.StartSequence(fsar::ItemNumber(step.sound), archive)
                                  : nullptr;
                if (!players_[i])
                {
                    return "sound " + xsflib::Hex(step.sound, 8) + " isn't a sequence whose files the set holds";
                }

                if (step.sound == script_.sound)
                {
                    SetTrackVolumes(*players_[i], volumes_);
                }
            }
            else
            {
                Write(engine, step, PlayerOf(step.sound, i));
            }
        }

        return std::nullopt;
    }

    // Whether a step comes after frame `frame`. The set plays on until the last step.
    bool Pending(uint64_t frame) const
    {
        return std::any_of(script_.steps.begin(), script_.steps.end(),
                           [frame](const ScriptStep& step) { return step.frame > frame; });
    }

    // The player of the script's sound, once a step has started it, or null.
    atk::SequenceSoundPlayer* Sound() const
    {
        return PlayerOf(script_.sound, script_.steps.size());
    }

    // What has changed so far, for a snapshot of the model.
    const State& Saved() const
    {
        return state_;
    }

    // Returns to where the runner stood before frame `frame`, once it had taken the steps of the frame before: a
    // restore of the engine to a snapshot taken there. `state` is what Saved gave then. The players that the steps of
    // frame `frame` and later started are dropped.
    void Rewind(uint64_t frame, const State& state)
    {
        for (std::size_t i = 0; i < script_.steps.size(); ++i)
        {
            if (script_.steps[i].frame >= frame)
            {
                players_[i] = nullptr;
            }
        }

        state_ = state;
        current_ = frame == 0 ? 0 : frame - 1;
        first_waiting_ = 0;
    }

private:
    // Sets the variable of a write. `player` plays the step's sound, or is null before a step has started it.
    void Write(atk::Engine& engine, const ScriptStep& step, atk::SequenceSoundPlayer* player)
    {
        if (step.variable >= ScriptStep::kLocalEnd && step.variable < ScriptStep::kGlobalEnd)
        {
            engine.GlobalVariables()[step.variable - ScriptStep::kLocalEnd] = step.value;
        }
        else if (player != nullptr && step.variable < ScriptStep::kLocalEnd)
        {
            player->SetVariable(engine, step.variable, step.value);
        }
        else if (player != nullptr)
        {
            player->SetTrackVariable(step.track, step.variable - ScriptStep::kGlobalEnd, step.value);
        }
    }

    // A call of user procedure `procedure` by track `track` of the sequence that `player` plays, in the frame after
    // the steps of frame current_. The game's handler runs first (RunHandler). Then the writes of kind 2 that wait for
    // the call take effect: those from current_ or before, of the procedure and the player's sound, that haven't yet.
    // The steps come in frame order.
    void OnProcedure(atk::Engine& engine, atk::SequenceSoundPlayer& player, int track, uint16_t procedure)
    {
        RunHandler(engine, player, track, procedure);

        std::vector<bool>& taken = state_.taken;
        while (first_waiting_ < script_.steps.size() &&
               (script_.steps[first_waiting_].kind != ScriptStep::kWriteInProcedure || taken[first_waiting_]))
        {
            ++first_waiting_;
        }

        for (std::size_t i = first_waiting_; i < script_.steps.size(); ++i)
        {
            const ScriptStep& step = script_.steps[i];
            if (step.frame > current_)
            {
                break;
            }
            if (step.kind != ScriptStep::kWriteInProcedure || step.procedure != procedure || taken[i] ||
                PlayerOf(step.sound, i) != &player)
            {
                continue;
            }

            Write(engine, step, &player);
            taken[i] = true;
        }
    }

    // Runs the game's handler of user procedures, if the descriptor names one, on the variables that the call of
    // `procedure` by track `track` of `player` sees. The handler's changes go back to the player, the track and the
    // global variables. The game's code writes them in the same way, through the callback's pointers.
    void RunHandler(atk::Engine& engine, atk::SequenceSoundPlayer& player, int track, uint16_t procedure)
    {
        if (!state_.procedures)
        {
            return;
        }

        bex::ProcedureVariables variables;
        variables.local = player.LocalVariables();
        variables.global = engine.GlobalVariables();
        if (const auto* calling = player.Track(track))
        {
            variables.track = calling->Variables();
        }

        const bex::ProcedureVariables before = variables;
        if (!state_.procedures->Run(procedure, variables))
        {
            return;
        }

        for (std::size_t i = 0; i < variables.local.size(); ++i)
        {
            if (variables.local[i] != before.local[i])
            {
                player.SetVariable(engine, static_cast<int>(i), variables.local[i]);
            }

            if (variables.global[i] != before.global[i])
            {
                engine.GlobalVariables()[i] = variables.global[i];
            }

            if (variables.track[i] != before.track[i])
            {
                player.SetTrackVariable(track, static_cast<int>(i), variables.track[i]);
            }
        }
    }

    // The player that the last start of `sound` before step `end` started, or null.
    atk::SequenceSoundPlayer* PlayerOf(uint32_t sound, std::size_t end) const
    {
        for (std::size_t i = end; i-- > 0;)
        {
            if (script_.steps[i].kind == ScriptStep::kStart && script_.steps[i].sound == sound)
            {
                return players_[i];
            }
        }

        return nullptr;
    }

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

    explicit EffectTail(const std::vector<AuxEffect>& effects)
    {
        for (const AuxEffect& effect : effects)
        {
            if (effect.type == AuxEffect::kReverb)
            {
                const auto frames =
                    static_cast<uint64_t>((kReverbMicroseconds + kFrameMicroseconds - 1) / kFrameMicroseconds);
                hold_frames_ = std::max(hold_frames_, frames + 1);
                audible_ = true;
                continue;
            }

            if (effect.type != AuxEffect::kDelay)
            {
                continue;
            }

            // nn::atk holds the delay to the longest delay.
            const auto delay =
                static_cast<int64_t>(std::min(effect.parameters[kDelayTime], effect.parameters[kDelayTimeMax]));
            const auto frames = static_cast<uint64_t>((delay + kFrameMicroseconds - 1) / kFrameMicroseconds);
            hold_frames_ = std::max(hold_frames_, frames + 1);
            audible_ = true;
        }

        quiet_frames_ = hold_frames_;
    }

    // Takes a frame's output.
    void Frame(std::span<const int16_t> output)
    {
        const auto nonzero = [](int16_t sample)
        {
            return sample != 0;
        };
        const bool rang = audible_ ? xsflib::rip::Audible(output) : std::any_of(output.begin(), output.end(), nonzero);
        quiet_frames_ = rang ? 0 : quiet_frames_ + 1;
    }

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
