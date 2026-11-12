// SPDX-License-Identifier: MIT

// hsfrip's parts for a Wii sound archive (RSAR) that a game's Switch release plays with NW4R SND, as Skyward Sword HD
// does: the archives among the game's files, the sounds to rip, their lengths, and the library and minis of a rip
// (docs/hsf.md, "Version 3: NW4R SND").

#pragma once

#include <cstdint>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/rsar/formats.h"

#include "hsf/format.h"
#include "rip/rip.h"

namespace hsf::rip
{

// Whether `data` starts as a Wii sound archive.
bool IsRsar(std::span<const uint8_t> data);

// The game's files whose names end in .brsar, in the order of `files`.
std::vector<std::string> FindRsarFiles(const std::vector<std::string>& files);

// A sound to rip: its index in the archive's sound table, its label, and its timing once the length analysis has
// worked it out.
struct Nw4rJob
{
    uint32_t index = 0;
    std::string label;
    std::optional<Timing> timing;
};

// The archive's sequence sounds, in the table's order, whose labels match `only`. With `bgm`, only the music
// (ntdlib::rsar::IsMusic). A sound without a label gets "SEQ_" and its index.
std::vector<Nw4rJob> SelectNw4rSounds(const ntdlib::rsar::SoundArchive& archive, const std::regex& only, bool bgm);

// The timing of the sequence sound at `index` on the model of NW4R SND on the layer of `settings`
// (ntdlib::rvl::nx::LengthConsole), or nothing for a sound that doesn't start or that the model can't play.
std::optional<Timing> AnalyzeNw4rLength(const ntdlib::rsar::SoundArchive& archive, const Nw4rSettings& settings,
                                        uint32_t index);

// The library of a rip: `archive` in a FILE chunk at its path among the game's files, with `descriptor` (version 3)
// naming it, and the tags.
std::vector<uint8_t> BuildNw4rLibrary(const FileChunk& archive, const ArchiveDescriptor& descriptor,
                                      const xsflib::psf::Tags& tags);

// A mini that plays `job`'s sound from the library `lib_name`.
std::vector<uint8_t> BuildNw4rMini(const Nw4rJob& job, const std::string& lib_name, xsflib::psf::Tags tags);

} // namespace hsf::rip
