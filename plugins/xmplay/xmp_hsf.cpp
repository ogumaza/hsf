// SPDX-License-Identifier: MIT

// xmp-hsf: XMPlay input plugin for HSF (.hsf and .minihsf), from xsflib's plugin template (xsflib/xmplay/input.h). The
// template's comment says what the plugin does. This file gives HSF's traits: its player, names, extensions and limits.
//
// The player core (src/hsf) plays the sound with ntdlib's model of the SDK's sound library, through ntdlib's model of
// the console's audio renderer.
//
// Build: see plugins/xmplay/README.md (clang-cl for 32-bit Windows, with the XMPlay plugin SDK).

#include <windows.h>
#include <xmpin.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "xsflib/psf/psf.h"
#include "xsflib/xmplay/input.h"

#include "hsf/format.h"
#include "hsf/player.h"

namespace
{

struct Traits
{
    using Player = hsf::Player;
    using Set = hsf::LoadedSet;

    static constexpr const char* kName = "HSF";
    static constexpr std::array<const char*, 2> kExtensions = {"hsf", "minihsf"};
    static constexpr uint8_t kPsfVersion = hsf::kPsfVersionHsf;
    static constexpr uint64_t kMaxFileSize = uint64_t{1} << 30; // largest file read into memory

    // HSF has no tags that set how a file plays.
    static constexpr std::array<const char*, 0> kPlaybackTags = {};

    static constexpr const char* kVersion = HSF_VERSION;
    static constexpr const char* kAbout =
        "Plays HSF (Nintendo Switch sequenced music) rips with HSF's model of the SDK's sound library, through HSF's "
        "model of the console's audio renderer.\n\n"
        "MIT licence. Uses zlib.";

    static std::optional<std::string> LoadSet(const std::string& path, const xsflib::psf::FileReader& reader, Set& out,
                                              std::string_view separators)
    {
        return hsf::LoadSet(path, reader, out, separators);
    }
};

} // namespace

// XMPlay looks the plugin up by this name (the SDK's xmpin.def exports it).
XMPIN* WINAPI XMPIN_GetInterface(DWORD face, InterfaceProc faceproc)
{
    return xsflib::xmplay::Plugin<Traits>::GetInterface(face, faceproc);
}
