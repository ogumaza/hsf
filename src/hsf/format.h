// SPDX-License-Identifier: MIT

// The HSF file format (docs/hsf.md): the PSF container, program chunks (FILE and SND), the archive descriptor, and
// tags.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/psf/program.h"
#include "xsflib/psf/psf.h"
#include "xsflib/psf/set.h"
#include "xsflib/psf/streaming.h"

namespace hsf
{

// PSF version byte of HSF. See docs/hsf.md ("Version byte") before changing it.
constexpr uint8_t kPsfVersionHsf = 0x29;

// The output's sample rate: 48,000 Hz exactly. Players report it, and the `length` and `fade` tags count at it
// (docs/hsf.md, "Tags").
constexpr int kPlayerSampleRate = 48'000;

// Output modes: 0 mono, 1 stereo, 2 surround. Version 1 plays stereo only.
constexpr uint32_t kOutputStereo = 1;

// The PSF container that the players share (xsflib::psf): tags, and reading and writing PSF files.
using xsflib::psf::FileReader;
using xsflib::psf::Tags;
using xsflib::psf::WritePsf;

// An effect on one of the aux buses of the voices' mix, in the order they're put there (docs/hsf.md, "Aux effects").
struct AuxEffect
{
    // The reference game's I3DL2 reverb on an aux effect: its 12 parameters in the order of nn::audio's
    // I3dl2ReverbParameterSet.
    static constexpr uint32_t kI3dl2Reverb = 1;
    static constexpr uint32_t kI3dl2Parameters = 12;

    uint32_t bus = 0; // 0 to 2: aux buses A to C
    uint32_t type = kI3dl2Reverb;
    std::vector<float> parameters;
};

// An FX line of JAudio's DSP program, set up by the game's code (docs/hsf.md, "Version 2: JAudio").
struct JaudioFxLine
{
    uint16_t flags = 0;               // 0 for a line that's off
    uint16_t subframes = 0;           // the delay's length, in sub-frames of 80 samples
    std::array<uint16_t, 2> buses{};  // the DSP's addresses of the two buses that the line feeds
    std::array<uint16_t, 2> levels{}; // the line's level into each, 0x8000 for 1
    std::array<int16_t, 8> filter{};  // the filter's coefficients

    bool operator==(const JaudioFxLine&) const = default;
};

// The states that the game's code gives the first 16 child tracks of a piece of music as the piece starts, each 0 to
// 15 (docs/hsf.md, "Version 2: JAudio").
struct JaudioTrackStates
{
    uint32_t sound = 0; // the sound's ID
    std::array<uint8_t, 16> states{};

    bool operator==(const JaudioTrackStates&) const = default;
};

// A volume that the game's code writes over a sound's volume in the sound table as it starts (docs/hsf.md, "Version
// 2: JAudio").
struct JaudioSoundVolume
{
    uint32_t sound = 0;  // the sound's ID
    uint32_t volume = 0; // 0 to 255, the value of the table's byte

    bool operator==(const JaudioSoundVolume&) const = default;
};

// What a version 2 descriptor gives the model of JAudio: the library's build, and what the game's code sets up for its
// sounds (docs/hsf.md, "Version 2: JAudio").
struct JaudioSettings
{
    // JAudio2 as the Super Mario Galaxy games' Switch releases build it, on Hagi's emulation of the DSP program.
    static constexpr uint32_t kGalaxyLibrary = 1;

    static constexpr std::size_t kCategories = 16;
    static constexpr std::size_t kFxLines = 4;

    // The longest delay of an FX line, in sub-frames: the 0x5000 bytes of each line's buffer.
    static constexpr uint16_t kMaxSubframes = 128;

    uint32_t library = kGalaxyLibrary;
    uint32_t dsp_level = 0x4000; // the DSP's level, in 4.12 for 4 times the game's level
    float music_volume = 1.0f;
    float effect_volume = 1.0f;
    std::array<float, kCategories> category_volumes{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                                    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    std::array<JaudioFxLine, kFxLines> fx_lines{};
    std::vector<JaudioTrackStates> track_states;
    std::vector<JaudioSoundVolume> sound_volumes;

    bool operator==(const JaudioSettings&) const = default;
};

// The reserved area of a set (docs/hsf.md, "Archive descriptor"). In version 1, the sound archive is played by
// ntdlib's model of the SDK's sound library (nn::atk) and of the console's audio renderer, with the effects a scene of
// the game puts on the aux buses. In version 2, a game's JAudio files are played by HSF's model of JAudio and of the
// emulator that runs JAudio's DSP program in the game.
struct ArchiveDescriptor
{
    static constexpr uint32_t kMagic = 0x41465348; // "HSFA"
    static constexpr uint32_t kVersion = 1;
    static constexpr uint32_t kJaudioVersion = 2;

    // nn::atk's voice count when the game doesn't change it, and the most a version 1 player has.
    static constexpr uint32_t kDefaultVoices = 96;

    // The voice count of version 2: the DSP program's 64 channels.
    static constexpr uint32_t kJaudioVoices = 64;

    // Version 2's group paths: the sequence archive, then the folder of the wave archives.
    static constexpr std::size_t kSequenceArchive = 0;
    static constexpr std::size_t kWaveFolder = 1;

    static std::optional<ArchiveDescriptor> Parse(const std::vector<uint8_t>& data);

    // Returns an error message for an unsupported descriptor, or no value if it is supported.
    std::optional<std::string> Check() const;

    std::vector<uint8_t> Serialize() const;

    // A version 2 descriptor for JAudio, with `settings`: 64 voices and no aux effects.
    static ArchiveDescriptor Jaudio(const JaudioSettings& settings);

    uint32_t version = kVersion;
    uint32_t flags = 0;
    uint32_t output_mode = kOutputStereo;
    uint32_t voice_count = kDefaultVoices;
    uint32_t sdk_version = 0; // major * 10000 + minor * 100 + micro, or 0 if unknown
    std::vector<AuxEffect> effects;

    // Version 1: the FILE chunks holding the sound archive (.bfsar) and the group files (.bfgrp) the game keeps outside
    // it. Version 2: the start-up archive, then the sequence archive and the folder of the wave archives.
    std::string archive_path;
    std::vector<std::string> group_paths;

    JaudioSettings jaudio; // version 2
};

// The program's chunks in the streaming formats' layout (xsflib/psf/streaming.h): FILE chunks, each a file of the game
// by its path in the game's files, and the SND chunk, the sound to play: an nn::atk item id in version 1, a JAudio
// sound ID in version 2 (docs/hsf.md, "Program: chunks"). The builder compresses the program as it writes it.
using xsflib::psf::FileChunk;
using ProgramBuilder = xsflib::psf::StreamingProgramBuilder;

// HSF's traits for xsflib's sets (xsflib/psf/set.h): archive mode only, each file held once, and a sound in every set.
struct SetFormat
{
    using ProcessDescriptor = void;
    using ArchiveDescriptor = hsf::ArchiveDescriptor;

    static constexpr uint8_t kPsfVersion = kPsfVersionHsf;
    static constexpr const char* kFile = "an HSF file";
    static constexpr bool kStreaming = true;
};

// A complete set: the files of a file and its libraries in loading order, the descriptor, the sound, and the tags of
// the top-level file. LoadSet loads an HSF or miniHSF file with its _lib and _libN chain. Paths in _lib tags are
// relative to the directory of the file that names them: its path up to the last of `separators`. It returns an error
// for a set the player can't play: one without a descriptor or a sound, or whose descriptor fails its Check().
using LoadedSet = xsflib::psf::LoadedSet<SetFormat>;
using xsflib::psf::LoadSet;

} // namespace hsf
