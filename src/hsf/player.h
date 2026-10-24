// SPDX-License-Identifier: MIT

// HSF player core: loads an .hsflib and .minihsf set and produces its audio. A set of version 1 plays on ntdlib's model
// of the SDK's sound library (nn::atk) and of the console's audio renderer. A set of version 2 plays on HSF's model of
// JAudio. Portable C++ with no platform audio or UI. The CLI renderer, the foobar2000 component and the XMPlay plugin
// use it.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/player/archive_source.h"
#include "xsflib/player/playback.h"
#include "xsflib/player/player.h"

#include "hsf/format.h"

namespace hsf
{

// The player. Loading, starting, rendering, the state and the tags' length, fade and volume come from xsflib's
// PlayerBase, and archive mode's steps from its ArchiveSource. Start starts the sound on the model, with the set's
// effects on the aux buses. It can be called again to restart from the beginning. The first Start moves the bytes of
// the sound archive and the group files out of the set and into the source's archive. A large archive is then held
// once. The sound has finished once the sequence has ended, no channel sounds and a frame's output was silent. The
// effects have then died away too. FinishedFrame gives the frame count at the end of that frame. The player takes
// snapshots of the nn::atk model. xsflib's Playback seeks with them. It takes none of the JAudio model: Playback then
// seeks by playing from the start.
class Player : public xsflib::player::PlayerBase<Player, LoadedSet>
{
public:
    // The source's part of a snapshot: the model's state between frames. A snapshot can only be restored to the player
    // that took it, before its next Start, with the audio that the player had rendered but not returned yet.
    struct SourceSnapshot;
    using Snapshot = xsflib::player::Snapshot<SourceSnapshot>;

    // The rate players report: 48,000 Hz.
    static constexpr int kSampleRate = kPlayerSampleRate;

    Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;
    ~Player();

private:
    friend class xsflib::player::PlayerBase<Player, LoadedSet>;

    // Archive mode's steps on ntdlib's model (xsflib::player::ArchiveSource), and on the model of JAudio.
    struct SourceTraits;
    using Source = xsflib::player::ArchiveSource<SourceTraits>;
    struct JaudioTraits;
    using JaudioSource = xsflib::player::ArchiveSource<JaudioTraits>;

    static constexpr const char* kNotPlayable = "no archive descriptor";
    static constexpr const char* kStartFailure = "the sound couldn't start: ";
    static constexpr const char* kStepFailure = "the model failed: ";

    static std::optional<std::string> LoadFile(const std::string& path, const FileReader& reader, LoadedSet& set)
    {
        return LoadSet(path, reader, set);
    }

    // Drops the sources, with the archives that they keep for restarts.
    void Unload();

    // Whether the set plays on the model of JAudio: its descriptor has version 2.
    bool Jaudio() const;

    bool Playable() const
    {
        return set_.archive.has_value();
    }

    bool StartSource();
    bool RunStep();
    bool Finished() const;

    std::vector<int16_t>& Output()
    {
        return output_;
    }

    std::shared_ptr<const SourceSnapshot> SaveSource(const SourceSnapshot* previous);
    bool CanRestore(const SourceSnapshot* source) const;
    void RestoreSource(const SourceSnapshot& source);

    std::unique_ptr<Source> source_;       // the model, and the archive that it keeps for restarts
    std::unique_ptr<JaudioSource> jaudio_; // the same for a set of version 2
    std::vector<int16_t> output_;          // samples produced and not dropped yet
};

// Playback with length, fade, volume and seeking, and its options (xsflib::player).
using Playback = xsflib::player::Playback<Player>;
using PlaybackOptions = xsflib::player::PlaybackOptions<Player>;

} // namespace hsf
