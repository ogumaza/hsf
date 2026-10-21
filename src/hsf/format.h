// SPDX-License-Identifier: MIT

// The HSF file format (docs/hsf.md): the PSF container, program chunks (FILE and SND), the archive descriptor, and
// tags.

#pragma once

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

// The reserved area of a set: the sound archive is played by ntdlib's model of the SDK's sound library (nn::atk) and of
// the console's audio renderer, with the effects a scene of the game puts on the aux buses (docs/hsf.md, "Archive
// descriptor").
struct ArchiveDescriptor
{
    static constexpr uint32_t kMagic = 0x41465348; // "HSFA"
    static constexpr uint32_t kVersion = 1;

    // nn::atk's voice count when the game doesn't change it, and the most a version 1 player has.
    static constexpr uint32_t kDefaultVoices = 96;

    static std::optional<ArchiveDescriptor> Parse(const std::vector<uint8_t>& data);

    // Returns an error message for an unsupported descriptor, or no value if it is supported.
    std::optional<std::string> Check() const;

    std::vector<uint8_t> Serialize() const;

    uint32_t version = kVersion;
    uint32_t flags = 0;
    uint32_t output_mode = kOutputStereo;
    uint32_t voice_count = kDefaultVoices;
    uint32_t sdk_version = 0; // major * 10000 + minor * 100 + micro, or 0 if unknown
    std::vector<AuxEffect> effects;
    std::string archive_path;             // FILE chunk holding the sound archive (.bfsar)
    std::vector<std::string> group_paths; // FILE chunks holding group files (.bfgrp) the game keeps outside it
};

// The program's chunks in the streaming formats' layout (xsflib/psf/streaming.h): FILE chunks, each a file of the game
// by its path in the game's files, and the SND chunk, the nn::atk item id of the sound to play (docs/hsf.md, "Program:
// chunks"). The builder compresses the program as it writes it.
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
// for a set a version 1 player can't play: one without a descriptor or a sound, or whose descriptor fails its Check().
using LoadedSet = xsflib::psf::LoadedSet<SetFormat>;
using xsflib::psf::LoadSet;

} // namespace hsf
