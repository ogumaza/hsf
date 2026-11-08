// SPDX-License-Identifier: MIT

// HSF ripper: builds an .hsflib for one of a game's sound archives and one .minihsf per sequence, played by ntdlib's
// model of the SDK's sound library and the console's audio renderer, and works out how long each sequence plays. For a
// game that plays its sound with JAudio, the same on the model of JAudio.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "ntdlib/jaudio2/engine.h"
#include "ntdlib/rip/archive.h"
#include "xsflib/rip/file_names.h"
#include "xsflib/rip/length.h"

#include "hsf/format.h"
#include "hsf/jaudio_models.h"
#include "hsf/sar.h"

namespace hsf::rip
{

// Whether `bytes` start as a Wii U sound archive: an FSAR whose byte order mark is big-endian. The readers read the
// archive, but the archive's sequences have big-endian arguments too, and nn::atk reads them little-endian. So the
// ripper refuses such an archive.
bool IsWiiUArchive(std::span<const uint8_t> bytes);

// HSF's types for the libraries and minis that a rip writes (ntdlib::rip's BuildArchiveLibrary and BuildMini).
struct RipFormat
{
    using ProgramBuilder = hsf::ProgramBuilder;
    using ArchiveDescriptor = hsf::ArchiveDescriptor;
    using FileChunk = hsf::FileChunk;

    static constexpr uint8_t kPsfVersion = kPsfVersionHsf;
};

// Filenames for the rips in one folder, valid on Windows, macOS and Linux and unique even on case-insensitive
// filesystems (xsflib::rip). HSF's are "<base>.hsflib" and "<index, four digits> <label>.minihsf".
using FileNames = xsflib::rip::FileNames;

// A sequence's length and fade in milliseconds, and whether it never makes a sound (xsflib::rip).
using Timing = ntdlib::rip::Timing;

// Works out how long sequences play by running them on ntdlib's model of nn::atk and the audio renderer (ntdlib::nx),
// set up as the player sets it up for a rip's descriptor (hsf/model.h). Analyze can be called from several threads at
// once.
class LengthAnalyzer
{
public:
    // `archive` is the sound archive, with the group files that it requires added. The model plays it with the voice
    // count and the effects of `settings`, the descriptor that the rip gets.
    explicit LengthAnalyzer(fsar::SoundArchive archive, ArchiveDescriptor settings = {});
    ~LengthAnalyzer();

    // A looping sequence gets two loops and a 10 s fade. Any other sequence runs to its end, or to where it falls
    // silent for good, its effects' tails included. No sequence runs for more than 600 s. A sequence that never makes a
    // sound is marked silent, and gets the 0.5 s that follows a sequence's last sound. The sequence's tracks play at
    // `track_volumes`. The game's code sets those volumes as it starts the sound. The sound starts by `script`
    // (docs/hsf.md, "Scripted starts"), or at once with no variables set. The script's sound has to be `sound_index`'s.
    // The length counts from the end of the script's lead-in. `settings`, when given, take the place of the analyzer's:
    // a sound with a reverb on aux bus B rings longer (rip/sound_states.h). The player scales the output by the mini's
    // volume tag. `volume` scales the output that the analysis judges in the same way. Returns nullopt when the length
    // can't be worked out (and then no length tag is written). Throws std::bad_alloc when memory runs out.
    std::optional<Timing> Analyze(uint32_t sound_index,
                                  const ntdlib::rip::TrackVolumes& track_volumes = ntdlib::rip::kFullTrackVolumes,
                                  const ScriptedStart* script = nullptr, const ArchiveDescriptor* settings = nullptr,
                                  float volume = 1.0f) const;

    // The frames that the model runs, with the sounds that `script` starts, until the global variable `variable` (16
    // to 31) holds `value`. The model plays `addons` as the script's add-on archives, in order. The steps of each frame
    // take effect as the player applies them, and the lead-in is ignored. Returns nothing when a start fails or when
    // the variable doesn't take the value within `limit` frames.
    std::optional<uint32_t> FramesUntil(const ScriptedStart& script, int variable, int16_t value, uint32_t limit,
                                        std::span<const fsar::SoundArchive* const> addons = {}) const;

    // The shortest delay of `script`, from 1 to `limit` frames, after which the script's sound makes a sound within
    // `window` frames of its lead-in's end. A delay adds its frames to the lead-in and to each step's frame. nn::atk's
    // random numbers step once a frame (atk::Engine). A sound whose sequence draws a number therefore draws another
    // one when the game starts the sound later. Returns nothing when the sound's sequence draws no number in the
    // window without a delay, or when no delay up to `limit` makes the sound sound. The sound's tracks play at
    // `track_volumes`. `settings`, when given, take the place of the analyzer's, as in Analyze.
    std::optional<uint32_t> SoundingDelay(const ScriptedStart& script, const ntdlib::rip::TrackVolumes& track_volumes,
                                          uint32_t limit, uint32_t window,
                                          const ArchiveDescriptor* settings = nullptr) const;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

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

// The length analysis's rules for the models of JAudio: frames of one sub-frame, 2.5 ms, and LengthAnalyzer's limits in
// them.
xsflib::rip::LengthRules JaudioLengthRules();

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
