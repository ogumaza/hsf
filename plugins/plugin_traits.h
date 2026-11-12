// SPDX-License-Identifier: MIT

// The traits that the foobar2000 component and the XMPlay plugin share, for xsflib's plugin templates
// (xsflib/foobar2000/input.h and xsflib/xmplay/input.h): HSF's player, names, extensions, limits, version and about
// text. Each plugin's traits derive from PluginTraits. The plugin's CMake target defines HSF_VERSION.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "xsflib/psf/psf.h"

#include "hsf/format.h"
#include "hsf/player.h"

namespace hsf
{

struct PluginTraits
{
    using Player = hsf::Player;
    using Set = hsf::LoadedSet;

    static constexpr const char* kName = "HSF";
    static constexpr std::array<const char*, 2> kExtensions = {"hsf", "minihsf"};
    static constexpr uint8_t kPsfVersion = kPsfVersionHsf;
    static constexpr uint64_t kMaxFileSize = uint64_t{1} << 30; // largest file read into memory

    // HSF has no tags that set how a file plays.
    static constexpr std::array<const char*, 0> kPlaybackTags = {};

    static constexpr const char* kVersion = HSF_VERSION;
    static constexpr const char* kAbout =
        "Plays HSF (Nintendo Switch sequenced music) rips on models of the games' sound libraries: nn::atk with the "
        "console's audio renderer, JAudio for the Super Mario Galaxy games and Pikmin 1 and 2, and NW4R SND for "
        "Skyward Sword HD.\n\n"
        "MIT licence. Uses zlib.";

    static std::optional<std::string> LoadSet(const std::string& path, const xsflib::psf::FileReader& reader, Set& out,
                                              std::string_view separators)
    {
        return hsf::LoadSet(path, reader, out, separators);
    }
};

} // namespace hsf
