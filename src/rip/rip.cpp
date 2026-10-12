// SPDX-License-Identifier: MIT

#include "rip/rip.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "xsflib/rip/length.h"

#include "atk/effect.h"
#include "atk/engine.h"
#include "atk/low_level_voice.h"
#include "audio/i3dl2.h"
#include "hsf/sar.h"

namespace hsf::rip
{
namespace
{

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

// HSF's model for the length analysis (xsflib::rip::MeasureLength): the engine and the renderer of an ArchiveModel, and
// the sound's player. A frame's output is what the renderer rendered in it.
class LengthModel
{
public:
    LengthModel(atk::ArchiveModel& model, atk::SequenceSoundPlayer& player)
        : model_(model), engine_(*model.engine), player_(player)
    {
    }

    bool RunFrame()
    {
        engine_.RunFrame();
        output_ = model_.renderer.DeviceOutput();
        ++frames_;

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

    // The effects ring on after the sequence, until their output is silence.
    bool Ringing() const
    {
        return std::any_of(output_.begin(), output_.end(), [](int16_t sample) { return sample != 0; });
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

    bool Settled() const
    {
        return player_.IsSettled(engine_);
    }

    bool Stuck() const
    {
        return player_.IsStuck(engine_);
    }

    bool Moving() const
    {
        return player_.IsMoving();
    }

    uint64_t ChangedAt() const
    {
        return player_.ChangedAt();
    }

    bool Busy() const
    {
        return engine_.IsBusy();
    }

    bool ChannelsChanging() const
    {
        return engine_.Channels().AnyChanging();
    }

    int ActiveChannels() const
    {
        return engine_.Channels().ActiveCount();
    }

    template <typename Hook>
    void OnLoop(Hook hook)
    {
        engine_.SetLoopHook(std::move(hook));
    }

private:
    atk::ArchiveModel& model_;
    atk::Engine& engine_;
    atk::SequenceSoundPlayer& player_;
    std::span<const int16_t> output_;
    uint64_t frames_ = 0;
};

// A sequence player's tracks.
constexpr int kTracks = 16;

// The counted loops that the player's open tracks are in, each a track and its loop's body.
std::set<std::pair<int, uint32_t>> CountedLoops(const atk::SequenceSoundPlayer& player)
{
    std::set<std::pair<int, uint32_t>> loops;
    for (int i = 0; i < kTracks; ++i)
    {
        const auto* track = player.Track(i);
        if (!track || !track->IsOpen())
        {
            continue;
        }

        for (const auto& entry : track->CallStack())
        {
            if (entry.is_loop && entry.loop_count != 0)
            {
                loops.insert({i, entry.address});
            }
        }
    }

    return loops;
}

// Measures a sequence on the model, with `effects` on the aux buses (xsflib::rip::MeasureLength). The counted loops in
// `endless_loops` loop for ever.
std::optional<Timing> MeasureSequence(const fsar::SoundArchive& archive, const atk::EngineOptions& options,
                                      const std::vector<AuxEffect>& effects, uint32_t sound_index,
                                      const std::set<std::pair<int, uint32_t>>& endless_loops = {})
{
    auto model = std::make_unique<atk::ArchiveModel>(archive, options);
    atk::Engine& engine = *model->engine;
    for (const AuxEffect& effect : effects)
    {
        const std::vector<float>& v = effect.parameters;
        const audio::I3dl2ReverbParameterSet parameters{v[0], v[1], v[2], v[3], v[4],  v[5],
                                                        v[6], v[7], v[8], v[9], v[10], v[11]};
        engine.AppendEffect(static_cast<int>(effect.bus), std::make_unique<atk::I3dl2AuxEffect>(parameters));
    }
    engine.SetEndlessLoops(endless_loops);
    atk::SequenceSoundPlayer* player = engine.StartSequence(sound_index);
    if (!player)
    {
        return std::nullopt;
    }

    LengthModel length_model(*model, *player);
    const xsflib::rip::LengthRules rules = HsfLengthRules();
    const std::optional<Timing> timing = xsflib::rip::MeasureLength(length_model, rules);

    // A sequence can repeat its music in a counted loop that the game plays for longer than the limit. When the measure
    // reaches the limit inside counted loops, it runs again with those loops endless. Their passes then count as loops
    // of the sequence.
    if (timing && endless_loops.empty() && length_model.Frames() >= rules.max_frames)
    {
        const std::set<std::pair<int, uint32_t>> running = CountedLoops(*player);
        if (!running.empty())
        {
            return MeasureSequence(archive, options, effects, sound_index, running);
        }
    }

    return timing;
}

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
    atk::EngineOptions options;
    std::vector<AuxEffect> effects;
};

LengthAnalyzer::LengthAnalyzer(fsar::SoundArchive archive, atk::EngineOptions options, std::vector<AuxEffect> effects)
    : impl_(new Impl{std::move(archive), options, std::move(effects)})
{
}

LengthAnalyzer::~LengthAnalyzer() = default;

std::optional<Timing> LengthAnalyzer::Analyze(uint32_t sound_index) const
{
    return xsflib::rip::TryMeasure(
        [this, sound_index] { return MeasureSequence(impl_->archive, impl_->options, impl_->effects, sound_index); });
}

} // namespace hsf::rip
