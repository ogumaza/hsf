# SPDX-License-Identifier: MIT

# xmp-hsf: the XMPlay input plugin, a 32-bit DLL built with clang-cl. XMPlay is a 32-bit program. Added by the top-level
# CMakeLists.txt when XMPLAY_SDK_DIR is set. See README.md. xsflib builds it against the SDK's headers
# (xsflib_add_xmplay_plugin).

xsflib_add_xmplay_plugin(xmp-hsf
    SDK ${XMPLAY_SDK_DIR}
    SOURCES xmp_hsf.cpp
    LIBRARIES hsf_player
    OPTIONS ${HSF_WARNINGS})
xsflib_add_version(xmp-hsf ${HSF_VERSION_ARGS} DESCRIPTION "HSF plugin for XMPlay")
