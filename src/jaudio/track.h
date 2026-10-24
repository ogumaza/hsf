// SPDX-License-Identifier: MIT

// JAudio2's sequences as the Super Mario Galaxy games' `main` builds them for AArch64: a sequence's tracks (JASTrack)
// with their channel managers (JASTrack::TChannelMgr), the reader and the control of a track's commands (JASSeqReader
// and JASSeqCtrl), the parser of the commands (JASSeqParser) with the tracks' ports and registers, the start of a
// note on a bank (JASBank::noteOn), and the list of the playing sequences (JASTrack::TList). Each sub-frame, the list
// runs each sequence's ticks, then updates its tracks' levels. A track's notes play on channels (channel.h).
//
// The tracks refer to one another, to their channels and to their sequence by index. A channel's oscillator can
// point to a track's settings of an envelope (JASTrack's oscillator parameters), as in the game. The tracks
// therefore stay in place for the engine's life.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include "jaudio/channel.h"
#include "jaudio/instruments.h"
#include "jaudio/oscillator.h"

namespace hsf::jaudio
{

// A sequence's data (a BMS file). A track reads its commands from it, and a command can name an envelope or a filter
// in it. The game reads those in the host's byte order, little-endian. A read past the data's end gives 0xff: the
// command that ends a track. In the game, it reads whatever follows the file in memory. The tracks of engines on
// several threads may read one sequence at once. Each envelope's table is made once, under a lock.
class Sequence
{
public:
    explicit Sequence(std::vector<uint8_t> bytes) : bytes_(std::move(bytes))
    {
    }

    Sequence(const Sequence&) = delete;
    Sequence& operator=(const Sequence&) = delete;

    uint8_t U8(uint32_t offset) const
    {
        return offset < bytes_.size() ? bytes_[offset] : 0xff;
    }

    // A 16-bit value in the host's order.
    uint16_t Native16(uint32_t offset) const
    {
        return static_cast<uint16_t>(U8(offset) | U8(offset + 1) << 8);
    }

    // An envelope's points from `offset` to the data's end, in the host's order.
    std::span<const OscPoint> Table(uint32_t offset) const;

private:
    std::vector<uint8_t> bytes_;
    mutable std::mutex mutex_;
    mutable std::map<uint32_t, std::vector<OscPoint>> tables_;
};

// A level of a track that can move to a target over a number of ticks (JASTrack::MoveParam_).
struct TrackParam
{
    float value = 0.0f;
    float target = 0.0f;
    uint32_t count = 0;
};

// A track's channel manager (JASTrack::TChannelMgr): a channel for each of the 8 note ids, and the levels that the
// track gives its channels. The sound's levels (JASSoundParams) come from the sound that plays the sequence.
struct ChannelMgr
{
    bool exists = false;
    std::array<uint32_t, 8> channels{Channel::kNone, Channel::kNone, Channel::kNone, Channel::kNone,
                                     Channel::kNone, Channel::kNone, Channel::kNone, Channel::kNone};
    ChannelParams params;
    const SoundParams* sound_params = nullptr;
};

// A track (JASTrack), by the game's fields.
struct Track
{
    static constexpr uint32_t kNone = 0xffffffff;

    // The status: free, playing or stopped.
    enum Status : uint32_t
    {
        kFree = 0,
        kRun = 1,
        kStop = 2,
    };

    // The flags. The game's pause (0x01) and stop (0x40) come from the game's code above the sequences. Nothing in a
    // rip sets them. The model leaves them out.
    static constexpr uint8_t kMute = 0x02;
    static constexpr uint8_t kGateByTempo = 0x04; // a gate note's time turns into sub-frames by the track's tempo
    static constexpr uint8_t kOwned = 0x08;       // a child that its parent allocated and frees
    static constexpr uint8_t kGateTie = 0x10;     // the next gate note goes on with the playing channel
    static constexpr uint8_t kInvalid = 0x20;     // the levels need an update

    bool in_pool = false; // allocated from the track pool
    bool allocated = false;

    // JASSeqReader.
    const Sequence* sequence = nullptr;
    uint32_t cursor = 0;
    uint32_t depth = 0;
    std::array<uint32_t, 8> stack{};
    std::array<uint16_t, 8> loop_counts{};

    // JASSeqCtrl.
    int32_t timer = 0;
    bool interrupted = false; // whether `swap` holds the cursor of the interrupted commands
    uint32_t swap = 0;
    uint32_t interrupt_table = 0;
    uint16_t pending = 0;
    uint16_t mask = 0;
    uint8_t interrupt_timer = 0; // the timer interrupt's repeats, or 0 for no end
    bool wait_note_finish = false;
    uint32_t timer_count = 0;
    uint32_t timer_reload = 0;
    bool rhythm_parser = false; // whether the parser is the Galaxy games' rhythm parser, not the default one

    // JASTrackPort.
    uint16_t import_flags = 0;
    uint16_t export_flags = 0;
    std::array<uint16_t, 16> ports{};

    // JASRegisterParam.
    std::array<uint16_t, 14> registers{};

    // The levels: volume, pitch, effect send, pan, surround and distortion.
    std::array<TrackParam, 6> params{};

    // The envelopes' settings that the track's notes take (JASOscillator::Data), and the points of a simple ADSR.
    std::array<OscData, 2> oscillators{};
    std::array<bool, 2> oscillator_tables{}; // whether the setting has a table: only then do notes take it
    std::array<OscPoint, 4> adsr{};

    uint32_t parent = kNone;
    std::array<uint32_t, 16> children{};
    std::array<ChannelMgr, 4> mgrs{};
    uint32_t mgr_count = 1;

    float playtime = 1.0f;
    float interval = 1.0f; // ticks per sub-frame
    float vibrato_depth = 0.0f;
    float vibrato_pitch = 1.0f;
    float tremolo_depth = 0.0f;
    float tremolo_pitch = 1.0f;
    uint16_t vibrato_delay = 0;
    uint16_t tremolo_delay = 0;
    std::array<uint16_t, 8> fir{};
    std::array<uint16_t, 8> iir{};
    uint8_t filter_mode = 0;
    float pan_power = 1.0f;
    float tempo_rate = 1.0f;
    uint32_t skip_samples = 0;
    uint16_t direct_release = 0;
    uint16_t tempo = 120;
    uint16_t timebase = 48;
    int8_t transpose = 0;
    uint8_t latest_key = 60;
    uint16_t bank = 0;
    uint16_t program = 0xf0;
    uint8_t bend_sense = 12;
    uint8_t note_on_priority = 0x40;
    uint8_t release_priority = 0;
    uint8_t gate_rate = 100;
    std::array<uint16_t, 6> mix_configs{};
    uint32_t status = kFree;
    uint8_t flags = 0;
};

// The sequences' tracks and the start of their notes. A callback of a track's channel reaches the track's channel
// manager through `OnChannel`.
class Tracks : public ChannelOwner
{
public:
    // JASTrack's pool, from JAU_JASInitializer's settings.
    static constexpr std::size_t kPoolTracks = 200;

    // The tracks of the sounds that play sequences (JAISeq's), at most this many at once.
    static constexpr std::size_t kRootTracks = 8;

    explicit Tracks(Voices& voices);

    Tracks(const Tracks&) = delete;
    Tracks& operator=(const Tracks&) = delete;

    // The bank that a track's bank number names (JASDefaultBankTable's registBank). Numbers 0 to 255.
    void SetBank(uint32_t number, const InstrumentBank* bank);

    // A sound's start of a sequence (JAISeq): a new root track reads `sequence` from `offset`, with the sound's levels
    // `sound` for its channels, and joins the list of playing sequences. `sound` must stay in place while the track
    // plays. Returns the root track's index, or Track::kNone without a free root track.
    uint32_t StartSequence(const Sequence& sequence, uint32_t offset, const SoundParams* sound);

    // JAISeq::reserveChildTracks_ (0x1d98d0): the tracks that a sound of the sequence manager takes from the pool as it
    // starts, before its sequence runs. The root track gets children in slots 0 and 1. The child in slot 0 gets
    // children in the slots below `count`, and the child in slot 1 in the slots below `count` - 16. The sequence's
    // commands open a reserved track in place. A mute or a parser that a track gets before then therefore lasts.
    void ReserveChildTracks(uint32_t root, uint32_t count);

    // AudRhythmMeSystem::setSeq (0x1fbe0c) gives a track the rhythm parser of the Galaxy games (AudRhythmSeqParser,
    // vtable 0x82efb0). The tracks that get the parser share it. The parser reads the commands as the default one
    // does. The parser also keeps the tempo of each tempo command that it parses (0xe0 without registers), 120 at
    // first. At each MIDI note on key 0x18, a beat, the parser sets the root track's tempo to that value.
    void UseRhythmParser(uint32_t index);

    // JASTrack::TList::seqMain: each playing sequence's ticks for a sub-frame, then the update of its levels. A
    // sequence ends when its root track's commands end.
    void SeqMain();

    // Whether the sequence of root track `root` still plays.
    bool Playing(uint32_t root) const
    {
        return tracks_[root].status == Track::kRun;
    }

    const Track& TrackAt(uint32_t index) const
    {
        return tracks_[index];
    }

    // JASInstRand's generator and the sequences' (the commands' random register operation).
    uint32_t InstRandom() const
    {
        return inst_random_;
    }

    uint32_t SeqRandom() const
    {
        return seq_random_;
    }

    // JASTrack::channelUpdateCallback.
    void OnChannel(uint32_t owner, Channel::Event event, uint32_t channel, DspBlock* block) override;

    // The sound's side of a track, for the code above the sequences. JAISeq::getChildTrack: the child in slot `id & 15`
    // of the root's child in slot `id >> 4`, or Track::kNone. JASTrack::assignExtBuffer for the first channel manager:
    // the levels that it takes from the sound, or none. JASTrack::mute.
    uint32_t ChildTrack(uint32_t root, uint32_t id) const;
    void AssignSoundParams(uint32_t index, const SoundParams* params);
    void Mute(uint32_t index, bool mute);

    // For the ripper's length analysis: a track's jump back outside a subroutine and a counted loop (the commands
    // 0xc7 and 0xc8), with the jump's target and whether the track played a note since its last such jump or since it
    // opened. And whether a track has played a note since it opened, whether it has made such a jump, and whether its
    // last pass, up to its last such jump, played no note. A note counts even when it gets no channel.
    using LoopHook = std::function<void(uint32_t track, uint32_t target, bool played)>;

    void SetLoopHook(LoopHook hook)
    {
        loop_hook_ = std::move(hook);
    }

    bool PlayedNotes(uint32_t index) const
    {
        return notes_[index] != 0;
    }

    bool Looped(uint32_t index) const
    {
        return looped_[index];
    }

    bool QuietPass(uint32_t index) const
    {
        return quiet_[index];
    }

    // The notes that asked for a bank that isn't loaded. The game's code reads through a null pointer at the first such
    // note.
    uint32_t MissingBankNotes() const
    {
        return missing_bank_notes_;
    }

    // The rhythm parser's tempo (UseRhythmParser).
    float RhythmTempo() const
    {
        return rhythm_tempo_;
    }

private:
    struct Args
    {
        std::array<uint32_t, 16> values{};

        uint32_t operator[](std::size_t i) const
        {
            return values[i];
        }
    };

    // JASTrack's constructor and init, and the pool's allocation of a child.
    void Construct(uint32_t index);
    void Init(uint32_t index);
    uint32_t AllocTrack();
    void FreeTrack(uint32_t index);

    uint32_t Root(uint32_t index) const;
    bool Muted(uint32_t index) const;
    int32_t TransposeSum(uint32_t index) const;

    // JASTrack::seqMain's steps, tickProc and close.
    int32_t TickProc(uint32_t index);
    int32_t SeqTick(uint32_t index);
    void CheckInterrupt(Track& track);
    void UpdateSeq(uint32_t index, bool update, float num);
    void UpdateTrack(uint32_t index, float num);
    void UpdateTempo(Track& track);
    void Close(uint32_t index);
    void ReleaseAll(ChannelMgr& mgr);
    bool CheckNoteStop(const Track& track, uint32_t note) const;

    // The parser: one command, and the arguments by their widths.
    int32_t Parse(uint32_t index);
    int32_t ParseCommand(uint32_t index, uint32_t command, uint32_t register_mask);
    int32_t Execute(uint32_t index, uint32_t command, const Args& args);
    void JumpBack(uint32_t index, uint32_t target);
    uint32_t ReadMidiValue(Track& track);
    uint32_t ReadReg(const Track& track, uint32_t reg) const;
    void WriteReg(uint32_t index, uint32_t reg, uint32_t value);
    static uint32_t RegisterRead(const Track& track, uint32_t reg);
    static void RegisterWrite(Track& track, uint32_t reg, uint32_t value);
    static bool Condition(const Track& track, uint32_t condition);
    uint32_t Be24(const Track& track, uint32_t offset) const;

    // The commands' actions.
    void NoteOn(uint32_t index, uint32_t note, uint32_t key, uint32_t velocity);
    void NoteOff(uint32_t index, uint32_t note, uint16_t direct_release);
    void NoteOnGate(uint32_t index, uint32_t key, uint32_t velocity, uint32_t time, uint32_t flags);
    void GateOn(uint32_t index, uint32_t key, uint32_t velocity, uint32_t flags, float time);
    uint32_t OpenChild(uint32_t index, uint32_t child);
    void CloseChild(uint32_t index, uint32_t child);
    void SetTempo(Track& track, uint32_t tempo);
    void SetLatestKey(uint32_t index, uint32_t key);
    void WritePort(uint32_t index, uint32_t port, uint32_t value);
    void SetParam(Track& track, uint32_t param, float value, uint32_t count);
    void Printf(Track& track);

    // JASTrack::channelStart and JASBank's noteOn and noteOnOsc. A channel's owner is its track and manager.
    uint32_t ChannelStart(uint32_t index, uint32_t mgr, uint32_t key, uint32_t velocity, uint32_t timer);
    uint32_t BankNoteOn(const InstrumentBank* bank, uint32_t program, uint32_t key, uint32_t velocity,
                        uint16_t priority, uint32_t owner);
    uint32_t NoteOnOsc(uint32_t wave, uint32_t key, uint32_t velocity, uint16_t priority, uint32_t owner);

    Voices& voices_;
    std::vector<Track> tracks_; // the pool's tracks, then the root tracks
    std::vector<uint32_t> free_tracks_;
    std::vector<uint32_t> list_; // the playing sequences' root tracks, in the order that they started
    std::array<const InstrumentBank*, 256> banks_{};
    uint32_t inst_random_ = 0;
    uint32_t seq_random_ = 0;

    // The length analysis's count of each track's notes since it opened, the count at its last jump back, whether it
    // has jumped back, and whether the pass up to its last jump back played no note.
    LoopHook loop_hook_;
    std::vector<uint32_t> notes_;
    std::vector<uint32_t> loop_notes_;
    std::vector<bool> looped_;
    std::vector<bool> quiet_;
    uint32_t missing_bank_notes_ = 0;
    float rhythm_tempo_ = 120.0f;
};

} // namespace hsf::jaudio
