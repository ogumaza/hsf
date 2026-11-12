// SPDX-License-Identifier: MIT

// The settings that hsfrip gives a game's sound archives, the messages about them, and its list of a game.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "cli/hsfrip.h"
#include "game/game.h"
#include "hsf/format.h"

namespace hsf::cli
{

// Reports the error when the file at `path` is another console's sound archive. `head` holds the file's first bytes.
// Returns whether the file is one.
bool RefuseForeignArchive(const std::string& path, std::span<const uint8_t> head);

// Reports the error when --archive names a sound archive that isn't among a game's `archives`. Returns whether it does.
bool RefuseMissingArchive(const std::vector<std::string>& archives, const Options& o);

// What a sound archive holds (such as "FSAR 2.2.0, 2911 sequences, 28 wave sounds, 220 streams"), or why it can't be
// read or ripped.
std::string DescribeArchive(std::vector<uint8_t> bytes);

// The settings that the options give every sound archive: the voice count of --voices, or else nn::atk's default.
hsf::ArchiveDescriptor OptionSettings(const Options& o);

// Whether the voice count comes from the game's boot.lua: --voices isn't given, and boot.lua gives a count from 1 to
// 96.
bool VoicesFromBoot(const hsf::game::GameFiles& game, const Options& o);

// The effects that a Bezel Engine game's code puts on the aux buses (hsf::game::StartupEffects), or none with
// --no-effects.
std::vector<hsf::game::BusEffect> GameEffects(const hsf::game::GameFiles& game, const Options& o);

// The effects as hsfrip reports them, such as "I3DL2_Default on aux bus A", or `none` without any.
std::string EffectsText(const std::vector<hsf::game::BusEffect>& effects, const std::string& none);

// The settings every sound archive of a game is ripped with: the options' settings, the voice count of the game's
// boot.lua when --voices isn't given, the effects a Bezel Engine game puts on the aux buses, and the game's handler of
// user procedures (hsf::rip::GameProcedures). Throws xsflib::FormatError for a table of the handler that the game's
// files hold in another form.
hsf::ArchiveDescriptor GameSettings(const hsf::game::GameFiles& game, const Options& o);

// What the settings say about the game's user procedures, for the summary: whose handler runs them, if any.
std::string ProceduresText(const hsf::ArchiveDescriptor& settings);

// Prints --list for a game: its sound archives, what each one holds, and the settings that a rip gives them.
void ListGame(const std::string& input, const hsf::game::GameFiles& game, const hsf::ArchiveDescriptor& settings,
              const Options& o);

} // namespace hsf::cli
