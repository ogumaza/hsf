// SPDX-License-Identifier: MIT

// What an archive descriptor sets up on ntdlib's model of nn::atk (see model.h).

#include "hsf/model.h"

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
namespace
{

// The parameter set that an AuxEffect's 12 values give (I3dl2Values).
audio::I3dl2ReverbParameterSet I3dl2Parameters(const std::vector<float>& v)
{
    return {v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]};
}

// The nanoseconds that nn::atk takes for a time parameter of a delay, a light limiter or a reverb, given in whole
// microseconds.
int64_t Nanoseconds(float microseconds)
{
    return static_cast<int64_t>(microseconds) * 1000;
}

// The channel mode that the last parameter of a delay, a light limiter or a reverb gives.
atk::EffectBase::ChannelMode ChannelMode(float mode)
{
    return static_cast<atk::EffectBase::ChannelMode>(static_cast<uint8_t>(mode));
}

// nn::atk's delay with a delay's parameters (DelayParameter). The longest delay and the channel mode come first.
// nn::atk takes them only before the delay is added, and holds the delay to the longest delay.
std::unique_ptr<atk::EffectDelay> MakeDelay(const std::vector<float>& p)
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
std::unique_ptr<atk::EffectLightLimiter> MakeLightLimiter(const std::vector<float>& p)
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
std::unique_ptr<atk::EffectReverb> MakeReverb(const std::vector<float>& p)
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

} // namespace

atk::EngineOptions EngineOptionsFor(const ArchiveDescriptor& descriptor)
{
    atk::EngineOptions options;
    options.voice_count = static_cast<int>(descriptor.voice_count);

    return options;
}

std::vector<float> I3dl2Values(const audio::I3dl2ReverbParameterSet& p)
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

void AppendEffects(atk::Engine& engine, const std::vector<AuxEffect>& effects)
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

ScriptRunner::ScriptRunner(ScriptedStart script, const std::array<float, SoundTrackVolumes::kTracks>& volumes,
                           const UserProcedureHandler& procedures)
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

ScriptRunner::~ScriptRunner()
{
    if (hooked_ != nullptr)
    {
        hooked_->SetUserProcedureHook(nullptr);
    }
}

std::optional<std::string> ScriptRunner::Apply(atk::Engine& engine, uint64_t frame)
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
                players_[i]->SetTrackVolumes(volumes_);
            }
        }
        else
        {
            Write(engine, step, PlayerOf(step.sound, i));
        }
    }

    return std::nullopt;
}

bool ScriptRunner::Pending(uint64_t frame) const
{
    return std::any_of(script_.steps.begin(), script_.steps.end(),
                       [frame](const ScriptStep& step) { return step.frame > frame; });
}

atk::SequenceSoundPlayer* ScriptRunner::Sound() const
{
    return PlayerOf(script_.sound, script_.steps.size());
}

void ScriptRunner::Rewind(uint64_t frame, const State& state)
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

void ScriptRunner::Write(atk::Engine& engine, const ScriptStep& step, atk::SequenceSoundPlayer* player)
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

void ScriptRunner::OnProcedure(atk::Engine& engine, atk::SequenceSoundPlayer& player, int track, uint16_t procedure)
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

void ScriptRunner::RunHandler(atk::Engine& engine, atk::SequenceSoundPlayer& player, int track, uint16_t procedure)
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

atk::SequenceSoundPlayer* ScriptRunner::PlayerOf(uint32_t sound, std::size_t end) const
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

EffectTail::EffectTail(const std::vector<AuxEffect>& effects)
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

void EffectTail::Frame(std::span<const int16_t> output)
{
    const auto nonzero = [](int16_t sample)
    {
        return sample != 0;
    };
    const bool rang = audible_ ? xsflib::rip::Audible(output) : std::any_of(output.begin(), output.end(), nonzero);
    quiet_frames_ = rang ? 0 : quiet_frames_ + 1;
}

} // namespace hsf
