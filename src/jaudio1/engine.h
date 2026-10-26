// SPDX-License-Identifier: MIT

// JAudio1's sound system as the Switch release of Pikmin 2 builds it on Hagi: a piece of music from the sequence
// archive, played on the models of JAudio1's sequences and channels (track.h, channel.h) and of Hagi's emulation of the
// DSP program (jaudio/dsp.h). Above JAudio1, the game's sound library (JAI) gives the sequence its levels, and the
// game's code answers the requests that the sequence's tracks send it (syncCPU). The answers depend on the class of the
// piece in the game's sound code (PSSystem). That code's directors also mute some of the tracks as they start. Settings
// and PieceSettings hold what the game's code sets up.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "jaudio/dsp.h"
#include "jaudio1/channel.h"
#include "jaudio1/conductor.h"
#include "jaudio1/instruments.h"
#include "jaudio1/track.h"

namespace hsf::jaudio1
{

// What the game's code sets up for all of its sounds. The levels of a channel at full volume for the mixers and for
// the auto mixer, and the DSP's level in 4.12, come from JASDriver::setLevel(0.9, 0.9, 3.9) in the game's start-up
// (0x4100c in the release's main). The volumes of the music and of the effects are the game's options for them (the
// sound system's volumes, 1 at the start).
struct Settings
{
    uint16_t channel_level = 14745;
    uint16_t auto_level = 14745;
    uint16_t dsp_level = 15974;
    float music_volume = 1.0f;
    float effect_volume = 1.0f;

    bool operator==(const Settings&) const = default;
};

// How the game's code plays a piece of music: the piece's class in the game's sound code, and what the code sets up
// for it.
struct PieceSettings
{
    // The classes. Each answers the tracks' requests differently.
    enum Kind : uint8_t
    {
        kBgm = 0,      // BgmSeq: a sequence alone
        kDirected = 1, // DirectedBgm: the tracks' requests reach the piece's objects for its tracks and its directors
        kJump = 2,     // JumpBgmSeq: a directed piece that also asks the game where to start and where to jump
        kAuto = 3,     // AutoBgm: a directed piece whose child tracks ask the conductor what to play (conductor.h)
    };

    uint8_t kind = kBgm;

    // The piece's volume in the game's list of its music, over 127.
    uint8_t volume = 50;

    // The levels that a child track keeps when it asks for its levels: bits 0 to 4 for the volume, the pitch, the
    // effect send, the pan and the surround level. The rest start at 0 until JAI sets them. The game's main music keeps
    // them all. Its other music keeps none.
    uint8_t kept_levels = 0x1f;

    // The ticks between the beats that a directed piece's root track asks for (SeqTrackRoot's beat interval).
    uint16_t beat_interval = 60;

    // The child tracks, by slot, that the piece's directors mute as the tracks start: the layers that the game fades in
    // for events, treasure, enemies, battles and the ground.
    uint16_t muted_tracks = 0;

    // The section that a jump piece starts at.
    uint16_t start_point = 0;

    // The conductor file of an AutoBgm piece (a .cnd file of the conductor archive).
    std::vector<uint8_t> conductor;

    bool operator==(const PieceSettings&) const = default;
};

// An FX line as the start-up archive's first FX scene sets it up (JAInter::Fx::init): the line's buffer in sub-frames
// (0 for a line that's off), its flags, the delay's length in sub-frames, its two sends by their buses' addresses in
// the DSP's memory, their levels, and the filter.
struct FxLine
{
    uint32_t buffer_subframes = 0;
    uint16_t flags = 0;
    uint16_t subframes = 0;
    std::array<uint16_t, 2> buses{};
    std::array<int16_t, 2> levels{};
    std::array<int16_t, 8> filter{};

    bool operator==(const FxLine&) const = default;
};

// A sound of the sound table (JAInter::SoundInfo). A sound's ID gives its section in the top 2 bits (0 for an effect,
// 2 for a sequence, 3 for a stream), an effect's category in bits 12 to 19, and the sound's index in its category in
// the low 10 bits. Bits 10 and 11 hold flags of the game's code.
struct SoundInfo
{
    uint32_t flags = 0;
    uint16_t offset = 0; // the number that the sound's sequence takes, if the section's format has bit 0
    float pitch = 1.0f;
    uint8_t volume = 127;
    uint8_t fxmix = 0;

    bool operator==(const SoundInfo&) const = default;
};

// The sound files of the game: the start-up archive (AAF), the sequence archive (an RARC of BMS files, with wide
// entries) and the wave archives (AW) that the wave systems name. The waves of the archives that the set holds go into
// the DSP's memory.
class SoundFiles
{
public:
    // Where the waves start in the DSP's memory. The channels' blocks, the FX lines' settings and buffers and the
    // outputs come before them.
    static constexpr uint32_t kWaves = 0x1c000;

    // The longest buffer of an FX line that fits below the waves, in sub-frames.
    static constexpr uint32_t kMaxFxSubframes = 128;

    // The sound table's categories of effects, 0 to 15. Category 16 holds the sequences and 17 the streams.
    static constexpr uint32_t kEffectCategories = 16;

    // A wave archive's bytes by its name in the wave systems, or null for an archive that the set lacks.
    using WaveArchives = std::function<const std::vector<uint8_t>*(const std::string& name)>;

    // A file of the sequence archive: its number in the archive, its name and the sequence.
    struct SequenceFile
    {
        uint16_t id = 0;
        std::string name;
        std::unique_ptr<Sequence> sequence;
    };

    // Throws xsflib::FormatError for a file that can't be read.
    SoundFiles(std::span<const uint8_t> aaf, std::span<const uint8_t> sequences, const WaveArchives& waves);

    SoundFiles(const SoundFiles&) = delete;
    SoundFiles& operator=(const SoundFiles&) = delete;

    // The sequence archive's files, in its order.
    const std::vector<SequenceFile>& Sequences() const
    {
        return sequences_;
    }

    // The file with number `id`, or null.
    const SequenceFile* FindSequence(uint16_t id) const;

    // The banks, in the start-up archive's order: each one's place in the list is its physical number. Each plays its
    // wave system's wave bank.
    const std::vector<std::unique_ptr<InstrumentBank>>& Banks() const
    {
        return banks_;
    }

    // JASInstRand's generator after the game's start-up has read the banks (JAInter::BankWave::init).
    RandomFast InstGenerator() const
    {
        return inst_random_;
    }

    // The FX lines of the start-up archive's first FX scene.
    const std::array<FxLine, 4>& FxLines() const
    {
        return fx_lines_;
    }

    // The sound table's format byte for the sequences.
    uint8_t SequenceFormat() const
    {
        return sequence_format_;
    }

    // The sound table's format byte for the effects. Bit 0 takes an effect's number from its sound info, bit 1 its
    // pitch, bit 2 its volume and bit 3 its effect send.
    uint8_t EffectFormat() const;

    // The sound of ID `id` in the sound table, or nullopt.
    std::optional<SoundInfo> FindSound(uint32_t id) const;

    // The number of sounds of a category of the sound table: 0 to 15 for the effects' categories, 16 for the
    // sequences and 17 for the streams.
    uint16_t SoundCount(uint32_t category) const;

    // The effects' scenes (the start-up archive's block of type 6, JAInter::SeMgr's category table): for each scene,
    // the number of slot tracks of each category of effects.
    const std::vector<std::vector<uint8_t>>& EffectScenes() const
    {
        return effect_scenes_;
    }

    // The DSP's memory as each sound starts: the waves from kWaves, zeros below.
    const DspMemory& Memory() const
    {
        return *memory_;
    }

    // The names of the wave archives that the wave systems name and the set lacks.
    const std::vector<std::string>& MissingWaves() const
    {
        return missing_waves_;
    }

private:
    void LoadWaves(const WaveArchives& waves);
    void LoadBanks();
    void LoadFxLines(std::span<const uint8_t> scenes);
    void LoadEffectScenes(std::span<const uint8_t> scenes);

    std::vector<uint8_t> aaf_;
    std::vector<SequenceFile> sequences_;
    std::unique_ptr<DspMemory> memory_;
    std::map<uint32_t, WaveBank> wave_banks_; // by wave system
    std::vector<std::unique_ptr<InstrumentBank>> banks_;
    RandomFast inst_random_;
    std::array<FxLine, 4> fx_lines_{};
    uint8_t sequence_format_ = 0;
    std::vector<uint8_t> sound_table_;
    std::vector<std::vector<uint8_t>> effect_scenes_;
    std::vector<std::string> missing_waves_;
};

// A piece played on the model, sub-frame by sub-frame. A frame of the DSP is 7 sub-frames.
class Engine
{
public:
    static constexpr int kSubframesPerFrame = 7;

    // A loop of a track: its index, the command that its jump goes to, and whether the track started a note since
    // its last such jump or since it opened.
    using LoopHook = std::function<void(uint32_t track, uint32_t target, bool played)>;

    Engine(const SoundFiles& files, const Settings& settings);
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    ~Engine();

    // Starts the sequence of file `id` of the sequence archive from the beginning, as `piece`. Returns an error for a
    // file that the archive lacks.
    std::optional<std::string> Start(uint16_t id, const PieceSettings& piece);

    // Starts the sequence of file `id` (the game's se.bms) as the game's effect sequence (PSSystem::SeSeq), at
    // `volume`, its volume in the game's list of its music over 127. Then starts the effect `sound_id` of the sound
    // table on it (JAInter::SeMgr). The effect takes the first slot track of its category in effect scene 0. It starts
    // in the game's first frame in which the effect sequence plays and the slot track is open. The effect plays at the
    // listener, and the game doesn't stop it. Returns an error for a file that the archive lacks, or for a sound that
    // isn't an effect of the sound table.
    std::optional<std::string> StartEffect(uint16_t id, uint8_t volume, uint32_t sound_id);

    // Draws `count` numbers from the sequences' generator (JASPlayer::getRandomS32) after a start, before the first
    // sub-frame. The game's sequences share the generator. Its state as a sound starts depends on what the game played
    // before. Each start begins at the generator's state as the game starts.
    void DrawSequenceNumbers(uint32_t count);

    // The numbers that the sequence has drawn from the sequences' generator since its start. Where it has drawn none,
    // the generator's state doesn't change what it plays.
    uint32_t SequenceDraws() const;

    // A sub-frame: at a frame's start, DsyncFrame. Then the DSP's render, JAI's work for the game's frame when one
    // falls due, the port commands that JAI has left, the DSP channels' update and the sequence's ticks.
    void RunSubframe();

    // The sub-frame's output: 80 stereo pairs, left then right, at the DSP's rate.
    std::span<const int16_t> Output() const
    {
        return output_;
    }

    // Whether the piece's sequence still plays. For an effect: whether JAI still holds the effect.
    bool Playing() const;

    // Whether the sequence plays, a DSP channel sounds, or the FX lines' delays still hold sound.
    bool Busy() const;

    // The DSP channels that play.
    int ActiveChannels() const;

    // Whether the FX lines' delays hold sound that they haven't played yet: a sample louder than 8 (-72 dB). A line's
    // feedback can keep quieter samples going round for ever.
    bool FxRinging() const;

    // The notes that asked for a bank that the files don't load. The game's note then doesn't start.
    uint32_t MissingBankNotes() const;

    // What the model leaves out: the tracks' reads and writes outside the tracks (Tracks::Unmodelled), and the
    // requests to the game's code that the piece's class doesn't answer.
    uint32_t Unmodelled() const;

    // Sets the hook for the tracks' jumps back. A jump inside a subroutine or a counted loop doesn't call it.
    void SetLoopHook(LoopHook hook);

    // The piece's tracks in the order of the sequence's tree: the root, then each child after its parent, by the
    // child's slot. Tracks that have closed aren't in it.
    std::vector<uint32_t> TreeOrder() const;

    // Whether the track has started a note since the piece started, and whether its last pass, up to its last jump
    // back, started none.
    bool PlayedNotes(uint32_t track) const;
    bool QuietPass(uint32_t track) const;

    // The length analysis's tests of a held sound. Held: each channel that plays holds its note on a looping wave,
    // with each envelope at its sustain level or in a cycle, and each open track has started a note, jumped back or
    // opened a track that is still open. Moving: a track's levels move. WaitsForEver: the track waits for its note to
    // end, and that note is held. Waiting: each open track that has opened no track still open waits for its note to
    // end, or has started no note and jumped back.
    bool Held() const;
    bool Moving() const;
    bool WaitsForNotes(uint32_t track) const;
    bool WaitsForEver(uint32_t track) const;
    bool Waiting() const;

    // A DSP channel's place in its wave, for the length analysis: whether it plays a wave, whether the wave loops, and
    // a position that drops each time the wave passes back to its loop's start.
    struct WavePlace
    {
        bool playing = false;
        bool loops = false;
        int64_t position = 0;
    };

    WavePlace DspChannelPlace(uint32_t index) const;

private:
    struct Model;

    // The game's answer to a track's request (PSGame's seqCpuSync).
    uint16_t Sync(uint32_t track, uint32_t value);

    // The start of the sequence for Start and StartEffect: the DSP and its FX lines, the banks, JAI's volume for the
    // sequence and the root track.
    std::optional<std::string> StartSequence(const SoundFiles::SequenceFile& file, float volume);

    // JAI's work in the game's frame (JAInter::SequenceMgr::processGFrameSequence) that changes a piece that the game
    // leaves alone: the levels of the child tracks once the root track has opened one (checkStartedSeq).
    void GameFrame();

    // JAI's work for an effect in the game's frame (JAInter::SeMgr::processGFrameSe), before the sequences' work.
    void EffectFrame();

    // The effect's levels from its lines (JAInter::SeMgr::sendSeAllParameter). With `send`, the levels that have
    // changed go to the slot track by a port command.
    void SendEffectLevels(bool send);

    // The track that JAI gives the number `number`: a child's slot, or 32 for the root track. The effect sequence's
    // numbers below 32 name the children of the root track's first two children, 16 each. Track::kNone if the slot is
    // empty.
    uint32_t JaiTrack(uint32_t number) const;

    // JAInter::SystemInterface::outerInit for the track that JAI numbers `number`: the levels that JAI
    // gives the track, by a port command that runs at the next sub-frame's start. `kept` holds the levels that keep
    // their values until then.
    void OuterInit(uint32_t number, uint16_t kept);

    // The port commands that JAI has left (JASPortCmd::execAllCommand).
    void RunPortCommands();

    const SoundFiles& files_;
    Settings settings_;
    std::unique_ptr<DspMemory> memory_;
    std::unique_ptr<Model> model_;
    std::vector<int16_t> output_;
    LoopHook loop_hook_;
};

} // namespace hsf::jaudio1
