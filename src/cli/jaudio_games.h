// SPDX-License-Identifier: MIT

// hsfrip's rips of the games that play their sound with JAudio: JAudio2, JAudio1 (Pikmin 2) and the C JAudio
// (Pikmin).

#pragma once

#include <filesystem>
#include <string>

#include "xsflib/cli/rip.h"

#include "cli/hsfrip.h"
#include "game/game.h"
#include "rip/jac_rip.h"
#include "rip/jaudio1_rip.h"
#include "rip/jaudio_rip.h"

namespace hsf::cli
{

// Rips a game that plays its sound with JAudio: one library of its sound files, and a mini for each sound that plays
// a sequence. The settings come from the game's code when HSF holds them for the game. The folder of the rip has the
// start-up archive's name. Returns the exit code.
int RipJaudioGame(const std::string& input, const hsf::game::GameFiles& game, const hsf::rip::JaudioPaths& paths,
                  const std::filesystem::path& out_dir, const Options& o, xsflib::cli::FileNameRegistries& registries);

// Rips Pikmin 2, whose Switch release plays its sound with JAudio1: one library of its sound files with the conductor
// archive, and a mini for each piece of music, each conductor of the cave music and each effect. The settings come
// from the game's code and from its list of its music and its track maps. The folder of the rip has the start-up
// archive's name. Returns the exit code.
int RipJaudio1Game(const std::string& input, const hsf::game::GameFiles& game, const hsf::rip::Jaudio1Paths& paths,
                   const std::filesystem::path& out_dir, const Options& o, xsflib::cli::FileNameRegistries& registries);

// Rips Pikmin, whose sound plays with the C JAudio: one library of the game's sound files and of the sequence index
// from its main, and a mini for each piece of music, each piece of the cutscenes' music and each effect. The settings
// come from the game's main, one of the builds that hsfrip reads. The folder of the rip has the bank file's name.
// Returns the exit code.
int RipJacGame(const std::string& input, const hsf::game::GameFiles& game, const hsf::rip::JacPaths& paths,
               const std::filesystem::path& out_dir, const Options& o, xsflib::cli::FileNameRegistries& registries);

} // namespace hsf::cli
