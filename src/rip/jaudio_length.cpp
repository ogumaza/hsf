// SPDX-License-Identifier: MIT

#include "rip/jaudio_length.h"

#include <cstdint>
#include <optional>
#include <utility>

#include "ntdlib/jaudio2/engine.h"
#include "ntdlib/nwsnd/length_model.h"
#include "xsflib/rip/length.h"

#include "hsf/jaudio.h"
#include "hsf/jaudio_models.h"

namespace hsf::rip
{

// A note that a sub-frame's commands start sounds two sub-frames on: the next sub-frame writes its channel's block, and
// the one after renders it. The limits are those of ntdlib's length analysis (ntdlib::nwsnd::LengthRulesFor).
xsflib::rip::LengthRules JaudioLengthRules()
{
    xsflib::rip::LengthRules rules = ntdlib::nwsnd::LengthRulesFor(2.5);
    rules.idle_frames = 20;
    rules.loop_offset = 2;
    rules.sound_offset = 2;
    rules.steady_offset = 2;

    return rules;
}

namespace
{

// The model of JAudio for the length analysis (xsflib::rip::MeasureLength). A frame's output is what AudioOut plays of
// the sub-frame. The FX lines ring on until their delays are empty. A loop is a jump back of the main track
// (JaudioModelBase::MainTrack), outside a subroutine. A pass that started no note counts only while the sound holds its
// notes (the engine's Held). A sound effect that holds a note until the game stops it makes such passes.
class JaudioLengthModel : public JaudioModelBase<jaudio::Engine, jaudio::Voices::kDspChannels, jaudio::Track::kNone>
{
public:
    explicit JaudioLengthModel(jaudio::Engine& engine) : JaudioModelBase(engine)
    {
    }

    bool RunFrame()
    {
        engine_.RunSubframe();
        TakeOutput();

        return true;
    }

    bool Ringing() const
    {
        return AnySample() || engine_.FxRinging();
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
};

} // namespace

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
