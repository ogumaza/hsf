// SPDX-License-Identifier: MIT

// HSF ripper: builds an .hsflib for one of a game's sound archives and one .minihsf per sequence, played by ntdlib's
// model of the SDK's sound library and the console's audio renderer, and works out how long each sequence plays. For a
// game that plays its sound with JAudio, the same on the model of JAudio.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "ntdlib/rip/archive.h"
#include "xsflib/rip/file_names.h"

#include "hsf/format.h"
#include "hsf/sar.h"
#include "jaudio/engine.h"

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
    // sound is marked silent, and gets the 0.5 s that follows a sequence's last sound. Returns nullopt when the length
    // can't be worked out (and then no length tag is written). Throws std::bad_alloc when memory runs out.
    std::optional<Timing> Analyze(uint32_t sound_index) const;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

// What the length analysis finds of a JAudio sound: its timing, and the notes that it plays on banks that the files
// don't load. The game stops at the first such note. Such a sound's data isn't all there.
struct JaudioTiming
{
    std::optional<Timing> timing;
    uint32_t missing_bank_notes = 0;
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
