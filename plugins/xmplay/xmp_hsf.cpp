// SPDX-License-Identifier: MIT

// xmp-hsf: XMPlay input plugin for HSF (.hsf and .minihsf), from xsflib's plugin template (xsflib/xmplay/input.h). The
// template's comment says what the plugin does. plugin_traits.h defines HSF's traits.
//
// The player core (src/hsf) plays a rip on ntdlib's model of the sound library that the game uses: nn::atk with the
// console's audio renderer, JAudio, or NW4R SND.
//
// Build: see plugins/xmplay/README.md (clang-cl for 32-bit Windows, with the XMPlay plugin SDK).

#include <windows.h>
#include <xmpin.h>

#include "xsflib/xmplay/input.h"

#include "../plugin_traits.h"

// XMPlay looks the plugin up by this name (the SDK's xmpin.def exports it).
XMPIN* WINAPI XMPIN_GetInterface(DWORD face, InterfaceProc faceproc)
{
    return xsflib::xmplay::Plugin<hsf::PluginTraits>::GetInterface(face, faceproc);
}
