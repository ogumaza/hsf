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

#include "xsflib/psf/descriptor.h"
#include "xsflib/psf/program.h"
#include "xsflib/psf/psf.h"
#include "xsflib/psf/set.h"
#include "xsflib/psf/streaming.h"

#include "hsf/jaudio_settings.h"

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

    // The renderer's delay and light limiter. nn::atk sets them up. Each has 8 parameters (DelayParameter and
    // LightLimiterParameter). Their times are whole microseconds, up to 10 s.
    static constexpr uint32_t kDelay = 2;
    static constexpr uint32_t kLightLimiter = 3;
    static constexpr uint32_t kRendererEffectParameters = 8;
    static constexpr float kMostTime = 10'000'000.0f;

    // The renderer's reverb. nn::atk sets it up (EffectReverb). It has 12 parameters (ReverbParameter). Its pre-delay
    // is a whole number of microseconds up to 300 ms, and its decay time a whole number from 100 ms to 20 s.
    static constexpr uint32_t kReverb = 4;
    static constexpr uint32_t kReverbParameters = 12;
    static constexpr float kMostPredelay = 300'000.0f;
    static constexpr float kLeastDecay = 100'000.0f;
    static constexpr float kMostDecay = 20'000'000.0f;

    uint32_t bus = 0; // 0 to 2: aux buses A to C
    uint32_t type = kI3dl2Reverb;
    std::vector<float> parameters;
};

// A delay's parameters by their index: the longest delay and the delay in microseconds, the in, feedback and dry gains,
// the low-pass amount, the channel spread, and the channel mode (0 to 3 for 1, 2, 4 or 6 channels).
enum DelayParameter : std::size_t
{
    kDelayTimeMax,
    kDelayTime,
    kDelayInGain,
    kDelayFeedbackGain,
    kDelayDryGain,
    kDelayLowPassAmount,
    kDelayChannelSpread,
    kDelayChannelMode,
};

// A light limiter's parameters by their index: the longest look-ahead, the attack, the release and the look-ahead in
// microseconds, the threshold, the input and output gains, and the channel mode (as a delay's).
enum LightLimiterParameter : std::size_t
{
    kLimiterLookAheadTimeMax,
    kLimiterAttackTime,
    kLimiterReleaseTime,
    kLimiterLookAheadTime,
    kLimiterThreshold,
    kLimiterInputGain,
    kLimiterOutputGain,
    kLimiterChannelMode,
};

// A reverb's parameters by their index: the early reflections' mode (0 to 4) and gain, the pre-delay in microseconds,
// the late reverb's mode (0 to 4) and gain, the decay time in microseconds, the decay ratio at high frequencies, the
// coloration, the reverb's gain, the out gain, the dry gain, and the channel mode (as a delay's).
enum ReverbParameter : std::size_t
{
    kReverbEarlyMode,
    kReverbEarlyGain,
    kReverbPredelay,
    kReverbLateMode,
    kReverbLateGain,
    kReverbDecay,
    kReverbHighFrequencyDecayRatio,
    kReverbColoration,
    kReverbGain,
    kReverbOutGain,
    kReverbDryGain,
    kReverbChannelMode,
};

// What a version 3 descriptor gives the model of NW4R SND: the layer that the game puts in place of AX, and the build
// of NW4R SND (docs/hsf.md, "Version 3: NW4R SND").
struct Nw4rSettings
{
    // Skyward Sword HD's layer: AX's voices mixed by nn::audio's renderer (ntdlib::rvl::nx::Layer).
    static constexpr uint32_t kSkywardSwordLayer = 1;

    // The build of NW4R SND in Skyward Sword, by its date in the Wii release's main.dol. The HD release's code has no
    // date. ntdlib plays builds from September 24 2009 on as that build.
    static constexpr uint32_t kSkywardSwordBuild = 20111007;

    uint32_t layer = kSkywardSwordLayer;
    uint32_t snd_build = kSkywardSwordBuild; // NW4R SND's build, by its date (YYYYMMDD)

    bool operator==(const Nw4rSettings&) const = default;
};

// The volumes that the game's code gives a sound's 16 tracks through the sound's handle as the sound starts: nn::atk's
// SetTrackVolume, track 0 first (docs/hsf.md, "Track volumes"). 1 leaves a track as the sequence plays it. The sound is
// its nn::atk item ID.
using SoundTrackVolumes = xsflib::psf::SoundTrackVolumes;

// A step of a sound's scripted start (docs/hsf.md, "Scripted starts"): at a frame of the model, the start of a
// sequence sound, or a value for one of a sequence's variables. A variable has the sequence commands' number. Numbers 0
// to 15 are a sound's variables, 16 to 31 the global variables, and 32 to 47 a track's variables. Every sequence shares
// the global variables. A start can start a sound of an add-on archive (AddonArchive). A write of kind 1 takes effect
// before its frame. A write of kind 2 waits from its frame for the next call of a user procedure by the sound's
// sequence. It takes effect inside that call. A write of the game's procedure does the same.
struct ScriptStep
{
    static constexpr uint8_t kStart = 0;
    static constexpr uint8_t kWrite = 1;
    static constexpr uint8_t kWriteInProcedure = 2;

    static constexpr uint16_t kLocalEnd = 16;
    static constexpr uint16_t kGlobalEnd = 32;
    static constexpr uint16_t kTrackEnd = 48;

    // The archive of a start of a sound of the set's archive, and the track of a variable that isn't a track's.
    static constexpr uint16_t kNone = 0xFFFF;

    uint32_t frame = 0; // the model's frame, from 0, before which the step takes effect, or from which it waits
    uint8_t kind = kWrite;
    uint8_t procedure = 0; // the user procedure that a write of kind 2 waits for, and 0 for the other kinds

    // A write's variable, or a start's archive: kNone for the set's archive, or 0 to n-1 for the descriptor's add-on
    // archives.
    uint16_t variable = kNone;

    uint16_t track = kNone; // 0 to 15 for variables 32 to 47
    int16_t value = 0;      // 0 for a start

    // The nn::atk item ID of the sound that a start starts, in its archive, or of the sound whose variable or track's
    // variable a write sets. A write of kind 1 to a global variable has 0. A write of kind 2 names the sound whose
    // procedure call it waits for, for a global variable too. A script starts each item ID once. An item ID therefore
    // names one sound, whichever archive holds it.
    uint32_t sound = 0;

    // A start of `sound` at `frame`, from the set's archive or from add-on archive `archive`.
    static ScriptStep Start(uint32_t frame, uint32_t sound, uint16_t archive = kNone);

    // A write of `value` to `variable` at `frame`: of `sound`'s variable, of the global variable, or of track `track`'s
    // variable of `sound`.
    static ScriptStep Write(uint32_t frame, uint16_t variable, int16_t value, uint32_t sound = 0,
                            uint16_t track = kNone);

    // A write of `value` to `variable` inside the first call of user procedure `procedure` by `sound`'s sequence at
    // `frame` or later: of the sound's variable, of the global variable, or of track `track`'s variable of the sound.
    static ScriptStep WriteInProcedure(uint32_t frame, uint8_t procedure, uint16_t variable, int16_t value,
                                       uint32_t sound, uint16_t track = kNone);

    bool operator==(const ScriptStep&) const = default;
};

// An add-on sound archive that the scripts start sounds from (docs/hsf.md, "Scripted starts"): its path, and the paths
// of the group files that the game keeps outside it. The paths name FILE chunks, like the descriptor's other paths. The
// game plays the add-on's sounds in the same nn::atk sound system as the set's archive.
struct AddonArchive
{
    std::string archive_path;
    std::vector<std::string> group_paths;

    bool operator==(const AddonArchive&) const = default;
};

// How the game starts a sound (docs/hsf.md, "Scripted starts"): the steps that start it and the other sounds that play
// beside it, and set their variables, in frame order. The model runs `lead_in` frames before its output starts.
struct ScriptedStart
{
    uint32_t sound = 0; // the nn::atk item ID of the set's sound
    uint32_t lead_in = 0;
    std::vector<ScriptStep> steps;

    // The script of a sound that starts at once with no variables set: the sound's start at frame 0.
    static ScriptedStart Plain(uint32_t sound);

    bool operator==(const ScriptedStart&) const = default;
};

// A table that the game's handler of user procedures reads (UserProcedureHandler): its number, 0 to 15, and its 128
// values.
struct UserProcedureTable
{
    static constexpr uint32_t kTables = 16;
    static constexpr std::size_t kValues = 128;

    uint32_t number = 0;
    std::array<int16_t, kValues> values{};

    bool operator==(const UserProcedureTable&) const = default;
};

// The game's handler of the user procedures that its sequences call with the command 0xF0E0 (docs/hsf.md, "User
// procedures"). The handler is ntdlib's model of the game's code, by its number. Jamboree's handler reads tables that
// the game loads from its files.
struct UserProcedureHandler
{
    static constexpr uint32_t kNone = 0;
    static constexpr uint32_t kJamboree = 1;   // Super Mario Party Jamboree
    static constexpr uint32_t kSuperstars = 2; // Mario Party Superstars

    uint32_t handler = kNone;
    std::vector<UserProcedureTable> tables; // each number once, for kJamboree only

    bool operator==(const UserProcedureHandler&) const = default;
};

// The reserved area of a set (docs/hsf.md, "Archive descriptor"). In version 1, the sound archive is played by ntdlib's
// model of the SDK's sound library (nn::atk) and of the console's audio renderer, with the effects a scene of the game
// puts on the aux buses. In version 2, a game's JAudio files are played by ntdlib's model of JAudio and of the emulator
// that runs JAudio's DSP program in the game. In version 3, a Wii game's sound archive (RSAR) is played by ntdlib's
// model of NW4R SND, on the layer that the game's Switch release puts in place of AX.
struct ArchiveDescriptor
{
    static constexpr uint32_t kMagic = 0x41465348; // "HSFA"
    static constexpr uint32_t kVersion = 1;
    static constexpr uint32_t kJaudioVersion = 2;
    static constexpr uint32_t kNw4rVersion = 3;

    // nn::atk's voice count when the game doesn't change it, and the most a version 1 player has.
    static constexpr uint32_t kDefaultVoices = 96;

    // The voice count of version 2: the DSP program's 64 channels.
    static constexpr uint32_t kJaudioVoices = 64;

    // The voice count of version 3: AX's 64 voices on the layer.
    static constexpr uint32_t kNw4rVoices = 64;

    // Version 2's group paths: the sequence archive, then the folder of the wave archives.
    static constexpr std::size_t kSequenceArchive = 0;
    static constexpr std::size_t kWaveFolder = 1;

    // Version 1's flag for a descriptor that lists the track volumes of sounds (`track_volumes`). HSF 1.3.0 and older
    // players refuse a descriptor with it.
    static constexpr uint32_t kFlagTrackVolumes = 1;

    // Version 1's flag for a descriptor that lists the scripted starts of sounds (`scripts`). HSF 1.3.0 and older
    // players refuse a descriptor with it.
    static constexpr uint32_t kFlagScripts = 2;

    // Version 1's flag for a descriptor that names the game's handler of user procedures (`procedures`). HSF 1.3.0 and
    // older players refuse a descriptor with it.
    static constexpr uint32_t kFlagProcedures = 4;

    static std::optional<ArchiveDescriptor> Parse(const std::vector<uint8_t>& data);

    // A version 2 descriptor for JAudio, with `settings`: 64 voices and no aux effects.
    static ArchiveDescriptor Jaudio(const JaudioSettings& settings);

    // A version 3 descriptor for the sound archive at `archive_path`, with `settings`: 64 voices, no aux effects and
    // no group files, and the SDK version `sdk_version`.
    static ArchiveDescriptor Nw4r(const Nw4rSettings& settings, std::string archive_path, uint32_t sdk_version);

    // Returns an error message for an unsupported descriptor, or no value if it is supported.
    std::optional<std::string> Check() const;

    std::vector<uint8_t> Serialize() const;

    // The track volumes that the descriptor gives `sound`, an nn::atk item ID, or null.
    const SoundTrackVolumes* TrackVolumesOf(uint32_t sound) const;

    // The scripted start that the descriptor gives `sound`, an nn::atk item ID, or null.
    const ScriptedStart* ScriptOf(uint32_t sound) const;

    uint32_t version = kVersion;
    uint32_t flags = 0;
    uint32_t output_mode = kOutputStereo;
    uint32_t voice_count = kDefaultVoices;
    uint32_t sdk_version = 0; // major * 10000 + minor * 100 + micro, or 0 if unknown
    std::vector<AuxEffect> effects;

    // Version 1: the FILE chunks holding the sound archive (.bfsar) and the group files (.bfgrp) the game keeps outside
    // it. Version 2: the start-up archive, then the sequence archive and the folder of the wave archives. Version 3:
    // the sound archive (.brsar), and no group files.
    std::string archive_path;
    std::vector<std::string> group_paths;

    // Version 1 with kFlagTrackVolumes: the track volumes of sounds, each sound once.
    std::vector<SoundTrackVolumes> track_volumes;

    // Version 1 with kFlagScripts: the scripted starts of sounds, each sound once, and the add-on archives that they
    // start sounds from.
    std::vector<ScriptedStart> scripts;
    std::vector<AddonArchive> addons;

    // Version 1 with kFlagProcedures: the game's handler of user procedures, for every sound of the set.
    UserProcedureHandler procedures;

    JaudioSettings jaudio; // version 2
    Nw4rSettings nw4r;     // version 3
};

// The program's chunks in the streaming formats' layout (xsflib/psf/streaming.h): FILE chunks, each a file of the game
// by its path in the game's files, and the SND chunk, the sound to play: an nn::atk item id in version 1, a JAudio
// sound ID in version 2, and the sound's index in the archive's sound table in version 3 (docs/hsf.md, "Program:
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
// for a set the player can't play: one without a descriptor or a sound, or whose descriptor fails its Check().
using LoadedSet = xsflib::psf::LoadedSet<SetFormat>;
using xsflib::psf::LoadSet;

} // namespace hsf
