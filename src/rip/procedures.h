// SPDX-License-Identifier: MIT

// The games whose handler of user procedures HSF runs (docs/hsf.md, "User procedures"): Super Mario Party Jamboree and
// Mario Party Superstars. A rip of such a game's sound archives names the handler in its descriptor.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "hsf/format.h"
#include "rip/game_paths.h"

namespace hsf::rip
{

// The handler of user procedures for a game's files, or nothing for another game. `files` lists the paths of the
// game's files. The game is known by its project archive, _Resident/AddonAudioProject.fspj, by the archive's size and
// SHA-1. Jamboree's handler gets each table that the game's files hold, audio/userproc/conv/00.msgpack to 15.msgpack.
// Throws xsflib::FormatError for a table of another form.
std::optional<UserProcedureHandler> GameProcedures(const std::vector<std::string>& files, const ReadGameFile& read);

// Whose handler `handler` is, such as "Super Mario Party Jamboree's", for messages.
std::string HandlerName(uint32_t handler);

} // namespace hsf::rip
