// SPDX-License-Identifier: MIT

// The paths of a game's files, with '/' between folders (hsf::game::GameFiles), and the reader of a file by its path.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "xsflib/ascii.h"

namespace hsf::rip
{

// Reads one of the game's files by its path. Returns false if it can't.
using ReadGameFile = std::function<bool(const std::string& path, std::vector<uint8_t>& data)>;

// The name of the file or folder at `path`: the part after the last '/'.
inline std::string FileNameOf(std::string_view path)
{
    const std::size_t slash = path.rfind('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

// The folder that holds the file or folder at `path`, without a final '/'. Empty at the top.
inline std::string FolderOf(std::string_view path)
{
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
}

// The path of `name` in `folder`, or `name` alone for no folder.
inline std::string InFolder(std::string_view folder, std::string_view name)
{
    return folder.empty() ? std::string(name) : std::string(folder) + "/" + std::string(name);
}

// `name` without its extension: the part before the last '.'.
inline std::string WithoutExtension(std::string_view name)
{
    return std::string(name.substr(0, name.rfind('.')));
}

// Whether `text` ends in `end` in any case. `end` is in lower case.
inline bool EndsWithLower(std::string_view text, std::string_view end)
{
    return text.size() >= end.size() && xsflib::AsciiLower(std::string(text.substr(text.size() - end.size()))) == end;
}

// Whether `path` is `name`, or ends in a '/' and `name`.
inline bool NamesFile(std::string_view path, std::string_view name)
{
    return path == name ||
           (path.size() > name.size() && path.ends_with(name) && path[path.size() - name.size() - 1] == '/');
}

} // namespace hsf::rip
