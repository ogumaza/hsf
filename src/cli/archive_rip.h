// SPDX-License-Identifier: MIT

// hsfrip's archive mode: a Switch sound archive, with its group files and the game's settings, ripped through
// ntdlib's ArchiveRipTool.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "ntdlib/bezel/sound_levels.h"
#include "ntdlib/rip/archive_mode.h"

#include "hsf/format.h"
#include "rip/rip.h"

namespace hsf::cli
{

// What a sound archive is ripped with: the archive's file and the group files found for it, by their paths among the
// game's files (or beside a loose archive), and the descriptor's settings. The length analysis plays with the same
// settings. A Bezel Engine game's sound sheets give its sounds' levels. A loose archive has none.
struct ArchiveToRip
{
    FileChunk archive;
    std::vector<FileChunk> groups;
    hsf::ArchiveDescriptor settings;
    const ntdlib::bezel::SoundLevels* levels = nullptr;
    bool game_code = false; // true for a game's archive ripped with the game's effects and rules

    // Finds another of the game's sound archives by its file name without the extension, for a piece whose companion
    // it holds (rip/variable_states.h). Nothing when the game has no such archive.
    std::function<std::optional<FileChunk>(std::string_view name)> find_archive;

    // Reads one of the game's files by its path, for a piece that follows a stream's music sync (WithSyncedPieces). A
    // file in a Bezel Engine archive has its path in the archive. Returns false when the game has no such file, and
    // for a loose archive.
    std::function<bool(std::string_view path, std::vector<uint8_t>& data)> find_file;
};

// hsfrip's archive mode (ntdlib::rip::ArchiveRipTool). A library holds only the files that its sequences play.
const ntdlib::rip::ArchiveRipTool<hsf::rip::RipFormat, ArchiveToRip>& ArchiveTool();

} // namespace hsf::cli
