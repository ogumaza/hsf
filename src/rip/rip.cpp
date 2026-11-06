// SPDX-License-Identifier: MIT

#include "rip/rip.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ntdlib/nwsnd/length_model.h"
#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/nx/atk/low_level_voice.h"
#include "ntdlib/nx/atk/sequence.h"
#include "xsflib/rip/length.h"

#include "hsf/format.h"
#include "hsf/jaudio.h"
#include "hsf/model.h"
#include "hsf/nx.h"
#include "hsf/sar.h"
#include "jaudio/engine.h"
#include "jaudio/output.h"

namespace hsf::rip
{
namespace
{

using ntdlib::nwsnd::kTrackCount;

// A frame of the audio renderer and nn::atk.
constexpr double kFrameMs = 5.0;

// The length analysis's rules in HSF's frames (xsflib::rip::LengthRules): at most 600 s, and a sequence that stays busy
// without a sound for 180 s has ended. The others are the defaults: a frame's commands sound from the next frame on,
// and a sequence that ends ends at its first idle frame when that comes before its last sound's frame ends.
xsflib::rip::LengthRules HsfLengthRules()
{
    xsflib::rip::LengthRules rules;
    rules.frame_ms = kFrameMs;
    rules.max_frames = 120000;
    rules.silent_frames = 36000;

    return rules;
}

// ntdlib's model for the length analysis (xsflib::rip::MeasureLength): the engine and the renderer of an ArchiveModel,
// and the sound's player. A frame's output is what the renderer rendered in it. EffectTail (hsf/model.h) says how long
// the effects ring. The model also counts each track's passes. A looping sequence's fade then waits for the tracks that
// loop at other lengths than the main track (OthersLooped). A track that another track opens again makes a pass each
// time. The fade therefore doesn't wait for the tracks that a looping track opens again in each of its loops.
class LengthModel
{
public:
    // The model has run `frame` frames, and `script` has taken the steps of those frames and of the next. The model
    // takes the steps of each later frame after the frame before it. The output that the analysis judges is scaled by
    // `volume`, as a player scales it by the mini's volume tag.
    LengthModel(atk::ArchiveModel& model, atk::SequenceSoundPlayer& player, const std::vector<AuxEffect>& effects,
                ScriptRunner& script, uint64_t frame, float volume)
        : model_(model),
          engine_(*model.engine),
          player_(player),
          tail_(effects),
          script_(script),
          frame_(frame),
          volume_(volume)
    {
    }

    // A start that fails stops the measure.
    bool RunFrame()
    {
        engine_.RunFrame();
        ++frame_;
        output_ = Scaled(model_.renderer.DeviceOutput());
        tail_.Frame(output_);
        ++frames_;
        if (script_.Apply(engine_, frame_))
        {
            return false;
        }

        return true;
    }

    // The frames run so far.
    uint64_t Frames() const
    {
        return frames_;
    }

    bool Sounded() const
    {
        return xsflib::rip::Audible(output_);
    }

    // The effects ring on after the sequence (EffectTail).
    bool Ringing() const
    {
        return tail_.Ringing();
    }

    std::size_t VoiceCount()
    {
        return engine_.LowLevelVoices().All().size();
    }

    xsflib::rip::VoicePlace Voice(std::size_t i)
    {
        const atk::LowLevelVoice& voice = engine_.LowLevelVoices().All()[i];
        return {voice.IsPlaying(), voice.Loops(), voice.PlayPosition()};
    }

    bool Active() const
    {
        return player_.IsActive();
    }

    // A sequence that waits for a value from a later step of the script isn't settled or stuck.
    bool Settled() const
    {
        return player_.IsSettled(engine_) && !script_.Pending(frame_);
    }

    bool Stuck() const
    {
        return player_.IsStuck(engine_) && !script_.Pending(frame_);
    }

    bool Moving() const
    {
        return player_.IsMoving();
    }

    uint64_t ChangedAt() const
    {
        return player_.ChangedAt();
    }

    // The script's later steps keep the set going. The player does the same.
    bool Busy() const
    {
        return engine_.IsBusy() || script_.Pending(frame_);
    }

    bool ChannelsChanging() const
    {
        return engine_.Channels().AnyChanging();
    }

    int ActiveChannels() const
    {
        return engine_.Channels().ActiveCount();
    }

    // Sets the hooks for the analysis's loops, for every track's passes and for each open of a track. Only the passes
    // that changed the sound count. An open of a track that a track opened before is such a pass. An open clears the
    // track's wait, and the track starts its part again at once. The first open isn't a pass.
    template <typename Hook>
    void OnLoop(Hook hook)
    {
        engine_.SetLoopHook(std::move(hook));
        const auto count_pass = [this](int track, uint32_t)
        {
            const auto* looping = player_.Track(track);
            if (looping && !looping->IsIdle())
            {
                ++passes_[static_cast<std::size_t>(track)];
            }
        };
        engine_.SetTrackLoopHook(count_pass);
        const auto count_open = [this](int, int opened, uint32_t)
        {
            const auto index = static_cast<std::size_t>(opened);
            if (opened_[index])
            {
                ++passes_[index];
            }

            opened_[index] = true;
        };
        engine_.SetOpenTrackHook(count_open);
    }

    // Whether every open track other than `track` has made kLengthLoops passes that changed the sound. A track whose
    // last pass changed nothing, or that waits for ever, doesn't hold the fade back.
    bool OthersLooped(int track) const
    {
        for (int i = 0; i < kTrackCount; ++i)
        {
            const auto* other = player_.Track(i);
            if (i != track && other && other->IsOpen() && !other->IsIdle() && !other->WaitsForEver(engine_) &&
                passes_[static_cast<std::size_t>(i)] < xsflib::rip::kLengthLoops)
            {
                return false;
            }
        }

        return true;
    }

private:
    // A frame's output as a player plays it with the mini's volume tag.
    std::span<const int16_t> Scaled(std::span<const int16_t> output)
    {
        if (volume_ == 1.0f)
        {
            return output;
        }

        scaled_.resize(output.size());
        std::transform(output.begin(), output.end(), scaled_.begin(),
                       [this](int16_t sample)
                       {
                           const double value = std::round(static_cast<double>(sample) * static_cast<double>(volume_));
                           return static_cast<int16_t>(std::clamp(value, -32768.0, 32767.0));
                       });

        return scaled_;
    }

    atk::ArchiveModel& model_;
    atk::Engine& engine_;
    atk::SequenceSoundPlayer& player_;
    std::span<const int16_t> output_;
    EffectTail tail_;
    ScriptRunner& script_;
    uint64_t frame_;                         // the model's frames, the script's lead-in included
    uint64_t frames_ = 0;                    // the frames measured
    float volume_;                           // the mini's volume tag, which scales the output
    std::vector<int16_t> scaled_;            // the frame's output, scaled
    std::array<int, kTrackCount> passes_{};  // each track's passes that changed the sound
    std::array<bool, kTrackCount> opened_{}; // whether another track has opened each track
};

// Measures a sequence on the model, set up for `settings` (xsflib::rip::MeasureLength), with `track_volumes` for its
// tracks and its output scaled by `volume`. The sound starts by `script`, just like in the player. The measure starts
// after the lead-in. The counted loops in `endless_loops` loop for ever.
std::optional<Timing> MeasureSequence(const fsar::SoundArchive& archive, const ArchiveDescriptor& settings,
                                      const ntdlib::rip::TrackVolumes& track_volumes, const ScriptedStart& script,
                                      float volume, const ntdlib::nwsnd::CountedLoopSet& endless_loops = {})
{
    auto model = std::make_unique<atk::ArchiveModel>(archive, EngineOptionsFor(settings));
    atk::Engine& engine = *model->engine;
    AppendEffects(engine, settings.effects);
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

    LengthModel length_model(*model, *player, settings.effects, runner, script.lead_in, volume);
    const xsflib::rip::LengthRules rules = HsfLengthRules();
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
    auto model = std::make_unique<atk::ArchiveModel>(archive, EngineOptionsFor(settings));
    atk::Engine& engine = *model->engine;
    AppendEffects(engine, settings.effects);
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

// A note that a sub-frame's commands start sounds two sub-frames on: the next sub-frame writes its channel's block, and
// the one after renders it. The limits are HsfLengthRules'.
xsflib::rip::LengthRules JaudioLengthRules()
{
    xsflib::rip::LengthRules rules;
    rules.frame_ms = 2.5;
    rules.max_frames = 240000;
    rules.silent_frames = 72000;
    rules.idle_frames = 20;
    rules.loop_offset = 2;
    rules.sound_offset = 2;
    rules.steady_offset = 2;

    return rules;
}

namespace
{

// The model of JAudio for the length analysis (xsflib::rip::MeasureLength). A frame's output is what AudioOut plays of
// the sub-frame. The FX lines ring on until their delays are empty. The voices are the DSP channels. A sequence is
// settled while the sound holds its notes and each track without an open child track waits for its notes to end (the
// engine's Held and Waiting). A sequence is never stuck.
//
// A loop is a jump back of the main track, outside a subroutine. The main track is the first track, in the order of
// the sequence's tree, that has started a note and whose last pass started one. A track that plays only an intro
// therefore leaves the count to the tracks that play in every pass. If no track's last pass started a note, the main
// track is the first track that has started a note. Neither choice takes a track that waits for ever (the engine's
// WaitsForEver). A pass that started no note counts only while the sound holds its notes (the engine's Held). A sound
// effect that holds a note until the game stops it makes such passes.
class JaudioLengthModel
{
public:
    explicit JaudioLengthModel(jaudio::Engine& engine) : engine_(engine)
    {
    }

    bool RunFrame()
    {
        engine_.RunSubframe();
        output_.clear();
        out_.Push(engine_.Output(), output_);
        ++frames_;

        return true;
    }

    uint64_t Frames() const
    {
        return frames_;
    }

    bool Sounded() const
    {
        return xsflib::rip::Audible(output_);
    }

    bool Ringing() const
    {
        return std::any_of(output_.begin(), output_.end(), [](int16_t sample) { return sample != 0; }) ||
               engine_.FxRinging();
    }

    std::size_t VoiceCount()
    {
        return jaudio::Voices::kDspChannels;
    }

    xsflib::rip::VoicePlace Voice(std::size_t index)
    {
        const jaudio::Engine::WavePlace place = engine_.DspChannelPlace(static_cast<uint32_t>(index));
        return {place.playing, place.loops, place.position};
    }

    bool Active() const
    {
        return engine_.Playing();
    }

    bool Settled() const
    {
        return engine_.Held() && engine_.Waiting();
    }

    bool Stuck() const
    {
        return false;
    }

    bool Moving() const
    {
        return engine_.Moving();
    }

    uint64_t ChangedAt() const
    {
        return 0;
    }

    bool Busy() const
    {
        return engine_.Busy();
    }

    bool ChannelsChanging() const
    {
        return !engine_.Held();
    }

    int ActiveChannels() const
    {
        return engine_.ActiveChannels();
    }

    template <typename Hook>
    void OnLoop(Hook hook)
    {
        engine_.SetLoopHook(
            [this, hook](uint32_t track, uint32_t target, bool played)
            {
                if (track == MainTrack() && (played || engine_.Held()))
                {
                    hook(static_cast<int>(track), target);
                }
            });
    }

private:
    // A track that waits for ever doesn't count.
    uint32_t MainTrack() const
    {
        uint32_t first = jaudio::Track::kNone;
        for (const uint32_t track : engine_.TreeOrder())
        {
            if (!engine_.PlayedNotes(track) || engine_.WaitsForEver(track))
            {
                continue;
            }

            if (!engine_.QuietPass(track))
            {
                return track;
            }

            if (first == jaudio::Track::kNone)
            {
                first = track;
            }
        }

        return first;
    }

    jaudio::Engine& engine_;
    jaudio::AudioOut out_;
    std::vector<int16_t> output_;
    uint64_t frames_ = 0;
};

} // namespace

bool IsWiiUArchive(std::span<const uint8_t> bytes)
{
    // The magic and a big-endian byte order mark, FE FF.
    constexpr std::array<uint8_t, 6> kHead = {'F', 'S', 'A', 'R', 0xFE, 0xFF};
    return bytes.size() >= kHead.size() && std::equal(kHead.begin(), kHead.end(), bytes.begin());
}

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

    auto model = std::make_unique<atk::ArchiveModel>(impl_->archive, EngineOptionsFor(impl_->settings));
    atk::Engine& engine = *model->engine;
    for (const fsar::SoundArchive* addon : addons)
    {
        engine.AddArchive(*addon);
    }

    AppendEffects(engine, impl_->settings.effects);
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

JaudioLengthAnalyzer::JaudioLengthAnalyzer(const jaudio::SoundFiles& files, ArchiveDescriptor settings)
    : files_(files), settings_(std::move(settings))
{
}

JaudioTiming JaudioLengthAnalyzer::Analyze(uint32_t sound) const
{
    JaudioTiming result;
    const auto measure = [this, sound, &result]() -> std::optional<Timing>
    {
        jaudio::Engine engine(files_, JaudioEngineSettings(settings_.jaudio));
        if (engine.Start(sound, TrackStatesFor(settings_.jaudio, sound)))
        {
            return std::nullopt;
        }

        JaudioLengthModel model(engine);
        const std::optional<Timing> timing = xsflib::rip::MeasureLength(model, JaudioLengthRules());
        result.missing_bank_notes = engine.MissingBankNotes();

        return timing;
    };
    result.timing = xsflib::rip::TryMeasure(measure);

    return result;
}

} // namespace hsf::rip
