// SPDX-License-Identifier: MIT

// HSF ripper: builds an .hsflib for one of a game's sound archives and one .minihsf per sequence, played by HSF's model
// of the SDK's sound library and the console's audio renderer, and works out how long each sequence plays.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "atk/engine.h"
#include "fsar/formats.h"
#include "hsf/format.h"

namespace hsf::rip
{

// Whether `bytes` start as a Wii U sound archive: an FSAR whose byte order mark is big-endian. The readers read the
// archive, but the archive's sequences have big-endian arguments too, and nn::atk reads them little-endian. So the
// ripper refuses such an archive.
bool IsWiiUArchive(std::span<const uint8_t> bytes);

// The names of the groups `archive` lists with no file: those that the game keeps outside the archive in group files.
std::vector<std::string> ExternalGroups(const fsar::SoundArchive& archive);

// An .hsflib holding the sound archive and the group files that its sequences require, by their paths in the game's
// files. The .hsflib has an archive descriptor with `settings`' voice count, SDK version and aux effects.
std::vector<uint8_t> BuildArchiveLibrary(const FileChunk& archive, const std::vector<FileChunk>& groups,
                                         ArchiveDescriptor settings, const Tags& tags);

// A .minihsf that plays sound `index` from the library `lib_name`.
std::vector<uint8_t> BuildMini(uint32_t index, const std::string& lib_name, Tags tags);

// Lists the data that sound `index` requires and the archive (with its added group files) doesn't hold: the sound's
// sequence and banks, and the banks' wave archives. Such data is kept in another file, or cut off by truncation. Empty
// when nothing is missing.
std::vector<std::string> MissingData(const fsar::SoundArchive& archive, uint32_t index);

// Whether `hsfrip --bgm` picks the sound at `index` as music: a sequence whose label has BGM in it, or one that plays
// on a music player. Games give their music players names with BGM, MUSIC, SONG or FANFARE among the words that
// underscores separate (PLAYER_BGM, SONG_PLAYER), even where the music's labels don't say so. A player whose name also
// has the word SE plays sound effects.
bool IsMusic(const fsar::SoundArchive& archive, uint32_t index);

// Filenames for the rips in one folder, valid on Windows, macOS and Linux and unique even on case-insensitive
// filesystems.
class FileNames
{
public:
    // "<base>.hsflib".
    std::string Library(const std::string& base);

    // "<index, four digits> <label>.minihsf".
    std::string Mini(uint32_t index, const std::string& label);

private:
    std::string Unique(const std::string& stem, const std::string& extension);

    std::set<std::string> taken_; // the names given out so far, in lowercase
};

// A sequence's length and fade in milliseconds, and whether it never makes a sound.
struct Timing
{
    long long length_ms;
    long long fade_ms;
    bool silent = false;
};

// Works out how long sequences play by running them on HSF's model of nn::atk and the audio renderer (src/atk and
// src/audio), with the effects a rip puts on the aux buses. Analyze can be called from several threads at once.
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
