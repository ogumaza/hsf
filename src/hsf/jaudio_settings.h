// SPDX-License-Identifier: MIT

// What a version 2 archive descriptor gives the model of JAudio (JaudioSettings), with the fields of libraries 2 and 3
// (docs/hsf.md, "Version 2: JAudio"). format.h includes it.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hsf
{

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

// A piece of music of a library 2 descriptor, and what the game's code sets up for it (docs/hsf.md, "Library 2:
// Pikmin 2").
struct JaudioPiece
{
    // The piece's classes in the game's sound code: 0 to 3. A piece of class 3 asks a conductor what to play.
    static constexpr uint8_t kClasses = 4;
    static constexpr uint8_t kConducted = 3;

    uint16_t file = 0;          // the piece's file in the sequence archive, by its number
    uint8_t kind = 0;           // the piece's class
    uint8_t volume = 50;        // the piece's volume in the game's list of its music, 0 to 127
    uint8_t kept_levels = 0x1f; // the levels that a child track keeps when it asks for them, bits 0 to 4
    uint16_t beat_interval = 60;
    uint16_t muted_tracks = 0; // the child tracks that the game's directors mute as the tracks start, a bit each
    uint16_t start_point = 0;  // the section that a piece of class 2 starts at
    std::string conductor;     // the conductor's file in the conductor archive: for class 3, and empty otherwise

    bool operator==(const JaudioPiece&) const = default;
};

// A sound that starts after the sequences' generator of random numbers has drawn some numbers. The game's generator
// can be in that state as the sound starts (docs/hsf.md, "Library 2: Pikmin 2").
struct JaudioDraws
{
    static constexpr uint32_t kMaxDraws = 0xffff;

    uint32_t sound = 0; // the sound's ID
    uint32_t draws = 0; // the numbers drawn before the sound starts, 1 to kMaxDraws

    bool operator==(const JaudioDraws&) const = default;
};

// The parts of a library 2 or 3 sound's ID: the section in the top 2 bits, 14 bits that are 0, and the sound's place in
// its section's list in the low 16.
constexpr uint32_t kSoundSectionMask = 0xc0000000;
constexpr uint32_t kSoundZeroMask = 0x3fff0000;
constexpr uint32_t kSoundPlaceMask = 0xffff;

// What a library 2 descriptor adds to the JAudio settings (docs/hsf.md, "Library 2: Pikmin 2").
struct JaudioLibrary2
{
    // A piece's sound ID: section 2 in the top 2 bits, and the piece's place in `pieces` in the low 16.
    static constexpr uint32_t kPieceSection = 0x80000000;
    static constexpr std::size_t kMaxPieces = 0x10000;

    uint16_t effect_sequence = 0;         // the file of the sequence archive that plays the effects, by its number
    uint8_t effect_sequence_volume = 127; // its volume in the game's list of its music, 0 to 127
    std::string conductor_archive;        // the FILE chunk that holds the conductor archive, or empty for none
    std::vector<JaudioPiece> pieces;
    std::vector<JaudioDraws> draws; // the sounds that start after draws, each sound once

    // The numbers that the sequences' generator draws before `sound` starts.
    uint32_t DrawsBefore(uint32_t sound) const
    {
        for (const JaudioDraws& d : draws)
        {
            if (d.sound == sound)
            {
                return d.draws;
            }
        }

        return 0;
    }

    bool operator==(const JaudioLibrary2&) const = default;
};

// A wave scene of library 3: a scene of one of the bank file's wave systems (docs/hsf.md, "Library 3: Pikmin").
struct JacScene
{
    uint16_t system = 0; // the wave system, by its place in the bank file
    uint16_t scene = 0;  // the scene, by its place in the system

    bool operator==(const JacScene&) const = default;
};

// A wave scene that the game loads as it starts. A scene that's set becomes its system's current scene.
struct JacLoad
{
    JacScene scene;
    uint32_t set = 0; // 1 if the game sets the scene, or 0

    bool operator==(const JacLoad&) const = default;
};

// A piece of music of a library 3 descriptor, and the levels and the waves that the game's code gives it.
struct JacPiece
{
    uint16_t file = 0;                // the piece's file in the sequence archive, by its number, and the piece's bank
    uint16_t playing_tracks = 0xffff; // the child tracks that play, a bit each
    float volume = 0.5f;              // the volume of the child tracks that play
    JacScene scene;                   // the wave scene that the piece sets

    bool operator==(const JacPiece&) const = default;
};

// A piece of the cutscenes' music: a number that the game writes to a port of the cutscenes' sequence.
struct JacPart
{
    uint16_t number = 0;
    JacScene scene; // the wave scene that the piece sets

    bool operator==(const JacPart&) const = default;
};

// An effect of a library 3 descriptor, by the way that the game's code starts it.
struct JacEffect
{
    // The kinds: the system's effects, the captain's, the Pikmin's, an action of an event, the formation's sound, the
    // crowd of Pikmin, a footstep and a cutscene's sound.
    static constexpr uint8_t kSystem = 0;
    static constexpr uint8_t kCaptain = 1;
    static constexpr uint8_t kPikmin = 2;
    static constexpr uint8_t kEvent = 3;
    static constexpr uint8_t kFormation = 4;
    static constexpr uint8_t kCrowd = 5;
    static constexpr uint8_t kWalk = 6;
    static constexpr uint8_t kCutscene = 7;
    static constexpr uint8_t kKinds = 8;

    // An event's types: 1 to kEventTypes.
    static constexpr uint8_t kEventTypes = 7;

    // A cutscene's type when the game writes the cutscene's number to port 1 too.
    static constexpr uint8_t kCutscenePart = 1;

    // A value that the effect doesn't write.
    static constexpr uint16_t kNone = 0xffff;

    uint8_t kind = kSystem;
    uint8_t type = 0;       // an event's type for kind 3, a cutscene's for kind 7, and 0 otherwise
    uint16_t value = 0;     // what the effect writes to its sequence's port
    uint16_t first = kNone; // what it writes first: as an event is created, to the captain's port 1, or a cutscene
    uint16_t finish = 0;    // a cutscene's sound: the game's frames from the sound to the cutscene's finish, or 0

    bool operator==(const JacEffect&) const = default;
};

// What a library 3 descriptor adds to the JAudio settings (docs/hsf.md, "Library 3: Pikmin").
struct JaudioLibrary3
{
    // A sound's ID: the section in the top 2 bits, and the sound's place in its list in the low 16.
    static constexpr uint32_t kEffectSection = 0;
    static constexpr uint32_t kPartSection = 0x40000000;
    static constexpr uint32_t kPieceSection = 0x80000000;
    static constexpr std::size_t kMaxEntries = 0x10000;

    // The sequences that the game starts on the first handles as it starts.
    static constexpr std::size_t kStartupHandles = 4;

    // The most that a level of a port holds.
    static constexpr uint16_t kMaxLevel = 0x7fff;

    std::string sequence_index; // the FILE chunk that holds the sequence index
    std::array<uint16_t, kStartupHandles> startup_files{0, 1, 13, 2};
    uint16_t effect_level = 0;   // what the game writes to port 0 of the effects' sequences
    uint16_t cutscene_level = 0; // what it writes to port 2 of the cutscenes' sequence
    float music_volume = 1.0f;   // the root track's volume in a piece of music
    std::vector<JacLoad> loads;  // in the game's order
    std::vector<JacPiece> pieces;
    std::vector<JacPart> parts;
    std::vector<JacEffect> effects;

    bool operator==(const JaudioLibrary3&) const = default;
};

// What a version 2 descriptor gives the model of JAudio: the library's build, and what the game's code sets up for its
// sounds (docs/hsf.md, "Version 2: JAudio").
struct JaudioSettings
{
    // JAudio2 as the Super Mario Galaxy games' Switch releases build it, on Hagi's emulation of the DSP program.
    static constexpr uint32_t kGalaxyLibrary = 1;

    // JAudio1 as the Switch release of Pikmin 2 builds it, on Hagi's emulation of the DSP program.
    static constexpr uint32_t kPikmin2Library = 2;

    // The C JAudio as the Switch release of Pikmin builds it, on Hagi's emulation of the DSP program.
    static constexpr uint32_t kPikminLibrary = 3;

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
    JaudioLibrary2 library2; // library 2
    JaudioLibrary3 library3; // library 3

    bool operator==(const JaudioSettings&) const = default;
};

} // namespace hsf
