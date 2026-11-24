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
#include "rip/jaudio_profiles.h"

namespace hsf::rip
{

// A note that a sub-frame's commands start sounds two sub-frames on: the next sub-frame writes its channel's block, and
// the one after renders it. The limits are those of ntdlib's length analysis (ntdlib::nwsnd::LengthRulesFor).
xsflib::rip::LengthRules JaudioLengthRules(xsflib::rip::Limit limit)
{
    xsflib::rip::LengthRules rules = ntdlib::nwsnd::LengthRulesFor(kJaudioFrameMs);
    rules.idle_frames = 20;
    rules.loop_offset = 2;
    rules.sound_offset = 2;
    rules.steady_offset = 2;

    return limit == xsflib::rip::Limit::kLong ? xsflib::rip::WithLongLimit(rules) : rules;
}

namespace
{

// The model of JAudio2 for the length analysis (SubframeLengthModel). The FX lines ring on until their delays are
// empty.
class JaudioLengthModel : public SubframeLengthModel<jaudio::Engine, jaudio::Voices::kDspChannels, jaudio::Track::kNone>
{
public:
    using SubframeLengthModel::SubframeLengthModel;
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

        // Music, the sounds of the music section, runs to the long limit.
        const bool music = sound >> 24 == kMusicSection;
        JaudioLengthModel model(engine);
        const std::optional<Timing> timing = xsflib::rip::MeasureLength(
            model, JaudioLengthRules(music ? xsflib::rip::Limit::kLong : xsflib::rip::Limit::kDefault));
        result.missing_bank_notes = engine.MissingBankNotes();

        return timing;
    };
    result.timing = xsflib::rip::TryMeasure(measure);

    return result;
}

} // namespace hsf::rip
