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
    // (engine:SetVoiceCountMax), and the I3DL2 reverbs of its sound effect list.
    std::optional<int> voice_count;
    std::map<std::string, I3dl2Preset> i3dl2_presets;
};

// Whether a file is one of the encrypted forms a game comes in: an NSP, an XCI or an NCA. Returns its kind's name, or
// nothing.
std::optional<std::string> EncryptedKind(const std::filesystem::path& path);

// Reads a folder or a Bezel Engine archive. Returns an error message if it's neither, or can't be read.
std::optional<std::string> LoadGame(const std::filesystem::path& input, GameFiles& out);

} // namespace hsf::game
