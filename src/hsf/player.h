// SPDX-License-Identifier: MIT

// HSF player core: loads an .hsflib and .minihsf set and produces its audio, with HSF's model of the SDK's sound
// library (nn::atk) on its model of the console's audio renderer. Portable C++ with no platform audio or UI. The CLI
// renderer and the foobar2000 component use it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hsf/format.h"

namespace hsf
{

namespace fsar
{

class SoundArchive;

} // namespace fsar

class Player
{
public:
    enum class State
    {
        kIdle,
        kPlaying,
        kFinished,
        kError
    };

    // The rate players report: 48,000 Hz.
    static constexpr int kSampleRate = kPlayerSampleRate;

    Player();
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // Loads a file and its libraries, without starting.
    bool Load(const std::string& path, const FileReader& reader);

    // Loads an already assembled set.
    bool Load(LoadedSet set);

    // Starts the sound on the model, with the set's effects on the aux buses. Can be called again to restart from the
    // beginning.
    bool Start();

    // Renders up to `frames` stereo frames (interleaved L, R) into `out`. Returns the number of frames written. That's
    // less than requested only after an error.
    std::size_t Render(int16_t* out, std::size_t frames);

    State GetState() const
    {
        return state_;
    }

    // The frame count at the end of the renderer's frame in which the sound was first found to have finished (the
    // sequence has ended, no channel sounds, and the frame's output was silent, so that the effects have died away
    // too), whatever block size Render is called with. Only meaningful in State::kFinished.
    uint64_t FinishedFrame() const
    {
        return finished_frame_;
    }

    const std::string& Error() const
    {
        return error_;
    }

    const Tags& GetTags() const
    {
        return set_.tags;
    }

    // Tagged length and fade in milliseconds (-1 if absent).
    long long LengthMs() const;
    long long FadeMs() const;

    // Tagged volume (linear gain, 1.0 if absent).
    double Volume() const;

    // The rip's files that the player loaded. The first Start moves the bytes of the sound archive and the group files
    // out of the set and into the player's archive. A large archive is then held once. Their chunks are empty after
    // that.
    const LoadedSet& GetLoadedSet() const
    {
        return set_;
    }

private:
    // The model playing the sound.
    class Source;

    bool Fail(const std::string& message);
    bool StartImpl();

    // Reads the sound archive and adds its group files, at the first start.
    std::optional<std::string> ReadArchive();

    // Runs one frame of the sound. Returns false after an error.
    bool Step();

    LoadedSet set_;
    std::unique_ptr<fsar::SoundArchive> archive_; // kept for restarts, and read by source_'s model
    std::unique_ptr<Source> source_;
    std::vector<int16_t> output_; // samples produced and not dropped yet
    State state_ = State::kIdle;
    std::string error_;
    std::size_t consumed_ = 0;    // samples of output_ already handed out (two per frame)
    uint64_t dropped_ = 0;        // samples dropped from the front of output_ since the sound started
    uint64_t finished_frame_ = 0; // see FinishedFrame
};

} // namespace hsf
