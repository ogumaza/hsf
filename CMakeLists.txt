# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
# Shared version for the tools. For a release, update it here and add notes in docs/release-notes/.
project(hsf VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Reject unsupported compilers. Windows builds require clang-cl or MSVC. On 32-bit x86, GCC and Clang use x87 floating
# point with extra precision. That would make the output differ from other builds. clang-cl and MSVC use SSE2 there.
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(compiler_name GCC)
else()
    set(compiler_name ${CMAKE_CXX_COMPILER_ID})
endif()
if (WIN32 AND NOT MSVC)
    message(FATAL_ERROR "HSF doesn't support ${compiler_name} on Windows, such as MinGW's GCC or the GNU-style "
                        "clang++. Build with Visual Studio's clang-cl (cmake -T ClangCL) or with MSVC; README.md has "
                        "the steps.")
endif()
if (NOT MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(i.86|x86|x86_64|AMD64)$")
    message(FATAL_ERROR "HSF doesn't support 32-bit x86 builds with ${compiler_name}: their output would differ from "
                        "other builds. Build for 64-bit instead, or on Windows use clang-cl or MSVC. They build "
                        "32-bit x86 with SSE2.")
endif()
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
# MSVC: the static C runtime (also in subprojects that declare an older minimum CMake version). With it, a plugin
# requires no runtime DLLs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
# clang-cl takes MSVC's options but is Clang underneath. clang-cl gets Clang's settings below through /clang:, and
# ignores MSVC's /d1trimfile.
if (MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(HSF_CLANG_CL ON)
endif()
# Output must not depend on the platform: no fused multiply-add contraction (clang contracts by default where the CPU
# has FMA, such as Apple Silicon). Then every build computes the same floats. MSVC's /fp:precise doesn't contract.
if (HSF_CLANG_CL)
    add_compile_options(/clang:-ffp-contract=off)
    # clang-cl does 32-bit x86 floating point on the x87 unit unless it's given SSE2. MSVC uses SSE2 there by default.
    if (CMAKE_SIZEOF_VOID_P EQUAL 4)
        add_compile_options(/arch:SSE2)
    endif()
elseif (NOT MSVC)
    add_compile_options(-ffp-contract=off)
endif()
# __FILE__ gives paths relative to the source or build directory. Debug information keeps absolute paths. Debuggers then
# find the sources. MSVC's /d1trimfile isn't documented, but Visual Studio 2019 16.4 and later have it.
if (HSF_CLANG_CL)
    add_compile_options("/clang:-fmacro-prefix-map=${CMAKE_SOURCE_DIR}/="
                        "/clang:-fmacro-prefix-map=${CMAKE_BINARY_DIR}/=")
elseif (MSVC)
    cmake_path(NATIVE_PATH CMAKE_SOURCE_DIR source_dir)
    cmake_path(NATIVE_PATH CMAKE_BINARY_DIR binary_dir)
    add_compile_options(/d1trimfile:${source_dir} /d1trimfile:${binary_dir})
else()
    add_compile_options(-fmacro-prefix-map=${CMAKE_SOURCE_DIR}/= -fmacro-prefix-map=${CMAKE_BINARY_DIR}/=)
endif()

if (MSVC)
    set(HSF_WARNINGS /W4)
    # fopen and freopen are standard C++. The _s versions in MSVC's warnings aren't portable.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    set(HSF_WARNINGS -Wall -Wextra)
endif()

# Zstandard, for Bezel Engine's archives (.bea). The system's copy is used where there's one. Otherwise Zstandard is
# downloaded and built along with HSF. A universal macOS build (more than one architecture in CMAKE_OSX_ARCHITECTURES)
# always builds it. Installed copies, such as Homebrew's copy, are built for the Mac's architecture only.
include(FetchContent)
list(LENGTH CMAKE_OSX_ARCHITECTURES osx_architectures)

if (osx_architectures LESS 2)
    find_package(zstd CONFIG QUIET)
endif()
if (TARGET zstd::libzstd)
    set(HSF_ZSTD zstd::libzstd)
else()
    FetchContent_Declare(zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG v1.5.7
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR build/cmake)
    foreach (option ZSTD_BUILD_PROGRAMS ZSTD_BUILD_TESTS ZSTD_BUILD_SHARED ZSTD_LEGACY_SUPPORT ZSTD_MULTITHREAD_SUPPORT)
        set(${option} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zstd)
    set_property(DIRECTORY ${zstd_SOURCE_DIR}/build/cmake PROPERTY EXCLUDE_FROM_ALL TRUE)
    # Only in the build: zstd's project installs the target. An installed target can't name the build folder.
    target_include_directories(libzstd_static INTERFACE $<BUILD_INTERFACE:${zstd_SOURCE_DIR}/lib>)
    set(HSF_ZSTD libzstd_static)
endif()

# zlib, for the PSF container's compressed program. The system's copy is used where there's one. Otherwise zlib is
# downloaded and built along with HSF. macOS has zlib for every architecture.
find_package(ZLIB QUIET)
if (NOT ZLIB_FOUND)
    FetchContent_Declare(zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG v1.3.1
        GIT_SHALLOW TRUE)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zlib)
    # Build only the static library. zlib's DLL would be built by default too. The DLL's resource script doesn't compile
    # with clang-cl.
    set_property(DIRECTORY ${zlib_SOURCE_DIR} PROPERTY EXCLUDE_FROM_ALL TRUE)
    target_include_directories(zlibstatic PUBLIC ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()

# Small helpers the other libraries share (byte order, ASCII lowercasing), and the include directory for all of src.
add_library(hsf_common INTERFACE)
target_sources(hsf_common INTERFACE src/common/ascii.h src/common/bytes.h)
target_include_directories(hsf_common INTERFACE src)

# The NintendoWare sound formats the Switch uses: FSAR, FSEQ, FBNK, FWAR, FWAV and FGRP.
add_library(hsf_fsar STATIC
    src/fsar/binary.h
    src/fsar/formats.cpp
    src/fsar/formats.h
)
target_link_libraries(hsf_fsar PUBLIC hsf_common)
target_compile_options(hsf_fsar PRIVATE ${HSF_WARNINGS})

# The model of the Switch's audio renderer. nn::atk's voices play through it.
add_library(hsf_audio STATIC
    src/audio/dsp.cpp
    src/audio/dsp.h
    src/audio/i3dl2.cpp
    src/audio/i3dl2.h
    src/audio/renderer.cpp
    src/audio/renderer.h
    src/audio/tables.h
    src/audio/voice.cpp
    src/audio/voice.h
)
target_link_libraries(hsf_audio PUBLIC hsf_common)
target_compile_options(hsf_audio PRIVATE ${HSF_WARNINGS})

# Archive mode's model of the Switch's sound library, nn::atk, on the model of the renderer.
add_library(hsf_atk STATIC
    src/atk/channel.cpp
    src/atk/channel.h
    src/atk/effect.cpp
    src/atk/effect.h
    src/atk/engine.cpp
    src/atk/engine.h
    src/atk/envelope.cpp
    src/atk/envelope.h
    src/atk/hardware.h
    src/atk/low_level_voice.cpp
    src/atk/low_level_voice.h
    src/atk/sequence.cpp
    src/atk/sequence.h
    src/atk/tables.h
    src/atk/util.cpp
    src/atk/util.h
    src/atk/voice.cpp
    src/atk/voice.h
)
target_link_libraries(hsf_atk PUBLIC hsf_audio hsf_fsar)
target_compile_options(hsf_atk PRIVATE ${HSF_WARNINGS})

# Switch games' files: folders, and Bezel Engine's archives (.bea) and data sheets.
add_library(hsf_game STATIC
    src/game/bea.cpp
    src/game/bea.h
    src/game/game.cpp
    src/game/game.h
    src/game/sheet.cpp
    src/game/sheet.h
)
target_link_libraries(hsf_game PUBLIC hsf_common PRIVATE ${HSF_ZSTD})
target_compile_options(hsf_game PRIVATE ${HSF_WARNINGS})

# The HSF file format: the PSF container, chunks, the archive descriptor and tags, and loading a set with its libraries.
add_library(hsf_format STATIC
    src/hsf/format.cpp
    src/hsf/format.h
)
target_link_libraries(hsf_format PUBLIC hsf_common PRIVATE ZLIB::ZLIB)
target_compile_options(hsf_format PRIVATE ${HSF_WARNINGS})

# The player: a set's sound on the model, its playback with lengths, fades and seeking, and rendering ahead on a worker
# thread for the foobar2000 component.
add_library(hsf_player STATIC
    src/hsf/playback.cpp
    src/hsf/playback.h
    src/hsf/player.cpp
    src/hsf/player.h
    src/hsf/render_ahead.cpp
    src/hsf/render_ahead.h
)
find_package(Threads REQUIRED)
target_link_libraries(hsf_player PUBLIC hsf_format hsf_atk Threads::Threads)
target_compile_options(hsf_player PRIVATE ${HSF_WARNINGS})

add_library(hsf_rip STATIC
    src/rip/rip.cpp
    src/rip/rip.h
)
target_link_libraries(hsf_rip PUBLIC hsf_format hsf_atk)
target_compile_options(hsf_rip PRIVATE ${HSF_WARNINGS})

# Sets HSF_VERSION and, on Windows, adds the version resource shown in Explorer's file properties.
function(hsf_add_version target description)
    target_compile_definitions(${target} PRIVATE HSF_VERSION="${hsf_VERSION}")
    if (WIN32)
        get_target_property(type ${target} TYPE)
        if (type STREQUAL "EXECUTABLE")
            set(VERSION_FILE_TYPE VFT_APP)
            set(VERSION_FILE_NAME ${target}${CMAKE_EXECUTABLE_SUFFIX})
        else()
            set(VERSION_FILE_TYPE VFT_DLL)
            set(VERSION_FILE_NAME ${target}${CMAKE_SHARED_LIBRARY_SUFFIX})
        endif()
        set(VERSION_DESCRIPTION ${description})
        set(VERSION_NAME ${target})
        set(rc ${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc)
        configure_file(${CMAKE_CURRENT_FUNCTION_LIST_DIR}/platform/windows/version.rc.in ${rc} @ONLY)
        target_sources(${target} PRIVATE ${rc})
    endif()
endfunction()

# WAV/FLAC output and the console for the command-line tools. libFLAC comes from its CMake package (vcpkg and other
# CMake-built installs), or else from pkg-config (Homebrew and Linux distributions). When neither has it, libFLAC is
# fetched and built from source. A universal macOS build (more than one architecture in CMAKE_OSX_ARCHITECTURES) always
# builds it. An installed libFLAC (Homebrew's build, for example) is built for the Mac's architecture only.
add_library(hsf_cli STATIC src/cli/audio_out.cpp src/cli/audio_out.h src/cli/console.cpp src/cli/console.h)
target_link_libraries(hsf_cli PUBLIC hsf_common)
target_compile_options(hsf_cli PRIVATE ${HSF_WARNINGS})
if (osx_architectures LESS 2)
    find_package(FLAC CONFIG QUIET)
    if (FLAC_FOUND)
        set(HSF_FLAC FLAC::FLAC)
    else()
        find_package(PkgConfig QUIET)
        if (PKG_CONFIG_FOUND)
            pkg_check_modules(FLAC QUIET IMPORTED_TARGET flac)
        endif()
        if (FLAC_FOUND)
            set(HSF_FLAC PkgConfig::FLAC)
        endif()
    endif()
endif()
if (NOT HSF_FLAC)
    FetchContent_Declare(flac
        GIT_REPOSITORY https://github.com/xiph/flac.git
        GIT_TAG 1.5.0
        GIT_SHALLOW TRUE)
    foreach (option BUILD_CXXLIBS BUILD_PROGRAMS BUILD_EXAMPLES BUILD_TESTING BUILD_DOCS INSTALL_MANPAGES
                    INSTALL_PKGCONFIG_MODULES INSTALL_CMAKE_CONFIG_MODULE WITH_OGG)
        set(${option} OFF CACHE BOOL "" FORCE)
    endforeach()
    FetchContent_MakeAvailable(flac)
    # Build only libFLAC: the helper libraries of flac's programs would be built by default too.
    set_property(DIRECTORY ${flac_SOURCE_DIR} PROPERTY EXCLUDE_FROM_ALL TRUE)
    set(HSF_FLAC FLAC::FLAC)
endif()
target_link_libraries(hsf_cli PRIVATE ${HSF_FLAC})

# hsfrip rips on several threads (--jobs). glibc before 2.34 keeps threads in a separate library.
add_executable(hsfrip src/cli/hsfrip.cpp)
target_link_libraries(hsfrip PRIVATE hsf_rip hsf_game hsf_cli Threads::Threads)
target_compile_options(hsfrip PRIVATE ${HSF_WARNINGS})
hsf_add_version(hsfrip "HSF ripper")

add_executable(hsfplay src/cli/hsfplay.cpp)
target_link_libraries(hsfplay PRIVATE hsf_player hsf_cli)
target_compile_options(hsfplay PRIVATE ${HSF_WARNINGS})
hsf_add_version(hsfplay "HSF renderer")

foreach (tool hsfrip hsfplay)
    # Windows: UTF-8 as the tools' code page. The tools can then take and open paths outside the system's ANSI code
    # page, such as the paths of files dropped on them. The linker merges the manifest into the one it makes.
    if (MSVC)
        target_sources(${tool} PRIVATE platform/windows/utf8.manifest)
    endif()

    # macOS: a droplet app for each tool (hsfrip.app, hsfplay.app). Files can be dropped on the app in the Finder. Each
    # app is an AppleScript applet with the tool inside, in Contents/Resources, and the version in its Info.plist.
    if (APPLE)
        find_program(OSACOMPILE osacompile)
        find_program(CODESIGN codesign)
        find_program(PLUTIL plutil)
        if (OSACOMPILE AND CODESIGN AND PLUTIL)
            set(DROPLET_TOOL ${tool})
            set(droplet_app ${CMAKE_BINARY_DIR}/${tool}.app)
            set(droplet_script ${CMAKE_BINARY_DIR}/${tool}.applescript)
            configure_file(platform/macos/droplet.applescript.in ${droplet_script} @ONLY)
            add_custom_command(
                OUTPUT ${droplet_app}/Contents/Resources/${tool}
                COMMAND ${CMAKE_COMMAND} -E rm -rf ${droplet_app}
                COMMAND ${OSACOMPILE} -o ${droplet_app} ${droplet_script}
                COMMAND ${PLUTIL} -replace CFBundleShortVersionString -string ${hsf_VERSION}
                        ${droplet_app}/Contents/Info.plist
                COMMAND ${PLUTIL} -replace CFBundleVersion -string ${hsf_VERSION} ${droplet_app}/Contents/Info.plist
                COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${tool}> ${droplet_app}/Contents/Resources/${tool}
                COMMAND ${CODESIGN} --force --deep --sign - ${droplet_app}
                DEPENDS ${tool} ${droplet_script}
                COMMENT "Building ${tool}.app"
                VERBATIM)
            add_custom_target(${tool}_app ALL DEPENDS ${droplet_app}/Contents/Resources/${tool})
        endif()
    endif()
endforeach()

# foobar2000 component (Windows with clang-cl or MSVC, or macOS): cmake -DFOOBAR2000_SDK_DIR=<unpacked SDK> ...
set(FOOBAR2000_SDK_DIR "" CACHE PATH "foobar2000 SDK (builds the foo_input_hsf component when set)")
if (FOOBAR2000_SDK_DIR)
    if (NOT MSVC AND NOT APPLE)
        message(FATAL_ERROR "the foobar2000 component requires clang-cl or MSVC on Windows, or macOS")
    endif()
    add_subdirectory(plugins/foobar2000)
endif()

add_executable(atk_test tests/atk_test.cpp tests/check.h)
target_link_libraries(atk_test PRIVATE hsf_atk)
target_compile_options(atk_test PRIVATE ${HSF_WARNINGS})

add_executable(atk_engine_test tests/atk_engine_test.cpp tests/check.h tests/fsar_files.h)
target_link_libraries(atk_engine_test PRIVATE hsf_atk)
target_compile_options(atk_engine_test PRIVATE ${HSF_WARNINGS})

add_executable(audio_test tests/audio_test.cpp tests/check.h)
target_link_libraries(audio_test PRIVATE hsf_audio)
target_compile_options(audio_test PRIVATE ${HSF_WARNINGS})

# The audio writer's test decodes the FLAC files it writes. For that, the test requires libFLAC's decoder too.
add_executable(audio_out_test tests/audio_out_test.cpp tests/check.h)
target_link_libraries(audio_out_test PRIVATE hsf_cli ${HSF_FLAC})
target_compile_options(audio_out_test PRIVATE ${HSF_WARNINGS})

add_executable(format_test tests/format_test.cpp tests/check.h)
target_link_libraries(format_test PRIVATE hsf_format ZLIB::ZLIB)
target_compile_options(format_test PRIVATE ${HSF_WARNINGS})

add_executable(fsar_test tests/fsar_test.cpp tests/check.h tests/fsar_files.h)
target_link_libraries(fsar_test PRIVATE hsf_fsar)
target_compile_options(fsar_test PRIVATE ${HSF_WARNINGS})

add_executable(game_test tests/game_test.cpp tests/check.h)
target_link_libraries(game_test PRIVATE hsf_game ${HSF_ZSTD})
target_compile_options(game_test PRIVATE ${HSF_WARNINGS})

add_executable(player_test tests/player_test.cpp tests/check.h tests/fsar_files.h)
target_link_libraries(player_test PRIVATE hsf_player)
target_compile_options(player_test PRIVATE ${HSF_WARNINGS})

add_executable(render_ahead_test tests/render_ahead_test.cpp tests/check.h)
target_link_libraries(render_ahead_test PRIVATE hsf_player)
target_compile_options(render_ahead_test PRIVATE ${HSF_WARNINGS})

add_executable(rip_test tests/rip_test.cpp tests/check.h tests/fsar_files.h)
target_link_libraries(rip_test PRIVATE hsf_rip)
target_compile_options(rip_test PRIVATE ${HSF_WARNINGS})

enable_testing()
add_test(NAME atk COMMAND atk_test)
add_test(NAME "atk engine" COMMAND atk_engine_test)
add_test(NAME audio COMMAND audio_test)
add_test(NAME audio_out COMMAND audio_out_test)
add_test(NAME format COMMAND format_test)
add_test(NAME fsar COMMAND fsar_test)
add_test(NAME game COMMAND game_test)
add_test(NAME player COMMAND player_test)
add_test(NAME render_ahead COMMAND render_ahead_test)
add_test(NAME rip COMMAND rip_test)

# The tools report the version they were built as.
string(REPLACE "." "\\." version_pattern ${hsf_VERSION})
foreach (tool hsfrip hsfplay)
    add_test(NAME version_${tool} COMMAND ${tool} --version)
    set_tests_properties(version_${tool} PROPERTIES PASS_REGULAR_EXPRESSION "^${tool} ${version_pattern}[\r\n]*$")
endforeach()

# Load local_tests.cmake from each immediate subdirectory to register tests that use locally supplied files.
file(GLOB local_tests CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/*/local_tests.cmake)
foreach (file IN LISTS local_tests)
    include(${file})
endforeach()
