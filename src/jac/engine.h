// SPDX-License-Identifier: MIT

// The C JAudio as the Switch release of Pikmin builds it on Hagi, with the parts of the game's code that set up its
// sounds: the start-up (Jac_Start and the game's code before its first title screen), the sequences that it plays on
// handles 0 to 3, the game's work in each of its frames (Jac_Gsync), and the start of a piece of music, a piece of the
// cutscenes' music or an effect. The models of the C JAudio's sequences, channels and banks (track.h, channel.h,
// bank.h) and of Hagi's emulation of the DSP program (jaudio/dsp.h) play them.
//
// A frame of the Switch's build is one sub-frame of 80 samples (JAC_SUBFRAMES and JAC_FRAMESAMPLES). Hagi plays them
// at 32,000 Hz: 400 sub-frames a second. At each frame's end, DspbufProcess starts the DSP's next frame (DsyncFrame),
// and UpdateDSP runs the CPU's side: the DSP player's callbacks, UpdateDSPchannelAll and the player's callbacks. The
// player's callbacks run the root tracks and two lists of the game's code: the port commands (the task list) and the
// queues of values for the tracks' ports. Hagi renders the frame from the blocks as UpdateDSP left them at the frame
// before.
//
// The game's code runs on another thread, between two sub-frames. The model runs it after a sub-frame's UpdateDSP.
// The game's main loop calls Jac_Gsync once in each of the game's frames. The model takes 30 frames a second: the
// game's frame k starts after the first sub-frame n with (n + 1) × 30 >= (k + 1) × 400. Jac_Gsync runs first, then
// the work of the sound in that frame.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/jaudio/bank.h"
#include "ntdlib/jaudio/wave_system.h"

#include "jac/bank.h"
#include "jac/calc.h"
#include "jac/channel.h"
#include "jac/track.h"
#include "jaudio/dsp.h"
#include "jaudio/track.h"

namespace hsf::jac
{

// An FX line as the game's start-up sets it up: its flags (0 for a line that's off), the delay's length in sub-frames,
// its two sends by their buses' addresses in the DSP's memory, their levels (0x8000 for 1), and the filter.
struct FxLine
{
    uint16_t flags = 0;
    uint16_t subframes = 0;
    std::array<uint16_t, 2> buses{};
    std::array<uint16_t, 2> levels{};
    std::array<int16_t, 8> filter{};
};

// What the game's start-up sets for all of its sounds. Jac_Start calls Jac_SetMixerLevel(0.8, 1.5): the mixers' level
// at full volume is 0.8 × 32,767 (Channel_SetMixerLevel), and the DSP's level in 4.12 is 1.5 × 4,096 (DsetMixerLevel).
struct Settings
{
    float channel_level = 0.8f;
    uint16_t dsp_level = 6144;
    std::array<FxLine, 4> fx_lines{};
};

// A wave scene: a scene of a wave system, by the system's place in the bank file and the scene's place in the system.
struct Scene
{
    uint16_t system = 0;
    uint16_t scene = 0;
};

// A scene that the game's start-up loads, and whether it sets it.
struct SceneLoad
{
    Scene scene;
    bool set = false;
};

// What the start-up plays and loads: the scenes in the game's order, the files of the sequence archive that start on
// handles 0 to 3, and the levels that the game writes to the root tracks' ports: the effects' to port 0 of handles 0
// and 1, the cutscenes' to port 2 of handle 2. `saved` is true when the game found a save. The game's code sets the
// options' levels only from a save. With no save, the start-up writes neither level, and the ports keep the
// sequences' values.
struct Startup
{
    std::vector<SceneLoad> loads;
    std::array<uint16_t, 4> files{0, 1, 13, 2};
    uint16_t effect_level = 0;
    uint16_t cutscene_level = 0;
    bool saved = true;
};

// The music's volume (0x6af444) in main's data. It stays at that value when the game finds no save (Startup::saved).
inline constexpr float kDataMusicVolume = 1.0f;

// A piece of music as the game starts it: its file of the sequence archive (and its bank), the child tracks that play
// and their volume, the scene that the game sets first, and the root track's volume: the music's volume of the
// options, or kDataMusicVolume with no save.
struct Piece
{
    uint16_t file = 0;
    uint16_t playing_tracks = 0xffff;
    float volume = 0.5f;
    Scene scene;
    float music_volume = kDataMusicVolume;
};

// An effect as the game's code starts it (docs/hsf.md, "Library 3: Pikmin"): the kind, the type, the value and the
// first value. A cutscene's sound also takes the game's frames from the sound to the cutscene's finish, or 0 for none.
struct Effect
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

    // A cutscene's type when the game writes the cutscene's number to port 1 too.
    static constexpr uint8_t kCutscenePart = 1;

    // A value that the effect doesn't write.
    static constexpr uint16_t kNone = 0xffff;

    uint8_t kind = 0;
    uint8_t type = 0;
    uint16_t value = 0;
    uint16_t first = kNone;
    uint16_t finish = 0;
};

// The game's numbers of the effects that the game's code handles apart from the others. A rip's selection of the
// effects reads them too.
//
// The system's effects that Jac_PlaySystemSe (0x266530) plays as other sounds. kContainerFilled plays the captain's
// effect kContainerSound. kShipPartEffect sets scene 6 of wave system kPartSystem and plays piece kShipPart of the
// cutscenes' music. kHighScore plays a stream. kPauseAll and kResumeAll pause and resume the game's sounds.
// kPlayerDownPause pauses the events and stops the formation's sound.
inline constexpr uint16_t kContainerFilled = 0x10;
inline constexpr uint16_t kShipPartEffect = 0x12;
inline constexpr uint16_t kHighScore = 0x20;
inline constexpr uint16_t kPauseAll = 0x26;
inline constexpr uint16_t kResumeAll = 0x27;
inline constexpr uint16_t kPlayerDownPause = 0x28;
inline constexpr uint16_t kContainerSound = 0xe;
inline constexpr uint16_t kPartSystem = 13;
inline constexpr uint16_t kShipPart = 8;

// The captain's effects (Jac_PlayOrimaSe, 0x265250): bit 15 marks an effect of the Pikmin. The pluck (kPluck) draws a
// cry (0x2652d4).
inline constexpr uint16_t kPikminEffect = 0x8000;
inline constexpr uint16_t kPluck = 4;

// The type of event whose creation starts its first action: the ship's (Jac_CreateEvent, 0x2626d0).
inline constexpr uint8_t kShipEvent = 7;

// The sound files of the game: the bank file (.bx), the sequence archive with its index (BARC), and the wave archives
// that the wave systems name.
class SoundFiles
{
public:
    using WaveArchives = Banks::WaveArchives;

    // A sequence of the archive: its number (its place in the index), its name and its commands.
    struct SequenceFile
    {
        uint16_t number = 0;
        std::string name;
        std::unique_ptr<Sequence> sequence;
    };

    // Throws xsflib::FormatError for a file that can't be read.
    SoundFiles(std::span<const uint8_t> bank_file, std::span<const uint8_t> sequences, std::span<const uint8_t> index,
               WaveArchives waves);

    SoundFiles(const SoundFiles&) = delete;
    SoundFiles& operator=(const SoundFiles&) = delete;

    const std::vector<SequenceFile>& Sequences() const
    {
        return sequences_;
    }

    // The sequence of number `number`, or null.
    const SequenceFile* FindSequence(uint16_t number) const;

    // The bank file's banks and wave systems, by their places in the file.
    const std::vector<ntdlib::jaudio::Bank>& FileBanks() const
    {
        return banks_;
    }

    const std::vector<ntdlib::jaudio::WaveSystem>& FileSystems() const
    {
        return systems_;
    }

    const WaveArchives& Waves() const
    {
        return waves_;
    }

private:
    std::vector<SequenceFile> sequences_;
    std::vector<ntdlib::jaudio::Bank> banks_;
    std::vector<ntdlib::jaudio::WaveSystem> systems_;
    WaveArchives waves_;
};

// The game's sound system on the model, frame by frame.
class Engine
{
public:
    // The DSP's memory: the 64 channels' blocks from kBlocks, the FX lines' settings and buffers, the outputs, and
    // ARAM from kAram.
    static constexpr uint32_t kBlocks = 0;
    static constexpr uint32_t kAram = 0x40000;
    static constexpr uint32_t kAramSize = 0x980000; // the game's size of ARAM (Jac_SetAudioARAMSize)

    // The sound system as Jac_Start leaves it: the mixers' and the DSP's levels, the FX lines (AuxBusInit), the tracks
    // and channels as Jaq_Reset and the channels' set-up leave them, and the game's answer to the tracks' requests
    // (TrackReceive, 0x267060 in the release's main). TrackReceive closes scene 6 of system 13 on the request 0x1000,
    // and it stops the events' actions on 0x8000 and 0x9000. No scene is loaded yet.
    Engine(const SoundFiles& files, const Settings& settings);
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    ~Engine();

    // The start-up (fn_266db0, called at 0x3b9e0 in the release's main): the sequences on handles 0 to 3
    // (Jac_InitBgm), the scenes in the game's order and the player's two lists. The game then waits for the track that
    // registers 0x20000, and sets up the events on that track's children (Jac_InitEventSystem) and the queue of the
    // cutscenes' effects (Jac_InitDemoSystem). The model does that in UpdateFrame, after the first sub-frame in which
    // the track is there. The options' code sets the levels of the music and the effects at the end of the start-up.
    // Gsync 0 applies them. Returns an error for a file that the archive lacks.
    std::optional<std::string> StartUp(const Startup& startup);

    // The game's frames that have started: the calls of Jac_Gsync. A sound starts in frame 0, once GameFrames gives 1.
    uint32_t GameFrames() const;

    // A piece of music on the music's first handle (handle 4): the piece's scene is set, then Jac_PlayBgm starts it
    // with its file's number as its bank. The root track plays at the music's volume. Each child track that plays gets
    // the piece's volume, and the others 0.
    std::optional<std::string> StartPiece(const Piece& piece);

    // A piece of the cutscenes' music: its scene is set, then its number goes to port 0 of the root track on handle 2
    // (Jac_PlayDemoSequenceDirect).
    std::optional<std::string> StartPart(uint16_t number, const Scene& scene);

    // An effect, through the ports of the effects' tracks (docs/hsf.md, "Library 3: Pikmin"). Its work in the frames
    // after frame 0 runs in UpdateFrame.
    std::optional<std::string> StartEffect(const Effect& effect);

    // A frame: the DSP's render, then the CPU's update.
    void RunFrame()
    {
        RenderFrame();
        UpdateFrame();
    }

    // The DSP's side of a frame: DsyncFrame's start of the frame, and Hagi's render of the blocks into the output.
    void RenderFrame();

    // The CPU's side of a frame: UpdateDsp, then the work of the game's code that falls due.
    void UpdateFrame();

    // UpdateDSP alone, for tests: the DSP player's callbacks, UpdateDSPchannelAll and the player's callbacks.
    void UpdateDsp();

    // The frame's output: 80 stereo pairs, left then right, at the DSP's rate.
    std::span<const int16_t> Output() const
    {
        return output_;
    }

    // Whether the sound still plays. A piece plays while its root track is open. A part or an effect plays on tracks
    // that keep running: it plays while its code has a step to take or a value that a track hasn't read, or while a
    // track of the trees that its code writes to has opened or started a note since it started.
    bool Playing() const;

    // Whether the sound plays, a DSP channel sounds, or the FX lines' delays still hold sound.
    bool Busy() const;

    // The DSP channels that play.
    int ActiveChannels() const;

    // Whether the FX lines' delays hold sound that they haven't played yet: a sample louder than 8 (-72 dB). A line's
    // feedback can keep quieter samples going round for ever.
    bool FxRinging() const;

    // The notes that asked for a bank, a program or a wave that the bank file and the current scenes lack. The game's
    // note then doesn't start.
    uint32_t MissingBankNotes() const;

    // The steps that the model leaves out: the tracks' (Tracks::Unmodelled) and the game's code's.
    uint32_t Unmodelled() const;

    // The length analysis, as jaudio1::Engine gives it, with jumps at any depth. A jump back of a track: its index,
    // the jump's target, whether the track started a note since its last jump back at the same depth or since it
    // opened, and the levels below the jump (LoopJump).
    using LoopHook = Tracks::LoopHook;

    void SetLoopHook(LoopHook hook);

    // The sound's tracks in the order of the sequence's tree: the root, then each child after its parent, by the
    // child's slot. Tracks that have closed aren't in it. A piece has its root's tree. A part or an effect has the
    // trees of the tracks whose ports its code writes, in the order of the writes.
    std::vector<uint32_t> TreeOrder() const;

    // Whether the track has started a note since the sound started, and whether its last pass, up to its last jump
    // back outside a subroutine and a counted loop, started none.
    bool PlayedNotes(uint32_t track) const;
    bool QuietPass(uint32_t track) const;

    // The tests of a held sound. Held: each channel that plays holds its note at the sustain level of its envelopes,
    // on a looping wave or an oscillator, and each open track has started a note, jumped back or opened a track that
    // is still open. An envelope with no attack table holds its level from the note's start.
    // Moving: a track's levels move. WaitsForNotes: the track waits for its note to end. WaitsForEver: it waits for a
    // note that is held. Waiting: each open track that has opened no track still open waits for its note to end.
    bool Held() const;
    bool Moving() const;
    bool WaitsForNotes(uint32_t track) const;
    bool WaitsForEver(uint32_t track) const;
    bool Waiting() const;

    // A DSP channel's place in its wave: whether it plays a wave, whether the wave loops, and a position that drops
    // each time the wave passes back to its loop's start.
    struct WavePlace
    {
        bool playing = false;
        bool loops = false;
        int64_t position = 0;
    };

    WavePlace DspChannelPlace(uint32_t index) const;

    // The models, for tests and the length analysis.
    jaudio::DspMemory& Memory()
    {
        return *memory_;
    }

    const jaudio::DspMemory& Memory() const
    {
        return *memory_;
    }

    Tracks& TrackModel();
    const Tracks& TrackModel() const;
    Channels& ChannelModel();
    const Channels& ChannelModel() const;
    Banks& BankModel();
    const Banks& BankModel() const;
    Random& Generator();

    // The model's state, with the game's state above the C JAudio (engine.cpp).
    struct Model;

private:
    // The effect's work in its next frame.
    void EffectFrame();

    const SoundFiles& files_;
    Settings settings_;
    std::unique_ptr<jaudio::DspMemory> memory_;
    std::unique_ptr<Model> model_;
    std::vector<int16_t> output_;
};

} // namespace hsf::jac
