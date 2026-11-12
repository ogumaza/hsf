// SPDX-License-Identifier: MIT

// The profiles of the JAudio2 games whose code HSF has read: what each game's code sets up for JAudio, and the
// variants of its sounds. Also the table of the music's volumes that Super Mario Galaxy 2 reads from its files.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/jaudio/archive.h"

#include "hsf/format.h"

namespace hsf::rip
{

// The sections of the sound table: sound effects, and the music that the game's sequence manager plays.
inline constexpr uint8_t kMusicSection = 1;

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

} // namespace hsf::rip
