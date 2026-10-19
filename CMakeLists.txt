# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
# Shared version for the tools (HSF_VERSION_ARGS below). For a release, update it here and add notes in
# docs/release-notes/.
project(hsf VERSION 1.1.2 LANGUAGES C CXX)

# xsflib: a library for players and rippers of the PSF family, as a submodule. Its XsflibProject.cmake sets what a
# project built on it starts with: C++20, a release build by default, the static C runtime, the options that keep the
# output the same on every platform, and the warnings for HSF's targets (HSF_WARNINGS). It refuses the compilers whose
# output would differ from other builds.
if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/externals/xsflib/CMakeLists.txt")
    message(FATAL_ERROR "externals/xsflib is missing. Run: git submodule update --init --recursive")
endif()
include(externals/xsflib/cmake/XsflibProject.cmake)
xsflib_project(CHECK_COMPILERS NAME HSF PREFIX HSF)

# xsflib also provides the libraries that HSF requires: zlib for the PSF container's compressed program, Zstandard for
# Bezel Engine's archives (.bea) and libFLAC for the tools' FLAC output. Each comes from the system where it can, or is
# built along with HSF. ntdlib, a library for the hardware and formats of Nintendo's consoles, builds on xsflib.
set(XSFLIB_PSF ON)
set(XSFLIB_CLI ON)
set(XSFLIB_ZSTD ON)
xsflib_add_libraries(xsflib ntdlib)

# xsflib's helpers that the other libraries share (byte order, ASCII lowercasing), and the include directory for all of
# src.
add_library(hsf_common INTERFACE)
target_include_directories(hsf_common INTERFACE src)
target_link_libraries(hsf_common INTERFACE xsflib::common)

# Switch games' files: folders, and Bezel Engine's archives (.bea) and data sheets.
add_library(hsf_game STATIC
    src/game/bea.cpp
    src/game/bea.h
    src/game/game.cpp
    src/game/game.h
    src/game/sheet.cpp
    src/game/sheet.h
)
target_link_libraries(hsf_game PUBLIC hsf_common PRIVATE xsflib::zstd)
target_compile_options(hsf_game PRIVATE ${HSF_WARNINGS})

# The HSF file format: the PSF container, chunks, the archive descriptor and tags, and loading a set with its libraries.
add_library(hsf_format STATIC
    src/hsf/format.cpp
    src/hsf/format.h
)
target_link_libraries(hsf_format PUBLIC hsf_common xsflib::psf)
target_compile_options(hsf_format PRIVATE ${HSF_WARNINGS})

# The player: a set's sound on the model. Playback with lengths, fades and seeking, and rendering ahead on a worker
# thread for the foobar2000 component and the XMPlay plugin, come from xsflib::player.
add_library(hsf_player STATIC
    src/hsf/player.cpp
    src/hsf/player.h
)
target_link_libraries(hsf_player PUBLIC hsf_format ntdlib::nx xsflib::player)
target_compile_options(hsf_player PRIVATE ${HSF_WARNINGS})

add_library(hsf_rip STATIC
    src/rip/rip.cpp
    src/rip/rip.h
)
target_link_libraries(hsf_rip PUBLIC hsf_format ntdlib::nx xsflib::rip ntdlib::rip_archive)
target_compile_options(hsf_rip PRIVATE ${HSF_WARNINGS})

# The version of each program, as HSF_VERSION, and on Windows the version resource that Explorer shows in the file's
# properties (xsflib_add_version).
set(HSF_VERSION_ARGS VERSION ${hsf_VERSION} DEFINE HSF_VERSION PRODUCT HSF
    COPYRIGHT "Copyright (c) 2026 the HSF authors. MIT licence.")

# hsfrip rips on several threads (--jobs). glibc before 2.34 keeps threads in a separate library.
find_package(Threads REQUIRED)
add_executable(hsfrip src/cli/hsfrip.cpp)
target_link_libraries(hsfrip PRIVATE hsf_rip hsf_game xsflib::cli Threads::Threads)
target_compile_options(hsfrip PRIVATE ${HSF_WARNINGS})
xsflib_add_version(hsfrip ${HSF_VERSION_ARGS} DESCRIPTION "HSF ripper")

add_executable(hsfplay src/cli/hsfplay.cpp)
target_link_libraries(hsfplay PRIVATE hsf_player xsflib::cli)
target_compile_options(hsfplay PRIVATE ${HSF_WARNINGS})
xsflib_add_version(hsfplay ${HSF_VERSION_ARGS} DESCRIPTION "HSF renderer")

# Windows: UTF-8 as the tools' code page, so that they can take and open paths outside the system's ANSI code page,
# such as those of files dropped on them. macOS: a droplet app for each tool (hsfrip.app, hsfplay.app), so that files
# can be dropped on it in the Finder.
foreach (tool hsfrip hsfplay)
    xsflib_add_utf8_manifest(${tool})
    xsflib_add_droplet(${tool} VERSION ${hsf_VERSION})
endforeach()

# foobar2000 component (Windows with clang-cl, or macOS): cmake -DFOOBAR2000_SDK_DIR=<unpacked SDK> ...
set(FOOBAR2000_SDK_DIR "" CACHE PATH "foobar2000 SDK (builds the foo_input_hsf component when set)")
if (FOOBAR2000_SDK_DIR)
    if (NOT MSVC AND NOT APPLE)
        message(FATAL_ERROR "the foobar2000 component requires clang-cl on Windows, or macOS")
    endif()
    add_subdirectory(plugins/foobar2000)
endif()

# XMPlay plugin (32-bit Windows with clang-cl): cmake -A Win32 -T ClangCL -DXMPLAY_SDK_DIR=<unpacked SDK> ...
set(XMPLAY_SDK_DIR "" CACHE PATH "XMPlay plugin SDK (builds the xmp-hsf plugin when set)")
if (XMPLAY_SDK_DIR)
    add_subdirectory(plugins/xmplay)
endif()

# The tests. -DHSF_TESTS=OFF leaves them out of a build that only makes the programs and plugins, such as CI's macOS
# job.
option(HSF_TESTS "Build HSF's tests and register them with CTest." ON)
if (HSF_TESTS)
    enable_testing()

    # The unit tests: hsf_<name>_test from tests/<name>_test.cpp, registered as hsf_<name>.
    xsflib_add_project_test(hsf format hsf_format)
    xsflib_add_project_test(hsf game hsf_game xsflib::zstd)
    xsflib_add_project_test(hsf player hsf_player)
    xsflib_add_project_test(hsf rip hsf_rip)

    # The tools report the version they were built as.
    string(REPLACE "." "\\." version_pattern ${hsf_VERSION})
    foreach (tool hsfrip hsfplay)
        add_test(NAME version_${tool} COMMAND ${tool} --version)
        set_tests_properties(version_${tool} PROPERTIES PASS_REGULAR_EXPRESSION "^${tool} ${version_pattern}[\r\n]*$")
    endforeach()

    # Load local_tests.cmake from each immediate subdirectory to register tests that use locally supplied files.
    xsflib_include_local_tests()
endif()
