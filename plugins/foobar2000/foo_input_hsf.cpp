// SPDX-License-Identifier: MIT

// foo_input_hsf: foobar2000 input component for HSF (.hsf and .minihsf), from xsflib's input template
// (xsflib/foobar2000/input.h). The template's comment says what the component does. This file defines the decoder's
// name and GUIDs. plugin_traits.h defines HSF's other traits.
//
// The player core (src/hsf) plays a rip on ntdlib's model of the sound library that the game uses: nn::atk with the
// console's audio renderer, JAudio, or NW4R SND.
//
// Build: see plugins/foobar2000/README.md (clang-cl on Windows, or Apple Clang on macOS, with the foobar2000 SDK).

#include "xsflib/foobar2000/input.h"
#include <SDK/foobar2000.h>

#include "../plugin_traits.h"

namespace
{

struct Traits : hsf::PluginTraits
{
    static constexpr const char* kDecoderName = "HSF decoder";

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

DECLARE_COMPONENT_VERSION("HSF Decoder", hsf::PluginTraits::kVersion, hsf::PluginTraits::kAbout);

VALIDATE_COMPONENT_FILENAME("foo_input_hsf.dll");
