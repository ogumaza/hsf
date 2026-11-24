// SPDX-License-Identifier: MIT

// The length analysis of a JAudio sound, on the model of JAudio2 (hsf::jaudio). The models of JAudio1 and the C JAudio
// use the rules in this file too, and its base classes: SubframeLengthModel for JAudio1, JaudioModelBase for the C
// JAudio.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/jaudio2/engine.h"
#include "ntdlib/jaudio2/output.h"
#include "xsflib/rip/length.h"

#include "hsf/format.h"
#include "hsf/jaudio_models.h"
#include "rip/rip.h"

namespace hsf::rip
{

// What the length analysis finds of a JAudio sound: its timing, and the notes that the files can't play. With libraries
// 1 and 2, those are the notes on banks that the files don't load. Such a sound's data isn't all there. With library 1,
// the game stops at the first such note. With library 3, they're the notes that get no wave from the bank file. With
// libraries 2 and 3, the game's note doesn't start. For library 2, also the numbers that the sound's sequence draws
// from the sequences' generator.
struct JaudioTiming
{
    std::optional<Timing> timing;
    uint32_t missing_bank_notes = 0;
    uint32_t sequence_draws = 0;
};

// A frame of the length analysis of JAudio: one sub-frame, 2.5 ms.
constexpr double kJaudioFrameMs = 2.5;

// The length analysis's rules for the models of JAudio: frames of one sub-frame (kJaudioFrameMs), and LengthAnalyzer's
// limits in them. With `limit` kLong, the analysis runs to xsflib's 3,600 s (xsflib::rip::WithLongLimit). The analyzers
// pick it for music, the sounds that --bgm picks. A sound at either limit gets 600 s and the fade.
xsflib::rip::LengthRules JaudioLengthRules(xsflib::rip::Limit limit = xsflib::rip::Limit::kDefault);

// The parts that the length models of JAudio2, JAudio1 and the C JAudio share (xsflib::rip::MeasureLength). `Engine`
// is the model's engine, `kVoices` the count of its DSP channels and `kNoTrack` its value for no track. The voices are
// the DSP channels. A sequence is settled while the sound holds its notes and each track without an open child track
// waits for its notes to end (the engine's Held and Waiting). A sequence is never stuck.
template <typename Engine, std::size_t kVoices, uint32_t kNoTrack>
class JaudioModelBase
{
public:
    bool Sounded() const
    {
        return xsflib::rip::Audible(output_);
    }

    std::size_t VoiceCount()
    {
        return kVoices;
    }

    xsflib::rip::VoicePlace Voice(std::size_t index)
    {
        const typename Engine::WavePlace place = engine_.DspChannelPlace(static_cast<uint32_t>(index));
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

    // The milliseconds from the start of the sub-frame just run to the first sample that isn't 0 in what AudioOut plays
    // of it, on either side, or nothing when every sample is 0 (xsflib::rip::MeasureLength). The player renders the
    // same samples at 48 kHz. AudioOut gives the first sub-frame one pair fewer than the others. The time therefore
    // comes from the pairs before the sample, and can come a pair before the sub-frame's start.
    std::optional<double> FirstSampleMs() const
    {
        const auto first = std::find_if(output_.begin(), output_.end(), [](int16_t sample) { return sample != 0; });
        if (first == output_.end())
        {
            return std::nullopt;
        }

        const uint64_t pair = pairs_before_ + static_cast<uint64_t>(first - output_.begin()) / 2;
        const double sample_ms = static_cast<double>(pair) * 1000.0 / static_cast<double>(kPlayerSampleRate);

        return sample_ms - static_cast<double>(frames_ - 1) * kJaudioFrameMs;
    }

protected:
    explicit JaudioModelBase(Engine& engine) : engine_(engine)
    {
    }

    // Takes what AudioOut plays of the frame or sub-frame that the engine has just run.
    void TakeOutput()
    {
        pairs_before_ += output_.size() / 2;
        ++frames_;
        output_.clear();
        out_.Push(engine_.Output(), output_);
    }

    // Whether the output has a sample other than 0.
    bool AnySample() const
    {
        return std::any_of(output_.begin(), output_.end(), [](int16_t sample) { return sample != 0; });
    }

    // The track whose loops count: the first track, in the order of the sequence's tree, that has started a note and
    // whose last pass started one. A track that plays only an intro therefore leaves the count to the tracks that play
    // in every pass. If no track's last pass started a note, it's the first track that has started a note. Neither
    // choice takes a track that waits for ever (the engine's WaitsForEver). kNoTrack when no track has started a note.
    uint32_t MainTrack() const
    {
        uint32_t first = kNoTrack;
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

            if (first == kNoTrack)
            {
                first = track;
            }
        }

        return first;
    }

    Engine& engine_;
    std::vector<int16_t> output_;

private:
    jaudio::AudioOut out_;
    uint64_t pairs_before_ = 0; // the pairs that AudioOut played before the current output
    uint64_t frames_ = 0;       // the sub-frames taken, the current one included
};

// The length model of the JAudio engines whose frame is one sub-frame (xsflib::rip::MeasureLength): JAudio2's and
// JAudio1's. A frame's output is what AudioOut plays of the sub-frame. The FX lines ring while the engine's FxRinging
// returns true. A loop is a jump back of the main track (JaudioModelBase::MainTrack), outside a subroutine. A pass that
// started no note counts only while the sound holds its notes (the engine's Held). A sound effect that holds a note
// until the game stops it makes such passes.
template <typename Engine, std::size_t kVoices, uint32_t kNoTrack>
class SubframeLengthModel : public JaudioModelBase<Engine, kVoices, kNoTrack>
{
public:
    explicit SubframeLengthModel(Engine& engine) : JaudioModelBase<Engine, kVoices, kNoTrack>(engine)
    {
    }

    bool RunFrame()
    {
        this->engine_.RunSubframe();
        this->TakeOutput();

        return true;
    }

    bool Ringing() const
    {
        return this->AnySample() || this->engine_.FxRinging();
    }

    template <typename Hook>
    void OnLoop(Hook hook)
    {
        this->engine_.SetLoopHook(
            [this, hook](uint32_t track, uint32_t target, bool played)
            {
                if (track == this->MainTrack() && (played || this->engine_.Held()))
                {
                    hook(static_cast<int>(track), target);
                }
            });
    }
};

// Works out how long JAudio sounds play, on the model of JAudio set up as the player sets it up for a version 2
// descriptor (hsf/jaudio.h). Analyze can be called from several threads at once.
class JaudioLengthAnalyzer
{
public:
    // `files` must outlive the analyzer.
    JaudioLengthAnalyzer(const jaudio::SoundFiles& files, ArchiveDescriptor settings);

    // As LengthAnalyzer::Analyze does, for the sound with ID `sound`. The timing is empty when the sound can't start.
    JaudioTiming Analyze(uint32_t sound) const;

private:
    const jaudio::SoundFiles& files_;
    ArchiveDescriptor settings_;
};

} // namespace hsf::rip
