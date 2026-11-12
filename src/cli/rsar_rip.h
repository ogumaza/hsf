// SPDX-License-Identifier: MIT

// hsfrip's rips of Wii sound archives (.brsar), such as those of Skyward Sword HD.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "xsflib/cli/rip.h"

#include "cli/hsfrip.h"
#include "game/game.h"
#include "hsf/format.h"

namespace hsf::cli
{

// Rips the Wii sound archive `rsar` into the folder of `out_dir` named after it: a library of the part of the archive
// that the sequences play, with a version 3 descriptor, and a mini for each sequence. The model plays the archive on
// Skyward Sword HD's layer, the only layer that HSF has. Sequences whose data isn't all there are listed and skipped,
// and so are those that the length analysis finds never sound. Returns the exit code.
int RipRsar(FileChunk rsar, const std::string& game_name, const std::filesystem::path& out_dir, const Options& o,
            xsflib::cli::FileNameRegistries& registries);

// Rips the Wii sound archives among a game's files, one after another (RipRsar). --archive picks one. Returns the
// exit code: 1 if any archive failed.
int RipRsarGame(const std::string& input, const hsf::game::GameFiles& game, const std::vector<std::string>& archives,
                const std::filesystem::path& out_dir, const Options& o, xsflib::cli::FileNameRegistries& registries);

} // namespace hsf::cli
