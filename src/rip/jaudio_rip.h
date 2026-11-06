// SPDX-License-Identifier: MIT

// hsfrip's parts for games that play their sound with JAudio: the game's sound files among its files, what the game's
// code sets up for JAudio, the sounds to rip, and the library and minis of a rip.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <vector>

#include "hsf/format.h"
#include "jaudio/engine.h"
#include "rip/rip.h"

namespace hsf::rip
{

// Reads a file of the game by its path. Returns false if it can't.
using ReadGameFile = std::function<bool(const std::string& path, std::vector<uint8_t>& data)>;

// A game's JAudio files, by their paths among the game's files: the start-up archive (BAA), the sequence archive (an
// RARC of BMS files, or empty for a game without one) and the folder of the wave archives (AW).
struct JaudioPaths
{
    std::string startup;
    std::string sequences;
    std::string waves;
};

// Whether `data` is a start-up archive of JAudio2 (BAA), big-endian or little-endian, or one compressed with Yaz0.
bool IsStartupArchive(std::span<const uint8_t> data);

// The JAudio files among a game's files `files`, or nothing. The start-up archive is a file directly in a folder named
// AudioRes, with a name that ends in .szs or .baa, that IsStartupArchive takes. The sequence archive is the first file
// whose name ends in .arc in the folder's Seqs folder, and the wave archives are in its Waves folder. `read` reads the
// game's files.
std::optional<JaudioPaths> FindJaudioFiles(const std::vector<std::string>& files, const ReadGameFile& read);

// A variant of a sound: the sound with the track states that a later state of the game gives it. The variant's label
// is the sound's label, a space and `name`.
struct JaudioVariant
{
    JaudioTrackStates states;
    std::string name;
};

// A game whose code HSF has read: its name, the SHA-1 of its start-up archive as the game's files hold it, what its
// code sets up for JAudio, and the groups of sounds that the game's code plays as other sounds. Each group is its
// section times 256 plus its number. `bgm_params` is the path, from the start-up archive's folder, of a table of the
// music's volumes that the game's code reads (see ReadBgmVolumes), or empty. `variants` holds the sounds' variants,
// each ripped beside its sound.
struct JaudioGame
{
    std::string name;
    std::string startup_sha1;
    JaudioSettings settings;
    std::vector<uint32_t> replaced_groups;
    std::string bgm_params;
    std::vector<JaudioVariant> variants;
};

// The volumes that the game's code writes into the sound table from its table of the music's volumes. Super Mario
// Galaxy 2's `main` does that at 0x128cf0. The table is an RARC, compressed with Yaz0 or not, that holds BgmParam.bcsv:
// a JMap table with the columns SoundIdName and Volume, in the archive's byte order. The game writes each row's volume,
// its low byte, over the volume of the sound that the row names, if the sound's type has bit 0x40 set. Rows that name
// no sound of `archive` change nothing. Throws xsflib::FormatError if the RARC or the table is damaged, or if the
// RARC lacks the table.
std::vector<JaudioSoundVolume> ReadBgmVolumes(std::span<const uint8_t> table_archive,
                                              const ntdlib::jaudio::SoundArchive& archive);

// The game whose start-up archive is `startup`, or null.
const JaudioGame* FindJaudioGame(std::span<const uint8_t> startup);

// A sound to rip: its index in the sound table, its label, its ID, its timing once the length analysis has worked it
// out, and the notes that the files can't play (JaudioTiming). A variant's job holds the variant's track states, or
// in library 2 the tracks that the variant's piece mutes.
struct JaudioJob
{
    uint32_t index = 0;
    std::string label;
    uint32_t id = 0;
    std::optional<Timing> timing;
    uint32_t missing_bank_notes = 0;
    uint32_t sequence_draws = 0; // library 2: the numbers that the sound's sequence draws from the sequences' generator
    std::optional<JaudioTrackStates> track_states;
    std::optional<uint16_t> muted_tracks;
};

// Whether `job` is a variant of a sound. A variant's mini holds its descriptor (JobDescriptor).
inline bool IsVariant(const JaudioJob& job)
{
    return job.track_states || job.muted_tracks;
}

// The sounds of the sound table that play sequences that the files hold, in the table's order, whose labels match
// `only`. With `bgm`, only music: the sounds of section 1. The game plays them through its sequence manager. The
// groups that `game` replaces are left out. Each of `game`'s variants of a sound follows the sound, if the variant's
// label matches `only`. `game` may be null.
std::vector<JaudioJob> SelectJaudioSounds(const jaudio::SoundFiles& files, const JaudioGame* game,
                                          const std::regex& only, bool bgm);

// The descriptor that a job's rip plays with: `descriptor`, with a variant's track states in place of those that it
// gives the sound, or a variant's muted tracks in place of those of its piece.
ArchiveDescriptor JobDescriptor(ArchiveDescriptor descriptor, const JaudioJob& job);

// A sound's label: its name in the sound table, or "sound" and its ID in hex.
std::string JaudioLabel(const ntdlib::jaudio::SoundEntry& sound);

// The library of a rip: the start-up archive, the sequence archive and the files in `waves`, in FILE chunks at their
// paths among the game's files, with `descriptor` (version 2) naming them, and the tags. `waves` holds the wave
// archives, and then Pikmin 2's conductor archive or Pikmin's sequence index.
std::vector<uint8_t> BuildJaudioLibrary(const FileChunk& startup, const std::optional<FileChunk>& sequences,
                                        const std::vector<FileChunk>& waves, ArchiveDescriptor descriptor,
                                        const xsflib::psf::Tags& tags);

// A mini that plays `job`'s sound from the library `lib_name`. A variant's mini holds the variant's descriptor
// (JobDescriptor of `descriptor`, the library's). As the last descriptor that a set loads, it takes the library's
// place.
std::vector<uint8_t> BuildJaudioMini(const JaudioJob& job, const ArchiveDescriptor& descriptor,
                                     const std::string& lib_name, xsflib::psf::Tags tags);

} // namespace hsf::rip
