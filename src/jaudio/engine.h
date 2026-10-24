// SPDX-License-Identifier: MIT

// JAudio2's sound system as the Super Mario Galaxy games' `main` builds it, on Hagi: a sound of the start-up archive's
// sound table, played on the models of JAudio2's sequences and channels (track.h, channel.h) and of Hagi's emulation of
// the DSP program (dsp.h). The game's code above JAudio2 sets the DSP's level, the FX lines and the volumes of its
// sound managers. Its BGM manager can also set the tracks of a piece of music as the piece starts. Settings and
// TrackStates hold what it sets.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/jaudio/archive.h"
#include "ntdlib/jaudio/rarc.h"

#include "jaudio/channel.h"
#include "jaudio/dsp.h"
#include "jaudio/instruments.h"
#include "jaudio/track.h"

namespace hsf::jaudio
{

// An FX line's settings as the game hands them to JASDsp::setFXLine (its FxlineConfig_), with the buses' numbers looked
// up in JASDsp's table of buses.
struct FxLineSettings
{
    // The longest delay: the games give each line a buffer of 0x5000 bytes.
    static constexpr uint16_t kMaxSubframes = 128;

    uint16_t flags = 0;     // 0 for an FX line that's off. Bit 0 filters the delayed sends, bit 1 the line's output
    uint16_t subframes = 0; // the delay's length in sub-frames of 80 samples, at most kMaxSubframes
    std::array<uint16_t, 2> buses{};  // the DSP's addresses of the two buses that the line feeds
    std::array<uint16_t, 2> levels{}; // the line's level into each, 0x8000 for 1
    std::array<int16_t, 8> filter{};  // the filter's 8 coefficients

    bool operator==(const FxLineSettings&) const = default;
};

// What the game's code sets up for its sounds: the DSP's level, the FX lines, the volumes of its sound managers, and
// the volumes that it writes into the sound table. The level is the value from JASDriver::setDSPLevel that
// DsyncFrame2ch passes, in 4.12 for 4 times the level. A sequence sound of section 1 plays at the sequence manager's
// volume. An effect, a sound of section 0, plays at the effect manager's volume times its category's. The category is
// the sound's group. A sound in `sound_volumes` plays at that volume in place of the table's byte.
struct Settings
{
    static constexpr std::size_t kCategories = 16;

    uint16_t dsp_level = 0x4000;
    std::array<FxLineSettings, 4> fx_lines{};
    float music_volume = 1.0f;
    float effect_volume = 1.0f;
    std::array<float, kCategories> category_volumes{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                                    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    std::map<uint32_t, uint8_t> sound_volumes; // by the sound's ID

    bool operator==(const Settings&) const = default;
};

// The states that the game's BGM manager gives the tracks of a piece of music as the piece starts, one for each of
// the sequence's first 16 child tracks, in the form of the game's table. Bits 0 to 2 give the state: 0 fades the
// track out, 1 fades it in, 2 mutes it and 3 unmutes it. Other values change nothing. Bit 3 mutes the track while its
// volume is 0. The piece's sound reserves these tracks as it starts, and its sequence opens them in place. A mute
// therefore lasts from the start. A fade at the start takes no time.
using TrackStates = std::vector<uint8_t>;

// The sound files of a game: the start-up archive (BAA), the sequence archive (an RARC of BMS files), and the wave
// archives (AW) that the wave systems name. The waves of the archives that the set holds go into the DSP's memory.
class SoundFiles
{
public:
    // Where the waves start in the DSP's memory. The channels' blocks, the FX lines' settings and buffers and the
    // outputs come before them.
    static constexpr uint32_t kWaves = 0x1c000;

    // A wave archive's bytes by its name in the wave systems, or null for an archive that the set lacks.
    using WaveArchives = std::function<const std::vector<uint8_t>*(const std::string& name)>;

    // The start-up archive and the sequence archive may each be compressed with Yaz0. Throws xsflib::FormatError for a
    // file that can't be read.
    SoundFiles(std::span<const uint8_t> startup, std::span<const uint8_t> sequences, const WaveArchives& waves);

    SoundFiles(const SoundFiles&) = delete;
    SoundFiles& operator=(const SoundFiles&) = delete;

    const ntdlib::jaudio::SoundArchive& Archive() const
    {
        return *archive_;
    }

    // The sound table's entry with ID `id`, or null.
    const ntdlib::jaudio::SoundEntry* Find(uint32_t id) const;

    // AudSoundInfo::isRhythmBgm (0x1fbd34): whether a piece of music gives the game's rhythm system its beats. The
    // sound plays a file of the sequence archive (types 0x60 to 0x6f), and bit 0 of the 16-bit field at +6 of its
    // entry is set.
    bool RhythmPiece(const ntdlib::jaudio::SoundEntry& sound) const;

    // The sequence that a sound plays: the sequence archive's file, decoded, or the start-up archive's sequence
    // collection with the sequence's offset in it. Null for a sound that plays no sequence that the files hold.
    struct SequenceRef
    {
        const Sequence* sequence = nullptr;
        uint32_t offset = 0;
    };
    SequenceRef SequenceOf(const ntdlib::jaudio::SoundEntry& sound) const;

    // The banks, in the order of the start-up archive's commands. Each plays its wave system's wave bank.
    const std::vector<std::unique_ptr<InstrumentBank>>& Banks() const
    {
        return banks_;
    }

    // The DSP's memory as each sound starts: the waves from kWaves, zeros below.
    const DspMemory& Memory() const
    {
        return *memory_;
    }

    // The names of the wave archives that the wave systems name and the set lacks.
    const std::vector<std::string>& MissingWaves() const
    {
        return missing_waves_;
    }

private:
    void LoadWaves(const WaveArchives& waves);
    void LoadBanks();

    std::vector<uint8_t> baa_;
    std::unique_ptr<ntdlib::jaudio::SoundArchive> archive_;
    std::unique_ptr<Sequence> collection_;
    std::map<uint32_t, std::unique_ptr<Sequence>> files_; // the sequence archive's files by id, decoded
    std::unique_ptr<DspMemory> memory_;
    std::map<uint32_t, WaveBank> wave_banks_; // by wave system
    std::vector<std::unique_ptr<InstrumentBank>> banks_;
    std::vector<std::string> missing_waves_;
};

// A sound played on the model, sub-frame by sub-frame. A frame of the DSP is 7 sub-frames.
class Engine
{
public:
    static constexpr int kSubframesPerFrame = 7;

    // The DSP's rate as JAudio2's start-up sets it (initAI): 32,000 Hz times 1.0008897. The oscillators' and the
    // channels' pitches follow it. Hagi's audio interface plays the DSP's output at 32,000 Hz.
    static constexpr float kDacRate = 32028.46875f;

    // A loop of a track: its index, the command that its jump goes to, and whether the track started a note since
    // its last such jump or since it opened.
    using LoopHook = std::function<void(uint32_t track, uint32_t target, bool played)>;

    Engine(const SoundFiles& files, const Settings& settings);
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    ~Engine();

    // Starts sound `id` from the beginning, with `states` for the tracks of a piece of music. Returns an error for a
    // sound that plays no sequence that the files hold.
    std::optional<std::string> Start(uint32_t id, const TrackStates& states = {});

    // A sub-frame: at a frame's start, the levels that the game's code sets each frame and DsyncFrame2ch. Then the
    // DSP's render, the DSP channels' update, the sequences' ticks and the free-running LFO's step.
    void RunSubframe();

    // The sub-frame's output: 80 stereo pairs, left then right, at the DSP's rate.
    std::span<const int16_t> Output() const
    {
        return output_;
    }

    // Whether the sound's sequence still plays.
    bool Playing() const;

    // Whether the sequence plays, a DSP channel sounds, or the FX lines' delays still hold sound.
    bool Busy() const;

    // The DSP channels that play.
    int ActiveChannels() const;

    // Whether the FX lines' delays hold sound that they haven't played yet.
    bool FxRinging() const;

    // The notes that asked for a bank that the files don't load. The game's code reads through a null pointer at the
    // first such note. The model leaves the note out.
    uint32_t MissingBankNotes() const;

    // Sets the hook for the tracks' jumps back. A jump inside a subroutine or a counted loop doesn't call it.
    void SetLoopHook(LoopHook hook);

    // The sound's tracks in the order of the sequence's tree: the root, then each child after its parent, by the
    // child's slot. Tracks that have closed aren't in it.
    std::vector<uint32_t> TreeOrder() const;

    // Whether the track has started a note since the sound started, and whether its last pass, up to its last jump
    // back, started none.
    bool PlayedNotes(uint32_t track) const;
    bool QuietPass(uint32_t track) const;

    // The length analysis's tests of a held sound. Held: each channel that plays holds its note at the sustain level
    // of its envelopes, on a looping wave, and each open track has started a note, jumped back or opened a track that
    // is still open. Moving: a track's levels move, or a channel's key sweeps. WaitsForEver: the track waits for its
    // notes to end, and each of them is held. Waiting: each open track that has opened no track still open waits for
    // its notes to end.
    bool Held() const;
    bool Moving() const;
    bool WaitsForNotes(uint32_t track) const;
    bool WaitsForEver(uint32_t track) const;
    bool Waiting() const;

    // A DSP channel's place in its wave, for the length analysis: whether it plays a wave, whether the wave loops, and
    // a position that drops each time the wave passes back to its loop's start.
    struct WavePlace
    {
        bool playing = false;
        bool loops = false;
        int64_t position = 0;
    };

    WavePlace DspChannelPlace(uint32_t index) const;

private:
    struct Model;

    // JAISeq's mix-out at a frame's start: the sound's levels into the root track, and the levels that the BGM
    // manager's track controllers give the child tracks.
    void MixOut();

    const SoundFiles& files_;
    Settings settings_;
    std::unique_ptr<DspMemory> memory_;
    std::unique_ptr<Model> model_;
    std::vector<int16_t> output_;
    LoopHook loop_hook_;
};

} // namespace hsf::jaudio
