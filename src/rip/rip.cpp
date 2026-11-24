// SPDX-License-Identifier: MIT

#include "rip/rip.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ntdlib/first_sample.h"
#include "ntdlib/nwsnd/length_model.h"
#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/nx/atk/length_model.h"
#include "ntdlib/nx/atk/sequence.h"
#include "xsflib/rip/length.h"

#include "hsf/format.h"
#include "hsf/model.h"
#include "hsf/nx.h"
#include "hsf/sar.h"

namespace hsf::rip
{
namespace
{

// What the length analysis measures beside ntdlib's model of nn::atk: the script that starts the sound, the effects'
// tail (EffectTail, hsf/model.h), and the output scaled by the volume tag of the mini. A player scales its output the
// same way. `engine` is the engine of `model`.
struct ScriptedModel
{
    ScriptedModel(std::unique_ptr<atk::ArchiveModel> archive_model, const ScriptedStart& start,
                  const ntdlib::rip::TrackVolumes& track_volumes, const ArchiveDescriptor& settings,
                  float output_volume)
        : model(std::move(archive_model)),
          engine(model->engine.get()),
          script(start, track_volumes, settings.procedures),
          tail(settings.effects),
          frame(start.lead_in),
          volume(output_volume)
    {
    }

    std::unique_ptr<atk::ArchiveModel> model;
    atk::Engine* engine;
    ScriptRunner script;
    EffectTail tail;
    uint64_t frame;                  // the model's frames, the script's lead-in included
    float volume;                    // the mini's volume tag, the scale of the output
    std::span<const int16_t> output; // the frame's output, scaled
    std::vector<int16_t> scaled;     // the frame's output, scaled, when the volume isn't 1
};

// The console of the length analysis (ntdlib::nwsnd::LengthModel): the console of the Switch (atk::LengthConsole), with
// the script, the tail and the volume. The output is the device output, before the volume. The player looks for the
// first sound in those samples, and a volume under 0.5 can round a sample to 0. Whether a frame sounded comes from the
// scaled output. The model counts each track's passes. A looping sequence's fade then waits for the tracks that loop at
// other lengths than the main track (OthersLooped). A track that another track opens again makes a pass each time. The
// open clears the track's wait, and the track starts its part again at once. The fade therefore doesn't wait for the
// tracks that a looping track opens again in each of its loops.
struct ScriptedConsole
{
    using Model = ScriptedModel;

    static constexpr double kFrameMs = atk::LengthConsole::kFrameMs;

    // The model has run `frame` frames, and the script has taken the steps of those frames and of the next. The script
    // takes the steps of each later frame after the frame before it. A start that fails stops the measure.
    static bool RunFrame(Model& m)
    {
        m.engine->RunFrame();
        ++m.frame;
        m.output = Scaled(m, m.model->renderer.DeviceOutput());
        m.tail.Frame(m.output);

        return !m.script.Apply(*m.engine, m.frame);
    }

    static std::span<const int16_t> Output(const Model& m)
    {
        return m.model->renderer.DeviceOutput();
    }

    static bool Sounded(const Model& m)
    {
        return xsflib::rip::Audible(m.output);
    }

    // The effects ring on after the sequence (EffectTail).
    static bool Ringing(const Model& m)
    {
        return m.tail.Ringing();
    }

    static std::size_t VoiceCount(const Model& m)
    {
        return atk::LengthConsole::VoiceCount(*m.model);
    }

    static xsflib::rip::VoicePlace Voice(const Model& m, std::size_t index)
    {
        return atk::LengthConsole::Voice(*m.model, index);
    }

    // A sequence that waits for a value from a later step of the script isn't settled or stuck. The set plays on until
    // the last step.
    static bool Pending(const Model& m)
    {
        return m.script.Pending(m.frame);
    }

    // A frame's output as a player plays it with the mini's volume tag.
    static std::span<const int16_t> Scaled(Model& m, std::span<const int16_t> output)
    {
        if (m.volume == 1.0f)
        {
            return output;
        }

        m.scaled.resize(output.size());
        std::transform(output.begin(), output.end(), m.scaled.begin(),
                       [volume = m.volume](int16_t sample)
                       {
                           const double value = std::round(static_cast<double>(sample) * static_cast<double>(volume));
                           return static_cast<int16_t>(std::clamp(value, -32768.0, 32767.0));
                       });

        return m.scaled;
    }
};

// The milliseconds from the start of a frame of the device's output to its first sample that isn't 0, on either side,
// or nothing when every sample is 0. The frame's 240 stereo pairs share its 5 ms.
std::optional<double> DeviceFirstSampleMs(std::span<const int16_t> device_output)
{
    if (device_output.size() < 2)
    {
        return std::nullopt;
    }

    const double pair_ms = 2.0 * atk::LengthConsole::kFrameMs / static_cast<double>(device_output.size());
    return ntdlib::FirstSampleMs(device_output, pair_ms);
}

// A model of `archive` and the add-on archives `addons`, set up for `settings`, with the effects on the aux buses.
std::unique_ptr<atk::ArchiveModel> MakeModel(const fsar::SoundArchive& archive, const ArchiveDescriptor& settings,
                                             std::span<const fsar::SoundArchive* const> addons = {})
{
    auto model = std::make_unique<atk::ArchiveModel>(archive, EngineOptionsFor(settings));
    for (const fsar::SoundArchive* addon : addons)
    {
        model->engine->AddArchive(*addon);
    }

    AppendEffects(*model->engine, settings.effects);

    return model;
}

// Takes the steps of frame 0 of the script that `runner` runs, then runs the `lead_in` frames of its lead-in, each
// followed by the steps of the next frame. Returns false when a start fails.
bool RunLeadIn(atk::Engine& engine, ScriptRunner& runner, uint32_t lead_in)
{
    if (runner.Apply(engine, 0))
    {
        return false;
    }

    for (uint64_t frame = 1; frame <= lead_in; ++frame)
    {
        engine.RunFrame();
        if (runner.Apply(engine, frame))
        {
            return false;
        }
    }

    return true;
}

// Measures a sequence on the model, set up for `settings`, with `track_volumes` for its tracks and its output scaled by
// `volume` (ntdlib::nwsnd::MeasureSequence). The sound starts by `script`, just like in the player. The measure starts
// after the lead-in. A sequence can repeat its music in a counted loop that the game plays for longer than the limit.
// When the measure reaches the limit inside counted loops, it runs again with each track's outermost counted loop
// endless, by the rule of ntdlib's length analysis. Their passes then count as loops of the sequence. An inner loop
// that runs at the limit, such as a fill, still ends, and repeats in each pass of the outer loop as the game plays it.
// With the long limit, a measure that reaches the limit runs again under xsflib's 3,600 s, with those loops endless if
// it ended inside any. The second measure is final.
std::optional<Timing> MeasureSequence(const fsar::SoundArchive& archive, const ArchiveDescriptor& settings,
                                      const ntdlib::rip::TrackVolumes& track_volumes, const ScriptedStart& script,
                                      float volume, xsflib::rip::Limit limit)
{
    const auto make_model = [&archive, &settings, &track_volumes, &script, volume]
    {
        return std::make_unique<ScriptedModel>(MakeModel(archive, settings), script, track_volumes, settings, volume);
    };
    const auto start = [&script](ScriptedModel& m) -> atk::SequenceSoundPlayer*
    {
        return RunLeadIn(*m.engine, m.script, script.lead_in) ? m.script.Sound() : nullptr;
    };

    return ntdlib::nwsnd::MeasureSequence<ScriptedConsole>(
        make_model, start, ntdlib::nwsnd::LengthRulesFor(atk::LengthConsole::kFrameMs), {}, limit);
}

// What a run of a scripted start shows in its first frames after the lead-in: whether the script's sound made a sound,
// and whether a sequence drew a random number.
struct StartProbe
{
    bool sounded = false;
    bool drew = false;
};

// Runs `script` on the model, set up for `settings`, for its lead-in and at most `window` frames more. The run stops at
// the first frame after the lead-in whose output is audible. A sequence that draws a number steps nn::atk's random
// numbers once more than the frames do (atk::Engine::FrameProcess steps them once a frame). The run therefore compares
// the generator's state with a copy that steps only once a frame.
StartProbe ProbeStart(const fsar::SoundArchive& archive, const ArchiveDescriptor& settings,
                      const ntdlib::rip::TrackVolumes& track_volumes, const ScriptedStart& script, uint32_t window)
{
    const std::unique_ptr<atk::ArchiveModel> model = MakeModel(archive, settings);
    atk::Engine& engine = *model->engine;
    ScriptRunner runner(script, track_volumes, settings.procedures);
    StartProbe probe;
    if (runner.Apply(engine, 0))
    {
        return probe;
    }

    atk::Random frames_only = engine.Rng();
    const uint64_t end = uint64_t{script.lead_in} + window;
    for (uint64_t frame = 1; frame <= end; ++frame)
    {
        engine.RunFrame();
        frames_only.Next();
        if (frame > script.lead_in && xsflib::rip::Audible(model->renderer.DeviceOutput()))
        {
            probe.sounded = true;
            break;
        }

        if (runner.Apply(engine, frame))
        {
            break;
        }
    }

    probe.drew = engine.Rng().State() != frames_only.State();

    return probe;
}

// `script` with its start delayed by `frames`: a longer lead-in, and each step that many frames later.
ScriptedStart Delayed(ScriptedStart script, uint32_t frames)
{
    script.lead_in += frames;
    for (ScriptStep& step : script.steps)
    {
        step.frame += frames;
    }

    return script;
}

} // namespace

struct LengthAnalyzer::Impl
{
    fsar::SoundArchive archive;
    ArchiveDescriptor settings;
};

LengthAnalyzer::LengthAnalyzer(fsar::SoundArchive archive, ArchiveDescriptor settings)
    : impl_(new Impl{std::move(archive), std::move(settings)})
{
}

LengthAnalyzer::~LengthAnalyzer() = default;

std::optional<uint32_t> LengthAnalyzer::FramesUntil(const ScriptedStart& script, int variable, int16_t value,
                                                    uint32_t limit,
                                                    std::span<const fsar::SoundArchive* const> addons) const
{
    if (variable < ScriptStep::kLocalEnd || variable >= ScriptStep::kGlobalEnd)
    {
        return std::nullopt;
    }

    const std::unique_ptr<atk::ArchiveModel> model = MakeModel(impl_->archive, impl_->settings, addons);
    atk::Engine& engine = *model->engine;
    ScriptRunner runner(script, ntdlib::rip::kFullTrackVolumes, impl_->settings.procedures);
    if (runner.Apply(engine, 0))
    {
        return std::nullopt;
    }

    const auto holds = [&engine, variable, value]
    {
        return engine.GlobalVariables()[static_cast<std::size_t>(variable - ScriptStep::kLocalEnd)] == value;
    };
    for (uint32_t frame = 1; frame <= limit; ++frame)
    {
        engine.RunFrame();
        if (holds())
        {
            return frame;
        }

        if (runner.Apply(engine, frame))
        {
            return std::nullopt;
        }
    }

    return std::nullopt;
}

std::optional<uint32_t> LengthAnalyzer::SoundingDelay(const ScriptedStart& script,
                                                      const ntdlib::rip::TrackVolumes& track_volumes, uint32_t limit,
                                                      uint32_t window, const ArchiveDescriptor* settings) const
{
    const ArchiveDescriptor& used = settings ? *settings : impl_->settings;
    const auto probe = [this, &used, &track_volumes, window](const ScriptedStart& start)
    {
        return ProbeStart(impl_->archive, used, track_volumes, start, window);
    };
    if (!probe(script).drew)
    {
        return std::nullopt;
    }

    for (uint32_t delay = 1; delay <= limit; ++delay)
    {
        if (probe(Delayed(script, delay)).sounded)
        {
            return delay;
        }
    }

    return std::nullopt;
}

std::optional<long long> LengthAnalyzer::FirstSampleMs(const ScriptedStart& script,
                                                       const ntdlib::rip::TrackVolumes& track_volumes, uint32_t limit,
                                                       const ArchiveDescriptor* settings) const
{
    const ArchiveDescriptor& used = settings ? *settings : impl_->settings;
    const std::unique_ptr<atk::ArchiveModel> model = MakeModel(impl_->archive, used);
    atk::Engine& engine = *model->engine;
    ScriptRunner runner(script, track_volumes, used.procedures);
    if (!RunLeadIn(engine, runner, script.lead_in))
    {
        return std::nullopt;
    }

    // The frames after the lead-in, each followed by the steps of the next, as the length analysis runs them.
    for (uint32_t frame = 0; frame < limit; ++frame)
    {
        engine.RunFrame();
        if (const std::optional<double> offset = DeviceFirstSampleMs(model->renderer.DeviceOutput()))
        {
            return static_cast<long long>(static_cast<double>(frame) * atk::LengthConsole::kFrameMs + *offset + 0.5);
        }

        if (runner.Apply(engine, uint64_t{script.lead_in} + frame + 1))
        {
            return std::nullopt;
        }
    }

    return std::nullopt;
}

std::optional<Timing> LengthAnalyzer::Analyze(uint32_t sound_index, const ntdlib::rip::TrackVolumes& track_volumes,
                                              const ScriptedStart* script, const ArchiveDescriptor* settings,
                                              float volume, xsflib::rip::Limit limit) const
{
    const ScriptedStart start = script ? *script : ScriptedStart::Plain(fsar::ItemId(fsar::kSoundItem, sound_index));
    const auto measure = [this, &track_volumes, &start, settings, volume, limit]
    {
        return MeasureSequence(impl_->archive, settings ? *settings : impl_->settings, track_volumes, start, volume,
                               limit);
    };
    return xsflib::rip::TryMeasure(measure);
}

} // namespace hsf::rip
