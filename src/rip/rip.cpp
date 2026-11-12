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
    atk::ArchiveModel& model;
    atk::Engine* engine;
    ScriptRunner& script;
    EffectTail tail;
    uint64_t frame;                  // the model's frames, the script's lead-in included
    float volume;                    // the mini's volume tag, which scales the output
    std::span<const int16_t> output; // the frame's output, scaled
    std::vector<int16_t> scaled;     // the frame's output, scaled, when the volume isn't 1
};

// The console of the length analysis (ntdlib::nwsnd::LengthModel): the console of the Switch (atk::LengthConsole), with
// the script, the tail and the volume. The model counts each track's passes. A looping sequence's fade then waits for
// the tracks that loop at other lengths than the main track (OthersLooped). A track that another track opens again
// makes a pass each time. The open clears the track's wait, and the track starts its part again at once. The fade
// therefore doesn't wait for the tracks that a looping track opens again in each of its loops.
struct ScriptedConsole
{
    using Model = ScriptedModel;

    // The model has run `frame` frames, and the script has taken the steps of those frames and of the next. The script
    // takes the steps of each later frame after the frame before it. A start that fails stops the measure.
    static bool RunFrame(Model& m)
    {
        m.engine->RunFrame();
        ++m.frame;
        m.output = Scaled(m, m.model.renderer.DeviceOutput());
        m.tail.Frame(m.output);

        return !m.script.Apply(*m.engine, m.frame);
    }

    static std::span<const int16_t> Output(const Model& m)
    {
        return m.output;
    }

    // The effects ring on after the sequence (EffectTail).
    static bool Ringing(const Model& m)
    {
        return m.tail.Ringing();
    }

    static std::size_t VoiceCount(const Model& m)
    {
        return atk::LengthConsole::VoiceCount(m.model);
    }

    static xsflib::rip::VoicePlace Voice(const Model& m, std::size_t index)
    {
        return atk::LengthConsole::Voice(m.model, index);
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

// Measures a sequence on the model, set up for `settings` (xsflib::rip::MeasureLength), with `track_volumes` for its
// tracks and its output scaled by `volume`. The sound starts by `script`, just like in the player. The measure starts
// after the lead-in. The counted loops in `endless_loops` loop for ever.
std::optional<Timing> MeasureSequence(const fsar::SoundArchive& archive, const ArchiveDescriptor& settings,
                                      const ntdlib::rip::TrackVolumes& track_volumes, const ScriptedStart& script,
                                      float volume, const ntdlib::nwsnd::CountedLoopSet& endless_loops = {})
{
    const std::unique_ptr<atk::ArchiveModel> model = MakeModel(archive, settings);
    atk::Engine& engine = *model->engine;
    engine.SetEndlessLoops(endless_loops);
    ScriptRunner runner(script, track_volumes, settings.procedures);
    if (runner.Apply(engine, 0))
    {
        return std::nullopt;
    }

    for (uint64_t frame = 1; frame <= script.lead_in; ++frame)
    {
        engine.RunFrame();
        if (runner.Apply(engine, frame))
        {
            return std::nullopt;
        }
    }

    atk::SequenceSoundPlayer* player = runner.Sound();
    if (!player)
    {
        return std::nullopt;
    }

    ScriptedModel scripted{
        *model, model->engine.get(), runner, EffectTail(settings.effects), script.lead_in, volume, {}, {}};
    ntdlib::nwsnd::LengthModel<ScriptedConsole> length_model(scripted, *player);
    const xsflib::rip::LengthRules rules = ntdlib::nwsnd::LengthRulesFor(atk::LengthConsole::kFrameMs);
    const std::optional<Timing> timing = xsflib::rip::MeasureLength(length_model, rules);

    // A sequence can repeat its music in a counted loop that the game plays for longer than the limit. When the measure
    // reaches the limit inside counted loops, it runs again with each track's outermost counted loop endless, by the
    // rule of ntdlib's length analysis. Their passes then count as loops of the sequence. An inner loop that runs at
    // the limit, such as a fill, still ends, and repeats in each pass of the outer loop as the game plays it.
    if (timing && endless_loops.empty() && length_model.Frames() >= rules.max_frames)
    {
        if (const auto running = ntdlib::nwsnd::OutermostCountedLoops(*player); !running.empty())
        {
            return MeasureSequence(archive, settings, track_volumes, script, volume, running);
        }
    }

    return timing;
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

std::optional<Timing> LengthAnalyzer::Analyze(uint32_t sound_index, const ntdlib::rip::TrackVolumes& track_volumes,
                                              const ScriptedStart* script, const ArchiveDescriptor* settings,
                                              float volume) const
{
    const ScriptedStart start = script ? *script : ScriptedStart::Plain(fsar::ItemId(fsar::kSoundItem, sound_index));
    const auto measure = [this, &track_volumes, &start, settings, volume]
    {
        return MeasureSequence(impl_->archive, settings ? *settings : impl_->settings, track_volumes, start, volume);
    };
    return xsflib::rip::TryMeasure(measure);
}

} // namespace hsf::rip
