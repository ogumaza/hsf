// SPDX-License-Identifier: MIT

// HSF ripper: builds an .hsflib for one of a game's sound archives and one .minihsf per sequence, played by ntdlib's
// model of the SDK's sound library and the console's audio renderer, and works out how long each sequence plays.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/rip/archive.h"
#include "xsflib/rip/file_names.h"

#include "hsf/format.h"
#include "hsf/nx.h"
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
// with the effects a rip puts on the aux buses. Analyze can be called from several threads at once.
class LengthAnalyzer
{
public:
    // `archive` is the sound archive, with the group files that it requires added. The model plays it with `options`,
    // and `effects` on the aux buses.
    explicit LengthAnalyzer(fsar::SoundArchive archive, atk::EngineOptions options = {},
                            std::vector<AuxEffect> effects = {});
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

} // namespace hsf::rip
