// SPDX-License-Identifier: MIT

// A Switch game's files, as hsfrip reads them: a folder (a game's RomFS once it's extracted, or any folder of its
// files), or one Bezel Engine archive (.bea). The files inside the folder's Bezel Engine archives count as the game's
// files too. A game's NSP and XCI files hold its files encrypted, and are refused.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "game/sheet.h"

namespace hsf::game
{

struct GameFiles
{
    // The folder's or the archive's name.
    std::string name;

    // Every file's path among the game's files, with '/' between folders. A file in a Bezel Engine archive has the
    // archive's path, then the file's path in it.
    std::vector<std::string> files;

    // The sound archives (FSAR, whatever their names) among them.
    std::vector<std::string> sound_archives;

    // Reads a file by its path. Returns false if it can't.
    std::function<bool(const std::string& path, std::vector<uint8_t>& data)> read_file;

    // What a Bezel Engine game's files set for its sound: the voice count that its boot.lua gives the engine
    // (engine:SetVoiceCountMax), and the effects of its sound effect list: I3DL2 reverbs, delays and light limiters.
    std::optional<int> voice_count;
    std::map<std::string, I3dl2Preset> i3dl2_presets;
    std::map<std::string, RendererPreset> renderer_presets;
};

// An effect that a Bezel Engine game's code puts on an aux bus: the bus (0 to 2 for A to C), the preset's name, and the
// effect as an aux effect's type and parameters (docs/hsf.md, "Aux effects").
struct BusEffect
{
    uint32_t bus = 0;
    std::string name;
    uint32_t type = 0;
    std::vector<float> parameters;
};

// The effects that a Bezel Engine game's code puts on the aux buses before its sound starts, from the presets of its
// sound effect list. WarioWare: Move It! puts Delay_Waltz on aux bus A, I3DL2_Waltz on B and Limiter_Waltz on C as it
// starts (0xc7c1ac in its main). A list with those three presets gets them. Otherwise, a list with I3DL2_Default gets
// that reverb on bus A. WarioWare: Get It Together!'s scenes put it there unless one names another.
std::vector<BusEffect> StartupEffects(const GameFiles& game);

// Whether a file is one of the encrypted forms a game comes in: an NSP, an XCI or an NCA. Returns its kind's name, or
// nothing.
std::optional<std::string> EncryptedKind(const std::filesystem::path& path);

// Reads a folder or a Bezel Engine archive. Returns an error message if it's neither, or can't be read.
std::optional<std::string> LoadGame(const std::filesystem::path& input, GameFiles& out);

} // namespace hsf::game
