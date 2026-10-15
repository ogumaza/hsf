// SPDX-License-Identifier: MIT

// foo_input_hsf: foobar2000 input component for HSF (.hsf and .minihsf), from xsflib's input template
// (xsflib/foobar2000/input.h). The template's comment says what the component does. This file gives HSF's traits: its
// player, names, extensions, GUIDs and limits.
//
// The player core (src/hsf) plays the sound with ntdlib's model of the SDK's sound library, through ntdlib's model of
// the console's audio renderer.
//
// Build: see plugins/foobar2000/README.md (clang-cl on Windows, or Apple Clang on macOS, with the foobar2000 SDK).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "xsflib/foobar2000/input.h"
#include "xsflib/psf/psf.h"
#include <SDK/foobar2000.h>

#include "hsf/format.h"
#include "hsf/player.h"

namespace
{

struct Traits
{
    using Player = hsf::Player;
    using Set = hsf::LoadedSet;

    static std::optional<std::string> LoadSet(const std::string& path, const xsflib::psf::FileReader& reader, Set& out,
                                              std::string_view separators)
    {
        return hsf::LoadSet(path, reader, out, separators);
    }

    static constexpr const char* kName = "HSF";
    static constexpr const char* kDecoderName = "HSF decoder";
    static constexpr std::array<const char*, 2> kExtensions = {"hsf", "minihsf"};
    static constexpr uint8_t kPsfVersion = hsf::kPsfVersionHsf;
    static constexpr t_filesize kMaxFileSize = t_filesize{1} << 30; // largest file read into memory
    static constexpr std::array<const char*, 0> kKeptTags = {};

    // {B098EC3B-4AA0-48F8-8AFC-47F4A5B857EE}
    static constexpr GUID kInputGuid = {0xb098ec3b, 0x4aa0, 0x48f8, {0x8a, 0xfc, 0x47, 0xf4, 0xa5, 0xb8, 0x57, 0xee}};
    // Advanced preferences entries, under Decoding > HSF decoder.
    static constexpr GUID kBranchGuid = {0x0cbafc7b, 0x9e8e, 0x469a, {0x82, 0xfa, 0x89, 0x99, 0xbc, 0x0a, 0xf8, 0x4b}};
    static constexpr GUID kLengthGuid = {0xddc4c1d5, 0xa9b2, 0x41c9, {0x8e, 0xf7, 0xea, 0x77, 0x02, 0xdf, 0x7a, 0x4f}};
    static constexpr GUID kFadeGuid = {0xf317d114, 0xcb1e, 0x42a2, {0xbc, 0x9b, 0x97, 0x62, 0x28, 0x04, 0xdf, 0xb1}};
    static constexpr GUID kEndlessGuid = {0x06179ada, 0x4d8b, 0x4652, {0xb9, 0x83, 0x23, 0xc4, 0x67, 0x82, 0xf3, 0xa7}};
    static constexpr GUID kRenderAheadGuid = {
        0x63821a5f, 0x89e7, 0x4535, {0xb6, 0xdc, 0xad, 0x3a, 0x31, 0x12, 0x0e, 0xad}};
};

xsflib::foobar2000::Settings<Traits> g_settings;
input_singletrack_factory_t<xsflib::foobar2000::Input<Traits>> g_input_hsf_factory;

} // namespace

DECLARE_FILE_TYPE("HSF files", "*.HSF;*.MINIHSF");

DECLARE_COMPONENT_VERSION("HSF Decoder", HSF_VERSION,
                          "Plays HSF (Nintendo Switch sequenced music) rips with HSF's model of the SDK's sound "
                          "library, through HSF's model of the console's audio renderer.\n\n"
                          "MIT licence. Uses zlib.");

VALIDATE_COMPONENT_FILENAME("foo_input_hsf.dll");
