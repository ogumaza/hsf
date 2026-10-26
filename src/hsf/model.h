// SPDX-License-Identifier: MIT

// What an archive descriptor sets up on ntdlib's model of nn::atk: the engine's options, and the effects on the aux
// buses. The player and the ripper's length analysis both set the model up with these functions. A rip's length then
// comes from the sound that the player plays.

#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "ntdlib/nx/atk/effect.h"
#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/nx/audio/i3dl2.h"
#include "xsflib/rip/length.h"

#include "hsf/format.h"
#include "hsf/nx.h"

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

// The nanoseconds that nn::atk takes for a time parameter of a delay or a light limiter, given in whole microseconds.
inline int64_t Nanoseconds(float microseconds)
{
    return static_cast<int64_t>(microseconds) * 1000;
}

// The channel mode that the last parameter of a delay or a light limiter gives.
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

// Puts `effects` on the aux buses of `engine` in their order, before the sound starts. Each effect is one that
// ArchiveDescriptor::Check accepts: an I3DL2 reverb, a delay or a light limiter.
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
        else
        {
            engine.AppendEffect(bus, std::make_unique<atk::I3dl2AuxEffect>(I3dl2Parameters(effect.parameters)));
        }
    }
}

// How long the effects ring in the output once the sound has stopped (docs/hsf.md, "The model"). The length analysis
// and the player both ask. Without a delay, the effects ring while a frame's output has a sample other than 0. The
// renderer's delay rounds its products down. Once its line holds a negative sample, its output stays at -1 or so for
// ever. With a delay, the effects therefore ring until no frame has been audible (xsflib::rip::Audible) for the delay's
// time, in whole frames rounded up, and one frame more. nn::atk holds the delay's time to the longest delay. With more
// than one delay, the one with the most frames counts. A delay's feedback is less than 1. After a whole delay without
// an audible frame, no echo can be audible.
class EffectTail
{
public:
    // The renderer's frame: 5 ms.
    static constexpr int64_t kFrameMicroseconds = 5000;

    explicit EffectTail(const std::vector<AuxEffect>& effects)
    {
        for (const AuxEffect& effect : effects)
        {
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
    bool audible_ = false;      // with a delay: only an audible frame rings
    uint64_t hold_frames_ = 1;  // how many frames in a row that don't ring end the ringing
    uint64_t quiet_frames_ = 1; // the frames since the last that rang
};

} // namespace hsf
