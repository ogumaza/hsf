// SPDX-License-Identifier: MIT

#include "jac/track.h"

#include <algorithm>

#include "jaudio1/iir_table.h"

namespace hsf::jac
{

namespace
{

// jamosc's tables of points, from main's data: the release of the default envelope (REL_TABLE, 0x6afdc8), a vibrato
// (VIB_TABLE, 0x6afdd4), a tremolo (TRE_TABLE, 0x6afdf8), and a simple ADSR (ADS_TABLE, 0x5bf3de). Each point is a
// mode, a time and a value.
constexpr std::array<OscPoint, 2> kRelTable = {{{0, 10, 0}, {15, 1, 0}}};
constexpr std::array<OscPoint, 6> kVibTable = {
    {{0, 0, 0}, {0, 20, 0x7fff}, {0, 20, 0}, {0, 20, -0x4000}, {0, 20, 0}, {13, 0, 1}}};
constexpr std::array<OscPoint, 6> kTreTable = {
    {{0, 0, 0x7fff}, {0, 20, 0}, {0, 20, -0x7fff}, {0, 20, 0}, {0, 20, 0x7fff}, {13, 0, 1}}};
constexpr std::array<OscPoint, 4> kAdsTable = {{{0, 0, 0x7fff}, {0, 0, 0x7fff}, {0, 0, 0}, {14, 0, 0}}};

template <std::size_t N>
OscTable TableOf(const std::array<OscPoint, N>& points)
{
    return {points.data(), N};
}

// jamosc's settings (main's data): VIBRATO_DEF (0x6afd68), TREMOLO_DEF (0x6afd88), ENVELOPE_DEF (0x6afda8) and
// ADSR_DEF. Each holds the target, the rate, the two tables, the width and the vertex.
const OscData kVibratoDef{1, 0.8f, TableOf(kVibTable), TableOf(kVibTable), 0.0f, 1.0f};
const OscData kTremoloDef{0, 1.0f, TableOf(kTreTable), TableOf(kTreTable), 0.0f, 1.0f};
const OscData kEnvelopeDef{0, 1.0f, {}, TableOf(kRelTable), 1.0f, 0.0f};
const OscData kAdsrDef{0, 1.0f, {}, {}, 1.0f, 0.0f};

// The bytes from osc_table (0x5bf2c4): the update flags of a track oscillator's targets, then Cmd_PanSwSet's two
// tables (0x5bf2c9 and 0x5bf2d0) and the byte after them. A target past the first table's 5 entries reads on into the
// next ones, as the game does.
constexpr std::array<uint8_t, 20> kOscFlags = {0x01, 0x02, 0x08, 0x04, 0x10, 0, 0, 0, 1, 1,
                                               2,    2,    0,    1,    2,    0, 2, 0, 2, 0};
constexpr std::size_t kCalcTypes = 5;
constexpr std::size_t kParentCalcTypes = 12;

// The command table's argument lists (Arglist, 0x5bf1c4): each command's count of arguments, and their types, two
// bits each from the low bits: 0 a byte, 1 a 16-bit word, 2 a 24-bit word, 3 a register's value.
struct ArgSpec
{
    uint8_t count = 0;
    uint16_t types = 0;
};

constexpr std::array<ArgSpec, 64> kArgs = {{
    {0, 0x000}, {2, 0x008}, {2, 0x008}, {1, 0x002}, {0, 0x000}, {0, 0x000}, {1, 0x000}, {1, 0x002},
    {0, 0x000}, {1, 0x001}, {0, 0x000}, {2, 0x000}, {2, 0x00c}, {1, 0x000}, {1, 0x000}, {1, 0x003},
    {2, 0x005}, {2, 0x00c}, {2, 0x00c}, {2, 0x00f}, {1, 0x000}, {1, 0x000}, {1, 0x000}, {2, 0x008},
    {5, 0x155}, {1, 0x000}, {1, 0x000}, {1, 0x000}, {1, 0x001}, {2, 0x004}, {1, 0x000}, {2, 0x008},
    {1, 0x000}, {0, 0x000}, {0, 0x000}, {0, 0x000}, {2, 0x004}, {0, 0x000}, {0, 0x000}, {1, 0x001},
    {0, 0x000}, {0, 0x000}, {1, 0x002}, {5, 0x000}, {4, 0x055}, {1, 0x002}, {1, 0x002}, {3, 0x000},
    {1, 0x000}, {1, 0x000}, {3, 0x028}, {0, 0x000}, {0, 0x000}, {0, 0x000}, {0, 0x000}, {0, 0x000},
    {0, 0x000}, {0, 0x000}, {1, 0x001}, {0, 0x000}, {0, 0x000}, {1, 0x001}, {1, 0x001}, {0, 0x000},
}};

// The commands by their place in the table (CMDP_LIST, 0x6afb68 in main's data): 0xc0 plus the place.
enum CommandIndex : uint8_t
{
    kOpenTrack = 1,
    kOpenTrackBros = 2,
    kCall = 3,
    kCallF = 4,
    kRet = 5,
    kRetF = 6,
    kJmp = 7,
    kJmpF = 8,
    kLoopS = 9,
    kLoopE = 10,
    kReadPort = 11,
    kWritePort = 12,
    kCheckPortImport = 13,
    kCheckPortExport = 14,
    kWaitReg = 15,
    kConnectName = 16,
    kParentWritePort = 17,
    kChildWritePort = 18,
    kSetLastNote = 20,
    kTimeRelate = 21,
    kSimpleOsc = 22,
    kSimpleEnv = 23,
    kSimpleAdsr = 24,
    kTranspose = 25,
    kCloseTrack = 26,
    kOutSwitch = 27,
    kUpdateSync = 28,
    kBusConnect = 29,
    kPauseStatus = 30,
    kSetInterrupt = 31,
    kDisInterrupt = 32,
    kClrI = 33,
    kSetI = 34,
    kRetI = 35,
    kIntTimer = 36,
    kConnectOpen = 37,
    kConnectClose = 38,
    kSyncCpu = 39,
    kFlushAll = 40,
    kFlushRelease = 41,
    kWait3 = 42,
    kPanPowSet = 43,
    kIirSet = 44,
    kFirSet = 45,
    kExtSet = 46,
    kPanSwSet = 47,
    kOscRoute = 48,
    kIirCutOff = 49,
    kOscFull = 50,
    kCheckWave = 58,
    kPrintf = 59,
    kNop = 60,
    kTempo = 61,
    kTimeBase = 62,
    kFinish = 63,
};

// The results of a command for the parser: go on, end the tick, try the interrupts again, or end the track.
constexpr int32_t kContinue = 0;
constexpr int32_t kEndTick = 1;
constexpr int32_t kRetry = 2;
constexpr int32_t kEndTrack = 3;

// The registers by number.
constexpr uint32_t kRegValue = 3;
constexpr uint32_t kRegX = 4;
constexpr uint32_t kRegY = 5;
constexpr uint32_t kRegBank = 6;
constexpr uint32_t kRegBendRange = 7;
constexpr uint32_t kRegArguments = 8; // the five pan weights (arguments), 8 to 12
constexpr uint32_t kRegPriority = 13;
constexpr uint32_t kRegLong = 16;

// The update flags: the levels of the timed parameters by their index, and the outer parameters' levels.
constexpr uint32_t kUpdateVolume = 0x1;
constexpr uint32_t kUpdatePitch = 0x2;
constexpr uint32_t kUpdateFxmix = 0x4;
constexpr uint32_t kUpdatePan = 0x8;
constexpr uint32_t kUpdateDolby = 0x10;
constexpr uint32_t kUpdateDistortion = 0x20;
constexpr uint32_t kUpdateTempo = 0x40;
constexpr uint32_t kUpdateFir = 0x80;
constexpr uint32_t kUpdateIir = 0xf000;
constexpr uint32_t kUpdateDelay = 0x20000;

// The timed parameters by index.
constexpr std::size_t kTimedVolume = 0;
constexpr std::size_t kTimedPitch = 1;
constexpr std::size_t kTimedFxmix = 2;
constexpr std::size_t kTimedPan = 3;
constexpr std::size_t kTimedDolby = 4;
constexpr std::size_t kTimedDistortion = 5;
constexpr std::size_t kTimedIir = 12;
constexpr std::size_t kTimedPause = 16;
constexpr std::size_t kTimedDelay = 17;

constexpr float kShortMax = 32767.0f;
constexpr uint8_t kNoNote = 0xff;

uint16_t Extend8To16(uint16_t value)
{
    return (value & 0x80) != 0 ? static_cast<uint16_t>(value + 0xff00) : value;
}

// __PanCalc: the first level, the second, or a mix of the two by `weight`.
float PanCalc(float from, float to, float weight, uint8_t type)
{
    switch (type)
    {
    case 0:
        return from;
    case 1:
        return to;
    case 2:
        return from * (1.0f - weight) + to * weight;
    default:
        // The game returns a register that holds no result.
        return from;
    }
}

// A float's conversion to an integer as the build does it (fcvtzs): toward 0, saturated to 32 bits.
int32_t ToInt(float value)
{
    if (!(value == value))
    {
        return 0;
    }
    if (value >= 2147483648.0f)
    {
        return 0x7fffffff;
    }
    if (value <= -2147483648.0f)
    {
        return static_cast<int32_t>(0x80000000u);
    }

    return static_cast<int32_t>(value);
}

// The two delays of a track's outputs from its level of delay (OSf32tos8 and the code around it): the level times
// 128, toward 0, gives the left output's delay when it's negative and the right output's when it's positive.
std::array<uint8_t, 2> Delays(float level)
{
    const int32_t value = ToInt(level * 128.0f);
    const auto left = static_cast<uint8_t>(static_cast<uint32_t>(-value) & (static_cast<uint32_t>(value) >> 7));
    const auto right = static_cast<uint8_t>(value < 0 ? 0 : value);
    return {left, right};
}

} // namespace

Tracks::Tracks(Channels& channels, const Banks& banks, Random& random)
    : channels_(channels), banks_(banks), random_(random)
{
    // Jaq_Reset.
    for (uint32_t i = 0; i < kTracks; i++)
    {
        tracks_[i].state = Track::kClosed;
        tracks_[i].child_mutes = 0;
        tracks_[i].muted = false;
        free_queue_[i] = i;
    }
    get_ = 0;
    back_ = 0;
    remain_ = kTracks;
    roots_.fill(Track::kNone);
    player_calls_.fill(Track::kNone);
    outer_.resize(kRoots);
    InitRegistTrack();
}

// seqsetup.

uint32_t Tracks::GetNewTrack()
{
    if (remain_ == 0)
    {
        return Track::kNone;
    }

    const uint32_t index = free_queue_[get_];
    get_ = (get_ + 1) % kTracks;
    remain_--;
    Track& track = tracks_[index];
    track.state = Track::kReady;
    track.allocated = true;

    return index;
}

bool Tracks::BackTrack(uint32_t index)
{
    Track& track = tracks_[index];
    track.state = Track::kClosed;
    if (!track.allocated || remain_ == kTracks)
    {
        return false;
    }

    free_queue_[back_] = index;
    remain_++;
    back_ = (back_ + 1) % kTracks;

    return true;
}

int32_t Tracks::AllocNewRoot(uint32_t index)
{
    for (uint32_t i = 0; i < kRoots; i++)
    {
        if (roots_[i] == Track::kNone)
        {
            roots_[i] = index;
            return static_cast<int32_t>(i);
        }
    }

    return -1;
}

void Tracks::DeAllocRoot(uint32_t index)
{
    for (uint32_t& root : roots_)
    {
        if (root == index)
        {
            root = Track::kNone;
            return;
        }
    }
}

void Tracks::InitTrack(uint32_t index, uint32_t cursor, uint32_t parent)
{
    // Init_Track. The fields that it doesn't set keep what the track's last use left.
    Track& track = tracks_[index];
    if (parent == Track::kNone)
    {
        track.cursor = 0;
        track.tempo = 120;
        track.timebase = 48;
        track.time_relate = 1;
        track.paused = false;
        track.pause_status = 10;
        track.child_mutes = 0;
        track.muted = false;
    }
    else
    {
        const Track& from = tracks_[parent];
        track.sequence = from.sequence;
        track.cursor = cursor;
        track.file_handle = from.file_handle;
        track.tempo = from.tempo;
        track.tempo_sync = false;
        track.tempo_rate = from.tempo_rate;
        track.timebase = from.timebase;
        track.time_relate = from.time_relate;
        track.paused = from.paused;
        track.pause_status = from.pause_status;
        track.child_mutes = 0;
    }
    track.depth = 0;
    track.note_time = 0;
    track.wait = 0;
    track.state = Track::kPlaying;
    track.parent = parent;
    track.enabled = 0;
    track.interrupted = 0;
    track.timer = 0;

    for (MoveParam& param : track.timed)
    {
        param.time = 0.0f;
        param.value = 1.0f;
        param.target = 1.0f;
    }
    const auto set = [&track](std::size_t param, float value)
    {
        track.timed[param].value = value;
        track.timed[param].target = value;
    };
    set(kTimedPitch, 0.0f);
    set(kTimedPan, 0.5f);
    set(kTimedPause, 0.5f);
    set(kTimedDelay, 0.0f);
    set(kTimedFxmix, 0.0f);
    set(kTimedDolby, 0.0f);
    for (std::size_t i = 1; i < 4; i++)
    {
        set(kTimedIir + i, 0.0f);
    }
    set(kTimedDistortion, 0.0f);

    track.registers.fill(0);
    ChannelManager& manager = channels_.Manager(index);
    if ((track.flags & 2) != 0 || parent == Track::kNone)
    {
        track.registers[kRegArguments + 0] = 0;
        track.registers[kRegArguments + 1] = 1;
        track.registers[kRegArguments + 2] = 1;
        track.registers[kRegArguments + 3] = 0x7fff;
        track.registers[kRegArguments + 4] = 0x4000;
        track.pan_calcs.fill(2);
        track.parent_pan_calcs.fill(2);
        manager.pan_calcs.fill(26);
        track.registers[kRegBank] = 0xf0;
        track.registers[kRegBendRange] = 0x0c;
        track.registers[kRegPriority] = 0x40;
    }
    else
    {
        const Track& from = tracks_[parent];
        for (std::size_t i = 0; i < 5; i++)
        {
            track.registers[kRegArguments + i] = from.registers[kRegArguments + i];
        }
        track.registers[kRegBank] = from.registers[kRegBank];
        track.registers[kRegBendRange] = from.registers[kRegBendRange];
        track.registers[kRegPriority] = from.registers[kRegPriority];
        track.pan_calcs = from.pan_calcs;
        track.parent_pan_calcs = from.parent_pan_calcs;
        manager.pan_calcs = channels_.Manager(parent).pan_calcs;
    }

    track.child_outer.fill(Track::kNone);
    track.children.fill(Track::kNone);
    track.notes.fill(kNoNote);
    track.channels.fill(Track::kNone);
    track.channel_ids.fill(0);
    track.note_flags = 0;
    track.last_note = 0;
    track.note_played = 0;
    track.gate = 0;
    InitEnv(track);
    track.transpose = 0;
    track.final_transpose = 0;
    track.ticks = -1;
    for (Port& port : track.ports)
    {
        port.import_flag = 0;
        port.export_flag = 0;
    }
    track.registered = false;

    notes_[index] = 0;
    jump_notes_[index].fill(0);
    looped_[index] = false;
    quiet_[index] = false;
    opens_[index]++;
}

int32_t Tracks::SetSeqData(uint32_t index, const Sequence& sequence)
{
    // Jaq_SetSeqData_Limit with the data in memory (mode 0) and no channels for the root's manager.
    if (index == Track::kNone)
    {
        index = GetNewTrack();
        if (index == Track::kNone)
        {
            return -1;
        }
    }
    else
    {
        tracks_[index].allocated = false;
    }

    // A root that the list can't hold leaves the track out of the pool, as the game does.
    const int32_t root = AllocNewRoot(index);
    if (root == -1)
    {
        return -1;
    }

    Track& track = tracks_[index];
    track.source = 0;
    track.sequence = &sequence;
    track.track_id = static_cast<uint32_t>(root);
    track.flags = 3;
    InitTrack(index, 0, Track::kNone);
    InitExtBuffer(static_cast<uint32_t>(root));
    AssignExtBuffer(index, static_cast<uint32_t>(root));
    channels_.InitOneShot(index, 0);
    track.tempo_counter = 0.0f;
    track.tempo_rate = 1.0f;
    UpdateTrackAll(index);
    track.state = Track::kReady;

    return root;
}

void Tracks::SetBankNumber(uint32_t index, uint8_t bank)
{
    Track& track = tracks_[index];
    track.registers[kRegBank] = static_cast<uint16_t>(bank << 8 | (track.registers[kRegBank] & 0xff));
}

bool Tracks::StartSeq(int32_t handle)
{
    if (handle < 0 || static_cast<uint32_t>(handle) >= kRoots)
    {
        return false;
    }

    const uint32_t index = roots_[handle];
    if (index == Track::kNone || tracks_[index].state != Track::kReady)
    {
        return false;
    }

    // Jac_RegisterPlayerCallback: the first free slot. The game refuses a callback that the list already holds.
    tracks_[index].state = Track::kPlaying;
    if (std::find(player_calls_.begin(), player_calls_.end(), index) != player_calls_.end())
    {
        return true;
    }

    for (uint32_t& slot : player_calls_)
    {
        if (slot == Track::kNone)
        {
            slot = index;
            break;
        }
    }

    return true;
}

bool Tracks::RegisterPlayerCall(PlayerCall call)
{
    for (std::size_t i = 0; i < kPlayerCalls; i++)
    {
        if (player_calls_[i] == Track::kNone)
        {
            player_calls_[i] = kReserved;
            game_calls_[i] = std::move(call);
            return true;
        }
    }

    return false;
}

void Tracks::RunPlayerCallbacks()
{
    // PlayerCallback (0x26a3d0).
    for (std::size_t i = 0; i < kPlayerCalls; i++)
    {
        const uint32_t slot = player_calls_[i];
        if (slot == Track::kNone)
        {
            continue;
        }

        const int32_t result = slot == kReserved ? (game_calls_[i] ? game_calls_[i]() : 0) : RootCallback(slot);
        if (result == -1)
        {
            player_calls_[i] = Track::kNone;
            game_calls_[i] = nullptr;
        }
    }
}

int32_t Tracks::OpenTrack(uint32_t index, uint32_t flags, uint32_t cursor)
{
    // Jaq_OpenTrack: the child's slot in the low 4 bits (a register's number with bit 5), its kind in bits 6 and 7,
    // or 4 with bit 5.
    uint8_t slot = flags & 0x0f;
    uint8_t kind = static_cast<uint8_t>((flags & 0xc0) >> 6);
    if ((flags & 0x20) != 0)
    {
        kind = 4;
        slot = static_cast<uint8_t>(ReadRegDirect(index, slot));
    }
    if (slot >= 16)
    {
        return -1;
    }

    Track& track = tracks_[index];
    if (track.children[slot] != Track::kNone)
    {
        CloseTrack(track.children[slot]);
    }

    const uint32_t child_index = GetNewTrack();
    if (child_index == Track::kNone)
    {
        unmodelled_++;
        return -1;
    }

    track.children[slot] = child_index;
    Track& child = tracks_[child_index];
    child.track_id = ((track.track_id << 4 | slot) & 0x0fffffff) | ((track.track_id & 0xf0000000) + 0x10000000);
    child.connection_id = 0;
    child.source = track.source;
    child.flags = kind;
    InitTrack(child_index, cursor, index);
    child.muted = track.muted || ((track.child_mutes >> slot) & 1) != 0;
    child.outer = track.child_outer[slot];
    if (child.outer != Track::kNone)
    {
        outer_[child.outer].references++;
    }
    channels_.InitOneShot(child_index, 0);
    UpdateTrackAll(child_index);

    return 0;
}

void Tracks::AllNoteOff(uint32_t index)
{
    // __AllNoteOff: a root's notes release over 10 frames, a child's stop.
    Track& track = tracks_[index];
    const uint16_t release = track.parent == Track::kNone ? 10 : 0;
    for (uint8_t i = 0; i < 8; i++)
    {
        NoteOff(index, i, release);
        track.notes[i] = kNoNote;
        track.channels[i] = Track::kNone;
    }
}

void Tracks::CloseTrack(uint32_t index)
{
    // Jaq_CloseTrack.
    if (index == Track::kNone || tracks_[index].state == Track::kClosed)
    {
        return;
    }

    AllNoteOff(index);
    BackTrack(index);
    Track& track = tracks_[index];
    for (uint32_t& child : track.children)
    {
        if (child != Track::kNone)
        {
            CloseTrack(child);
            child = Track::kNone;
        }
    }
    if (track.outer != Track::kNone)
    {
        outer_[track.outer].references--;
        track.outer = Track::kNone;
    }
    for (uint32_t& outer : track.child_outer)
    {
        if (outer != Track::kNone)
        {
            outer_[outer].assigned = false;
            outer = Track::kNone;
        }
    }
    track.muted = false;
    track.child_mutes = 0;
    if (track.parent != Track::kNone)
    {
        channels_.FixMoveAll(index, track.parent);
    }
    else
    {
        channels_.FixReleaseAll(index);
    }
    UnRegistTrack(index);
}

void Tracks::StopSequence(uint32_t index)
{
    // __StopSeq.
    SeqUpdate(index, 0);
    CloseTrack(index);
    DeAllocRoot(index);
}

int32_t Tracks::RootCallback(uint32_t index)
{
    // Jaq_RootCallback: the root's ticks at its tempo, each a tick of every track of the tree, then the update of
    // their levels.
    Track& track = tracks_[index];
    if (track.state == Track::kClosed)
    {
        return -1;
    }
    if (track.state == Track::kStopping)
    {
        StopSequence(index);
        return -1;
    }

    track.tempo_counter += track.tempo_rate;
    while (track.tempo_counter >= 1.0f)
    {
        track.tempo_counter -= 1.0f;
        if (SeqMain(index, false) == -1)
        {
            StopSequence(index);
            return -1;
        }
    }
    SeqUpdate(index, 0);

    return 0;
}

// jammain_2.

uint8_t Tracks::ReadByte(Track& track)
{
    return track.sequence->U8(track.cursor++);
}

uint16_t Tracks::Read16(Track& track)
{
    const uint16_t high = ReadByte(track);
    return static_cast<uint16_t>(high << 8 | ReadByte(track));
}

uint32_t Tracks::Read24(Track& track)
{
    const uint32_t high = ReadByte(track);
    const uint32_t middle = ReadByte(track);
    return high << 16 | middle << 8 | ReadByte(track);
}

uint8_t Tracks::ByteAt(const Track& track, uint32_t offset)
{
    return track.sequence->U8(offset);
}

uint16_t Tracks::WordAt(const Track& track, uint32_t offset)
{
    return static_cast<uint16_t>(ByteAt(track, offset) << 8 | ByteAt(track, offset + 1));
}

uint32_t Tracks::Word24At(const Track& track, uint32_t offset)
{
    return static_cast<uint32_t>(ByteAt(track, offset)) << 16 | static_cast<uint32_t>(ByteAt(track, offset + 1)) << 8 |
           ByteAt(track, offset + 2);
}

uint32_t Tracks::LongAt(const Track& track, uint32_t offset)
{
    return static_cast<uint32_t>(WordAt(track, offset)) << 16 | WordAt(track, offset + 2);
}

uint16_t Tracks::Register(const Track& track, uint32_t reg)
{
    if (reg < track.registers.size())
    {
        return track.registers[reg];
    }

    // Past the registers, the game reads the pointers of the track's outer parameters (registers 32 to 35) and of its
    // children's (36 to 99), then the ports. A null pointer reads as 0.
    if (reg < 36)
    {
        if (track.outer != Track::kNone)
        {
            unmodelled_++;
        }
        return 0;
    }
    if (reg < 100)
    {
        if (track.child_outer[(reg - 36) / 4] != Track::kNone)
        {
            unmodelled_++;
        }
        return 0;
    }

    unmodelled_++;

    return 0;
}

void Tracks::SetRegister(Track& track, uint32_t reg, uint16_t value)
{
    if (reg < track.registers.size())
    {
        track.registers[reg] = value;
        return;
    }

    unmodelled_++;
}

bool Tracks::Condition(uint32_t index, uint8_t condition)
{
    // __ConditionCheck on register 3.
    const uint16_t value = ReadRegDirect(index, kRegValue);
    switch (condition & 0x0f)
    {
    case 0:
        return true;
    case 1:
        return value == 0;
    case 2:
        return value != 0;
    case 3:
        return value == 1;
    case 4:
        return value >= 0x8000;
    case 5:
        return value < 0x8000;
    default:
        return false;
    }
}

int32_t Tracks::SeqTimeToDspTime(uint32_t index, int32_t time, uint8_t percent) const
{
    // Jam_SEQtimeToDSPtime: a note's time in ticks, times its gate in percent, in frames.
    const Track& track = tracks_[index];
    float frames = static_cast<float>(time) * static_cast<float>(percent) / 100.0f;
    if (track.time_relate == 1)
    {
        frames = frames / track.tempo_rate;
    }
    else
    {
        frames = frames * 120.0f / static_cast<float>(track.timebase);
    }

    return ToInt(frames);
}

void Tracks::WriteTimeParam(uint32_t index, uint8_t command)
{
    // Jam_WriteTimeParam (commands 0x90 to 0x9f, 0x26ae90 in the release's main): a timed parameter's target, from a
    // register, a byte, a byte in the high half or a word, and its time in ticks, none, from a register, a byte or a
    // word. The release's code fills the low half of a byte below 0x80 with twice the byte: 0x7f gives 0x7ffe. It
    // divides a target by 32,767.
    Track& track = tracks_[index];
    const uint8_t param = ReadByte(track);
    int16_t target = 0;
    switch (command & 0x0c)
    {
    case 0x00:
        target = static_cast<int16_t>(Register(track, ReadByte(track)));
        break;

    case 0x04:
        target = static_cast<int16_t>(ReadByte(track));
        break;

    case 0x08:
        {
            const uint8_t high = ReadByte(track);
            target = static_cast<int16_t>(high << 8 | (high < 0x80 ? high << 1 : 0));
        }
        break;

    default:
        target = static_cast<int16_t>(Read16(track));
        break;
    }

    int32_t time = 0;
    switch (command & 0x03)
    {
    case 0:
        time = -1;
        break;
    case 1:
        time = Register(track, ReadByte(track));
        break;
    case 2:
        time = ReadByte(track);
        break;
    default:
        time = Read16(track);
        break;
    }

    if (param >= track.timed.size())
    {
        unmodelled_++;
        return;
    }

    MoveParam& move = track.timed[param];
    move.target = static_cast<float>(target) / kShortMax;
    if (time >= 0)
    {
        move.time = static_cast<float>(time);
    }
    if (move.time <= 0.0f)
    {
        move.value = move.target;
        move.step = 0.0f;
        move.time = 1.0f;
    }
    else
    {
        move.step = (move.target - move.value) / move.time;
    }
}

void Tracks::WriteRegDirect(uint32_t index, uint8_t reg, uint16_t value)
{
    // Jam_WriteRegDirect: registers 0 to 2 hold a byte, and register 3 takes its sign extension. Registers 32 and 33
    // take nothing, and register 34 splits its value into registers 0 and 1. Register 3 takes every value.
    Track& track = tracks_[index];
    uint16_t result = value;
    switch (reg)
    {
    case 0:
    case 1:
    case 2:
        value &= 0xff;
        result = Extend8To16(value);
        break;

    case 32:
    case 33:
        return;

    case 34:
        WriteRegDirect(index, 0, static_cast<uint16_t>(value >> 8));
        result = value;
        value &= 0xff;
        reg = 1;
        break;

    default:
        break;
    }
    SetRegister(track, reg, value);
    track.registers[kRegValue] = result;
}

uint32_t Tracks::LoadTable(uint32_t index, uint32_t offset, uint32_t entry, uint32_t type)
{
    // LoadTbl: entry `entry` of a table of bytes, words, 24-bit words or 32-bit words. Type 8 reads a 32-bit word at
    // the entry's number as a byte offset.
    const Track& track = tracks_[index];
    switch (type)
    {
    case 4:
        return ByteAt(track, offset + entry);

    case 5:
        return WordAt(track, offset + entry * 2);

    case 6:
        return Word24At(track, offset + entry * 3);

    case 7:
        return LongAt(track, offset + entry * 4);

    case 8:
        return LongAt(track, offset + entry);

    default:
        unmodelled_++;
        return 0;
    }
}

void Tracks::WriteRegParam(uint32_t index, uint8_t command)
{
    // Jam_WriteRegParam (commands 0xa0 to 0xaf): an operation on a register. The low nibble of the command gives the
    // operation and the source's kind. 0xa9 and 0xaa read a second byte with the operation, and 0xaa loads from a
    // table. Each test reads the command byte as the one before it left it.
    Track& track = tracks_[index];
    uint32_t source = command & 0x0c;
    uint32_t operation = command & 0x03;
    uint32_t type = 0;
    uint32_t wide = 0;
    bool wide_set = false;
    if ((command & 0x0f) == 0x0b)
    {
        source = 0;
        operation = 0x0b;
    }
    if ((command & 0x0f) == 0x0a)
    {
        command = ReadByte(track);
        source = command & 0x0c;
        operation = 0x0a;
        type = static_cast<uint32_t>(command >> 4) + 4;
    }
    if ((command & 0x0f) == 0x09)
    {
        command = ReadByte(track);
        source = command & 0x0c;
        operation = command & 0xf0;
        if (source == 8)
        {
            source = 0x10;
        }
    }

    uint8_t reg = ReadByte(track);
    if (operation == 0x0a)
    {
        wide = ReadReg32(index, ReadByte(track));
        wide_set = true;
    }

    int16_t value = 0;
    switch (source)
    {
    case 0:
        value = static_cast<int16_t>(ReadRegDirect(index, ReadByte(track)));
        break;
    case 4:
        value = static_cast<int16_t>(ReadByte(track));
        break;
    case 8:
        value = static_cast<int16_t>(ReadByte(track) << 8);
        break;
    case 12:
        value = static_cast<int16_t>(Read16(track));
        break;
    default:
        value = -1;
        break;
    }

    const auto old = static_cast<int16_t>(ReadRegDirect(index, reg));
    const auto shift = [](int32_t amount)
    {
        return static_cast<uint32_t>(amount) & 31;
    };
    switch (operation)
    {
    case 0x00:
        break;

    case 0x01:
        if (source == 4)
        {
            value = static_cast<int16_t>(Extend8To16(static_cast<uint16_t>(value)));
        }
        value = static_cast<int16_t>(old + value);
        break;

    case 0x02:
        // The product goes to registers 4 and 5 (Jam_WriteRegXY).
        wide = static_cast<uint32_t>(static_cast<int32_t>(old) * static_cast<int32_t>(value));
        WriteRegDirect(index, kRegX, static_cast<uint16_t>(wide >> 16));
        WriteRegDirect(index, kRegY, static_cast<uint16_t>(wide));
        return;

    case 0x03:
        track.registers[kRegValue] = static_cast<uint16_t>(old - value);
        return;

    case 0x0b:
        value = static_cast<int16_t>(old - value);
        break;

    case 0x10:
        if (source == 4)
        {
            value = static_cast<int16_t>(Extend8To16(static_cast<uint16_t>(value)));
        }
        value = value < 0 ? static_cast<int16_t>(static_cast<uint32_t>(static_cast<uint16_t>(old)) >> shift(-value))
                          : static_cast<int16_t>(static_cast<uint32_t>(static_cast<uint16_t>(old)) << shift(value));
        break;

    case 0x20:
        if (source == 4)
        {
            value = static_cast<int16_t>(Extend8To16(static_cast<uint16_t>(value)));
        }
        value = value < 0 ? static_cast<int16_t>(static_cast<int32_t>(old) >> shift(-value))
                          : static_cast<int16_t>(static_cast<uint32_t>(static_cast<int32_t>(old)) << shift(value));
        break;

    case 0x30:
        value = static_cast<int16_t>(old & value);
        break;

    case 0x40:
        value = static_cast<int16_t>(old | value);
        break;

    case 0x50:
        value = static_cast<int16_t>(old ^ value);
        break;

    case 0x60:
        value = static_cast<int16_t>(-old);
        break;

    case 0x90:
        {
            // GetRandom_s32 modulo the source's value. The build's division by 0 gives 0, and the remainder is then the
            // draw.
            wide = static_cast<uint32_t>(random_.S32());
            wide_set = true;
            const uint32_t divisor = static_cast<uint16_t>(value);
            value = static_cast<int16_t>(divisor == 0 ? wide : wide % divisor);
            break;
        }

    case 0x0a:
        // The entry's number is the signed source value.
        wide = LoadTable(index, wide, static_cast<uint32_t>(static_cast<int32_t>(value)), type);
        value = static_cast<int16_t>(wide);
        break;

    default:
        break;
    }

    // Register 3 takes the result. The release's code gives it 0 for a half of the bank's or the priority's register
    // (0x26bffc and 0x26c050 in its main).
    uint16_t result = static_cast<uint16_t>(value);
    switch (reg)
    {
    case 0:
    case 1:
    case 2:
        value = static_cast<int16_t>(value & 0xff);
        result = Extend8To16(static_cast<uint16_t>(value));
        break;

    case 0x21:
        reg = kRegBank;
        value = static_cast<int16_t>((track.registers[kRegBank] & 0xff00) | (value & 0x00ff));
        result = 0;
        break;

    case 0x20:
        reg = kRegBank;
        value = static_cast<int16_t>((track.registers[kRegBank] & 0x00ff) | (value << 8));
        result = 0;
        break;

    case 0x2e:
        reg = kRegPriority;
        value = static_cast<int16_t>((track.registers[kRegPriority] & 0xff00) | (value & 0x00ff));
        result = 0;
        break;

    case 0x2f:
        reg = kRegPriority;
        value = static_cast<int16_t>((track.registers[kRegPriority] & 0x00ff) | (value << 8));
        result = 0;
        break;

    case 0x22:
        WriteRegDirect(index, 0, static_cast<uint16_t>(static_cast<uint16_t>(value) >> 8));
        value = static_cast<int16_t>(value & 0xff);
        result = static_cast<uint16_t>(value);
        reg = 1;
        break;

    case 0x28:
    case 0x29:
    case 0x2a:
    case 0x2b:
        {
            // A 32-bit register takes the draw or the table's word. After the other operations, the game writes
            // whatever its register held.
            if (!wide_set)
            {
                unmodelled_++;
                return;
            }
            const uint32_t at = kRegLong + 2 * (reg - 0x28u);
            track.registers[at] = static_cast<uint16_t>(wide >> 16);
            track.registers[at + 1] = static_cast<uint16_t>(wide);
            return;
        }

    default:
        break;
    }

    SetRegister(track, reg, static_cast<uint16_t>(value));
    track.registers[kRegValue] = result;
    if (reg == kRegBank)
    {
        ClearOverwrite(track);
    }
    if (reg == kRegBendRange)
    {
        track.update_flags |= kUpdatePitch;
    }
    if (reg == kRegPriority)
    {
        ChannelManager& manager = channels_.Manager(index);
        manager.priority = track.registers[kRegPriority] | 0x10000u;
        manager.release_time = 0;
    }
}

uint16_t Tracks::ReadRegDirect(uint32_t index, uint8_t reg)
{
    // Jam_ReadRegDirect: 0x20 the bank, 0x21 the program, 0x22 registers 0 and 1 as one word, 0x2c the children that
    // play, 0x2d the notes that have stopped, 0x30 the count of the current loop. The result is a 16-bit signed value,
    // and the bank's shift extends its sign.
    const Track& track = tracks_[index];
    int16_t result = 0;
    switch (reg)
    {
    case 0x20:
    case 0x21:
        result = static_cast<int16_t>(track.registers[kRegBank]);
        break;

    case 0x22:
        {
            const uint16_t low = ReadRegDirect(index, 1);
            const uint16_t high = ReadRegDirect(index, 0);
            result = static_cast<int16_t>(high << 8 | low);
            break;
        }

    case 0x2c:
        {
            uint32_t mask = 0;
            for (std::size_t i = 16; i-- > 0;)
            {
                const uint32_t child = track.children[i];
                mask = mask << 1 | (child != Track::kNone && tracks_[child].state != Track::kClosed ? 1 : 0);
            }
            result = static_cast<int16_t>(mask);
            break;
        }

    case 0x2d:
        {
            uint32_t mask = 0;
            for (uint8_t i = 8; i-- > 0;)
            {
                mask = mask << 1 | (CheckNoteStop(index, i) ? 1 : 0);
            }
            result = static_cast<int16_t>(mask);
            break;
        }

    case 0x30:
        result = track.depth == 0 ? 0 : static_cast<int16_t>(track.loop_counts[track.depth - 1]);
        break;

    default:
        result = static_cast<int16_t>(Register(track, reg));
        break;
    }

    switch (reg)
    {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x21:
        result = static_cast<int16_t>(result & 0xff);
        break;
    case 0x20:
        result = static_cast<int16_t>(result >> 8);
        break;
    default:
        break;
    }

    return static_cast<uint16_t>(result);
}

uint32_t Tracks::ReadReg32(uint32_t index, uint8_t reg)
{
    // Jam_ReadReg32: 40 to 43 the 32-bit registers, 35 registers 4 and 5 as one word.
    const Track& track = tracks_[index];
    if (reg >= 40 && reg <= 43)
    {
        const uint32_t at = kRegLong + 2 * (reg - 40u);
        return static_cast<uint32_t>(track.registers[at]) << 16 | track.registers[at + 1];
    }
    if (reg == 35)
    {
        return static_cast<uint32_t>(ReadRegDirect(index, kRegX)) << 16 | ReadRegDirect(index, kRegY);
    }

    return ReadRegDirect(index, reg);
}

uint32_t Tracks::ExchangeRegisterValue(uint32_t index, uint8_t reg)
{
    // __ExchangeRegisterValue: registers below 64, the ports' values from 64.
    if (reg < 64)
    {
        return ReadReg32(index, reg);
    }

    const uint32_t port = reg - 64u;
    if (port < 16)
    {
        return tracks_[index].ports[port].value;
    }

    unmodelled_++;

    return 0;
}

void Tracks::SetInterrupt(uint32_t index, uint16_t interrupt)
{
    Track& track = tracks_[index];
    const uint32_t bit = 1u << (interrupt & 31);
    if ((track.enabled & bit) != 0)
    {
        track.requests = static_cast<uint8_t>(track.requests | bit);
    }
}

bool Tracks::TryInterrupt(uint32_t index)
{
    // Jam_TryInterrupt: the lowest interrupt that is enabled and requested, unless a handler runs.
    Track& track = tracks_[index];
    if (track.interrupted != 0)
    {
        return false;
    }

    for (uint32_t i = 0; i < 8; i++)
    {
        const uint32_t bit = 1u << i;
        if ((track.enabled & bit) != 0 && (track.requests & bit) != 0)
        {
            track.saved_cursor = track.cursor;
            track.cursor = track.interrupt_targets[i];
            track.interrupted = static_cast<uint8_t>(bit);
            track.saved_wait = track.wait;
            track.wait = 0;
            track.requests = static_cast<uint8_t>(track.requests ^ bit);
            return true;
        }
    }

    return false;
}

bool Tracks::WritePortAppDirect(uint32_t index, uint32_t port, uint16_t value)
{
    if (index == Track::kNone)
    {
        return false;
    }
    if (port >= 16)
    {
        unmodelled_++;
        return true;
    }

    Track& track = tracks_[index];
    track.ports[port].value = value;
    track.ports[port].import_flag = 1;
    if (port == 0)
    {
        SetInterrupt(index, 3);
    }
    if (port == 1)
    {
        SetInterrupt(index, 4);
    }

    return true;
}

bool Tracks::ReadPortAppDirect(uint32_t index, uint32_t port, uint16_t& value)
{
    if (index == Track::kNone)
    {
        return false;
    }
    if (port >= 16)
    {
        unmodelled_++;
        return true;
    }

    Track& track = tracks_[index];
    track.ports[port].export_flag = 0;
    value = track.ports[port].value;

    return true;
}

bool Tracks::CheckPortAppDirect(uint32_t index, uint32_t port, uint16_t value)
{
    // Condition 0: whether the sequence has written the port. Condition 1: whether the game's last write has been
    // read.
    if (index == Track::kNone || port >= 16)
    {
        unmodelled_++;
        return false;
    }

    const Port& p = tracks_[index].ports[port];
    switch (value & 0xff)
    {
    case 0:
        return p.export_flag != 0;
    case 1:
        return p.import_flag != 1;
    default:
        return false;
    }
}

void Tracks::InitRegistTrack()
{
    registrations_ = 0;
    for (Registration& entry : registered_)
    {
        entry.track = Track::kNone;
    }
}

void Tracks::RegistTrack(uint32_t index, uint32_t id)
{
    // Jam_RegistTrack: an ID that the list holds keeps its track. A full list takes the first free entry.
    for (uint32_t i = 0; i < registrations_; i++)
    {
        if (registered_[i].id == id)
        {
            return;
        }
    }
    uint32_t at = registrations_;
    if (registrations_ == kRegistered)
    {
        at = 0;
        while (at < kRegistered && registered_[at].track != Track::kNone)
        {
            at++;
        }
        if (at == kRegistered)
        {
            return;
        }
    }
    else
    {
        registrations_++;
    }
    registered_[at].id = id;
    registered_[at].track = index;
    tracks_[index].registered = true;
}

void Tracks::UnRegistTrack(uint32_t index)
{
    Track& track = tracks_[index];
    if (!track.registered)
    {
        return;
    }

    for (uint32_t i = 0; i < registrations_; i++)
    {
        if (registered_[i].track == index)
        {
            registered_[i].track = Track::kNone;
        }
    }
    track.registered = false;
    while (registrations_ != 0 && registered_[registrations_ - 1].track == Track::kNone)
    {
        registrations_--;
    }
}

uint32_t Tracks::TrackHandle(uint32_t id) const
{
    for (uint32_t i = 0; i < registrations_; i++)
    {
        if (registered_[i].id == id && registered_[i].track != Track::kNone)
        {
            return registered_[i].track;
        }
    }

    return Track::kNone;
}

void Tracks::InitExtBuffer(uint32_t outer)
{
    OuterParam& param = outer_[outer];
    param.switches = 0;
    param.updates = 0;
    param.assigned = false;
    param.references = 0;
}

bool Tracks::AssignExtBuffer(uint32_t index, uint32_t outer)
{
    if (index == Track::kNone || outer == Track::kNone)
    {
        return false;
    }

    tracks_[index].outer = outer;
    outer_[outer].references++;

    return true;
}

uint32_t Tracks::AddOuter()
{
    outer_.emplace_back();
    const auto index = static_cast<uint32_t>(outer_.size() - 1);
    InitExtBuffer(index);

    return index;
}

bool Tracks::AssignExtBufferP(uint32_t index, uint8_t child, uint32_t outer)
{
    if (index == Track::kNone || outer == Track::kNone)
    {
        return false;
    }
    if (child >= 16)
    {
        unmodelled_++;
        return true;
    }

    Track& track = tracks_[index];
    track.child_outer[child] = outer;
    outer_[outer].assigned = true;
    AssignExtBuffer(track.children[child], outer);

    return true;
}

void Tracks::SetExtParamD(uint32_t outer, uint8_t flag, float value)
{
    if (outer == Track::kNone)
    {
        return;
    }

    OuterParam& param = outer_[outer];
    switch (flag)
    {
    case OuterParam::kVolume:
        param.volume = value;
        break;
    case OuterParam::kPitch:
        param.pitch = value;
        break;
    case OuterParam::kFxmix:
        param.fxmix = value;
        break;
    case OuterParam::kDolby:
        param.dolby = value;
        break;
    case OuterParam::kPan:
        param.pan = value;
        break;
    case OuterParam::kTempo:
        param.tempo = value;
        break;
    default:
        return;
    }
    param.updates = static_cast<uint16_t>(param.updates | flag);
}

void Tracks::SetExtParam(uint32_t index, uint8_t flag, float value)
{
    if (index == Track::kNone)
    {
        return;
    }

    SetExtParamD(tracks_[index].outer, flag, value);
}

void Tracks::OnExtSwitch(uint32_t index, uint16_t switches)
{
    if (index == Track::kNone)
    {
        return;
    }

    OnExtSwitchD(tracks_[index].outer, switches);
}

void Tracks::OffExtSwitch(uint32_t index, uint16_t switches)
{
    if (index == Track::kNone)
    {
        return;
    }

    OffExtSwitchD(tracks_[index].outer, switches);
}

void Tracks::SetExtParamP(uint32_t index, uint8_t child, uint8_t flag, float value)
{
    if (index == Track::kNone)
    {
        return;
    }
    if (child >= 16)
    {
        unmodelled_++;
        return;
    }

    SetExtParamD(tracks_[index].child_outer[child], flag, value);
}

void Tracks::OnExtSwitchP(uint32_t index, uint8_t child, uint16_t switches)
{
    if (index == Track::kNone)
    {
        return;
    }
    if (child >= 16)
    {
        unmodelled_++;
        return;
    }

    OnExtSwitchD(tracks_[index].child_outer[child], switches);
}

void Tracks::OnExtSwitchD(uint32_t outer, uint16_t switches)
{
    if (outer == Track::kNone)
    {
        return;
    }

    OuterParam& param = outer_[outer];
    param.switches = static_cast<uint16_t>(param.switches | switches);
    param.updates = static_cast<uint16_t>(param.updates | switches);
}

void Tracks::OffExtSwitchD(uint32_t outer, uint16_t switches)
{
    if (outer == Track::kNone)
    {
        return;
    }

    OuterParam& param = outer_[outer];
    param.switches = static_cast<uint16_t>(param.switches ^ (param.switches & switches));
    param.updates = static_cast<uint16_t>(param.updates | switches);
}

uint32_t Tracks::SequenceOuter(const Sequence* sequence, uint32_t offset)
{
    // Cmd_EXTSet's buffer lies in the sequence's data. The game reads its levels from the data's bytes until the code
    // above the sequences sets them. The model starts them at 0.
    const auto [entry, added] = sequence_outer_.try_emplace({sequence, offset}, 0);
    if (added)
    {
        outer_.emplace_back();
        entry->second = static_cast<uint32_t>(outer_.size() - 1);
    }

    return entry->second;
}

void Tracks::MuteTrack(uint32_t index, bool mute)
{
    Track& track = tracks_[index];
    if (track.parent != Track::kNone)
    {
        Track& parent = tracks_[track.parent];
        track.muted = parent.muted || mute;
        const auto bit = static_cast<uint16_t>(1u << (track.track_id & 0xf));
        if (!mute)
        {
            parent.child_mutes = static_cast<uint16_t>(parent.child_mutes & ~bit);
        }
        else
        {
            parent.child_mutes = static_cast<uint16_t>(parent.child_mutes | bit);
        }
    }
    else
    {
        track.muted = mute;
    }
    track.update_flags |= kUpdateVolume;
    if (track.muted && (track.pause_status & 0x20) != 0)
    {
        for (uint8_t i = 0; i < 8; i++)
        {
            NoteOff(index, i, 10);
        }
    }
}

void Tracks::PauseTrack(uint32_t index)
{
    Track& track = tracks_[index];
    track.paused = true;
    if ((track.pause_status & 0x01) != 0)
    {
        track.update_flags |= kUpdateVolume;
    }
    if ((track.pause_status & 0x04) != 0)
    {
        for (uint8_t i = 0; i < 8; i++)
        {
            NoteOff(index, i, 10);
        }
    }
    if ((track.pause_status & 0x08) != 0)
    {
        for (std::size_t i = 0; i < 8; i++)
        {
            const uint32_t channel = track.channels[i];
            if (channel != Track::kNone && track.channel_ids[i] == channels_.At(channel).id)
            {
                channels_.SetPause(channel, true);
            }
        }
    }
    SetInterrupt(index, 0);

    for (const uint32_t child : track.children)
    {
        if (child != Track::kNone && tracks_[child].state != Track::kClosed)
        {
            PauseTrack(child);
        }
    }
}

void Tracks::UnpauseTrack(uint32_t index)
{
    // Jam_UnPauseTrack (0x26d820) with its children's. It acts on a track that isn't paused too.
    Track& track = tracks_[index];
    track.update_flags |= kUpdateVolume;
    track.paused = false;

    for (std::size_t i = 0; i < 8; i++)
    {
        const uint32_t channel = track.channels[i];
        if (channel != Track::kNone && track.channel_ids[i] == channels_.At(channel).id)
        {
            channels_.SetPause(channel, false);
        }
    }

    SetInterrupt(index, 1);

    for (const uint32_t child : track.children)
    {
        if (child != Track::kNone && tracks_[child].state != Track::kClosed)
        {
            UnpauseTrack(child);
        }
    }
}

void Tracks::UpdateTrackAll(uint32_t index)
{
    // Jam_UpdateTrackAll (0x26c8f0): every level that the track's channel manager gives its channels. A track of kind
    // 4 takes its parent's channels and sets nothing.
    Track& track = tracks_[index];
    if (track.flags == 4)
    {
        return;
    }

    ChannelManager& manager = channels_.Manager(index);
    const float weight = static_cast<float>(track.registers[kRegArguments + 3]) / kShortMax;
    manager.priority = track.registers[kRegPriority] | 0x10000u;
    manager.release_time = 0;
    const std::array<uint8_t, 2> delays = Delays(track.timed[kTimedDelay].value);
    manager.delay_max = 0x10;
    manager.delays[0] = delays[0];
    manager.delays[1] = delays[1];

    float volume = track.muted ? 0.0f : track.timed[kTimedVolume].value;
    float pitch = PitchToCent(track.timed[kTimedPitch].value, static_cast<float>(track.registers[kRegBendRange]));
    float pan = track.timed[kTimedPan].value;
    float fxmix = track.timed[kTimedFxmix].value;
    float dolby = track.timed[kTimedDolby].value;
    if (track.outer != Track::kNone)
    {
        const OuterParam& outer = outer_[track.outer];
        if ((outer.switches & OuterParam::kVolume) != 0)
        {
            volume = volume * outer.volume;
        }
        if ((outer.switches & OuterParam::kPitch) != 0)
        {
            pitch = pitch * outer.pitch;
        }
        if ((outer.switches & OuterParam::kFxmix) != 0)
        {
            fxmix = PanCalc(fxmix, outer.fxmix, weight, track.pan_calcs[1]);
        }
        if ((outer.switches & OuterParam::kDolby) != 0)
        {
            dolby = PanCalc(dolby, outer.dolby, weight, track.pan_calcs[2]);
        }
        if ((outer.switches & OuterParam::kPan) != 0)
        {
            pan = PanCalc(pan, outer.pan, weight, track.pan_calcs[0]);
        }
    }

    if (track.parent == Track::kNone || (track.flags & 1) != 0)
    {
        manager.volume = volume;
        manager.pitch = pitch;
        manager.pan = pan;
        manager.fxmix = fxmix;
        manager.dolby = dolby;
        return;
    }

    const ChannelManager& parent = channels_.Manager(track.parent);
    const float parent_weight = static_cast<float>(track.registers[kRegArguments + 4]) / kShortMax;
    manager.volume = volume * parent.volume;
    manager.pitch = pitch * parent.pitch;
    manager.pan = PanCalc(pan, parent.pan, parent_weight, track.parent_pan_calcs[0]);
    manager.fxmix = PanCalc(fxmix, parent.fxmix, parent_weight, track.parent_pan_calcs[1]);
    manager.dolby = PanCalc(dolby, parent.dolby, parent_weight, track.parent_pan_calcs[2]);
    if (track.outer != Track::kNone && (outer_[track.outer].switches & OuterParam::kFir) != 0)
    {
        manager.fir = outer_[track.outer].fir;
        manager.filter_mode = 8;
    }
    for (std::size_t i = 0; i < 4; i++)
    {
        manager.iir[i] = static_cast<int16_t>(ToInt(track.timed[kTimedIir + i].value * kShortMax));
    }
    manager.filter_mode = static_cast<uint8_t>(manager.filter_mode | 0x20);
    manager.distortion = static_cast<int16_t>(ToInt(track.timed[kTimedDistortion].value * kShortMax));
}

void Tracks::UpdateTrack(uint32_t index, uint32_t flags)
{
    // Jam_UpdateTrack: the levels that `flags` names, and the levels that the track's oscillators move.
    Track& track = tracks_[index];
    if (track.flags == 4)
    {
        return;
    }

    ChannelManager& manager = channels_.Manager(index);
    const float weight = static_cast<float>(track.registers[kRegArguments + 3]) / kShortMax;
    if ((flags & kUpdateDelay) != 0)
    {
        const std::array<uint8_t, 2> delays = Delays(track.timed[kTimedDelay].value);
        manager.delays[0] = delays[0];
        manager.delays[1] = delays[1];
    }
    if ((flags & kUpdateTempo) != 0 && track.parent == Track::kNone)
    {
        UpdateTempo(index);
    }

    const OuterParam* outer = track.outer != Track::kNone ? &outer_[track.outer] : nullptr;
    const bool update_volume = (flags & kUpdateVolume) != 0;
    float volume = 0.0f;
    if (update_volume)
    {
        volume = track.muted ? 0.0f : track.timed[kTimedVolume].value;
        if (outer != nullptr && (outer->switches & OuterParam::kVolume) != 0)
        {
            volume *= outer->volume;
        }
        if (track.paused && (track.pause_status & 1) != 0)
        {
            volume *= track.timed[kTimedPause].value;
        }
    }

    const bool update_pitch = (flags & kUpdatePitch) != 0;
    float pitch = 0.0f;
    if (update_pitch)
    {
        pitch = PitchToCent(track.timed[kTimedPitch].value, static_cast<float>(track.registers[kRegBendRange]));
        if (outer != nullptr && (outer->switches & OuterParam::kPitch) != 0)
        {
            pitch = pitch * outer->pitch;
        }
    }

    const bool update_pan = (flags & kUpdatePan) != 0;
    float pan = 0.0f;
    if (update_pan)
    {
        pan = track.timed[kTimedPan].value;
        if (outer != nullptr && (outer->switches & OuterParam::kPan) != 0)
        {
            pan = PanCalc(pan, outer->pan, weight, track.pan_calcs[0]);
        }
    }

    const bool update_fxmix = (flags & kUpdateFxmix) != 0;
    float fxmix = 0.0f;
    if (update_fxmix)
    {
        fxmix = track.timed[kTimedFxmix].value;
        if (outer != nullptr && (outer->switches & OuterParam::kFxmix) != 0)
        {
            fxmix = PanCalc(fxmix, outer->fxmix, weight, track.pan_calcs[1]);
        }
    }

    const bool update_dolby = (flags & kUpdateDolby) != 0;
    float dolby = 0.0f;
    if (update_dolby)
    {
        dolby = track.timed[kTimedDolby].value;
        if (outer != nullptr && (outer->switches & OuterParam::kDolby) != 0)
        {
            dolby = PanCalc(dolby, outer->dolby, weight, track.pan_calcs[2]);
        }
    }

    if ((flags & kUpdateIir) != 0)
    {
        for (std::size_t i = 0; i < 4; i++)
        {
            manager.iir[i] = static_cast<int16_t>(ToInt(track.timed[kTimedIir + i].value * kShortMax));
        }
        manager.filter_mode = static_cast<uint8_t>(manager.filter_mode | 0x20);
    }
    if (outer != nullptr && (flags & kUpdateFir) != 0 && (outer->switches & OuterParam::kFir) != 0)
    {
        manager.fir = outer->fir;
        manager.filter_mode = static_cast<uint8_t>((manager.filter_mode & 0x20) + 8);
    }
    if ((flags & kUpdateDistortion) != 0)
    {
        manager.distortion = static_cast<int16_t>(ToInt(track.timed[kTimedDistortion].value * kShortMax));
    }

    for (std::size_t i = 0; i < 2; i++)
    {
        if (track.osc_routes[i] != 0x0e)
        {
            continue;
        }

        const float offset = OscToOfs(&track.oscillators[i], track.osc_states[i]);
        switch (track.oscillators[i].target)
        {
        case OscData::kVolume:
            volume = volume * offset;
            break;
        case OscData::kPitch:
            pitch = pitch * offset;
            break;
        case OscData::kPan:
            pan = pan * offset;
            break;
        case OscData::kFxmix:
            fxmix = fxmix * offset;
            break;
        case OscData::kDolby:
            dolby = dolby * offset;
            break;
        default:
            break;
        }
    }

    if (track.parent == Track::kNone || (track.flags & 1) != 0)
    {
        if (update_volume)
        {
            manager.volume = volume;
        }
        if (update_pitch)
        {
            manager.pitch = pitch;
        }
        if (update_pan)
        {
            manager.pan = pan;
        }
        if (update_fxmix)
        {
            manager.fxmix = fxmix;
        }
        if (update_dolby)
        {
            manager.dolby = dolby;
        }
        return;
    }

    const ChannelManager& parent = channels_.Manager(track.parent);
    const float parent_weight = static_cast<float>(track.registers[kRegArguments + 4]) / kShortMax;
    if (update_volume)
    {
        manager.volume = parent.volume * volume;
    }
    if (update_pitch)
    {
        manager.pitch = parent.pitch * pitch;
    }
    if (update_pan)
    {
        manager.pan = PanCalc(pan, parent.pan, parent_weight, track.parent_pan_calcs[0]);
    }
    if (update_fxmix)
    {
        manager.fxmix = PanCalc(fxmix, parent.fxmix, parent_weight, track.parent_pan_calcs[1]);
    }
    if (update_dolby)
    {
        manager.dolby = PanCalc(dolby, parent.dolby, parent_weight, track.parent_pan_calcs[2]);
    }
}

void Tracks::UpdateTempo(uint32_t index)
{
    // Jam_UpdateTempo: a root's ticks per sub-frame, from its timebase and tempo, times the outer tempo. Each child
    // copies its parent's.
    Track& track = tracks_[index];
    if (track.parent == Track::kNone)
    {
        track.tempo_rate =
            static_cast<float>(track.timebase) * static_cast<float>(track.tempo) / (kDacRate * 60.0f / 80.0f);
        if (track.outer != Track::kNone && (outer_[track.outer].switches & OuterParam::kTempo) != 0)
        {
            track.tempo_rate = track.tempo_rate * outer_[track.outer].tempo;
        }
    }
    else
    {
        track.tempo_rate = tracks_[track.parent].tempo_rate;
        track.timebase = tracks_[track.parent].timebase;
    }
    for (const uint32_t child : track.children)
    {
        if (child != Track::kNone && tracks_[child].state != Track::kClosed)
        {
            UpdateTempo(child);
        }
    }
}

// noteon.

int32_t Tracks::NoteOn(uint32_t index, uint8_t slot, uint8_t key, uint8_t velocity, int32_t time)
{
    // NoteON: a channel from the track's manager, or from the nearest manager up the tree that has a free one, which
    // moves to the track's manager first. A track of kind 4 plays on its parent's manager.
    Track& track = tracks_[index];
    if (track.muted && (track.pause_status & 0x40) != 0)
    {
        return -1;
    }

    if (track.channels[slot] != Track::kNone)
    {
        NoteOff(index, slot, 0);
    }

    const uint32_t parent = track.parent;
    uint32_t manager = index;
    uint32_t up = parent;
    while (channels_.Manager(manager).count == 0 || channels_.Manager(manager).lists[ChannelManager::kFree] == kNone)
    {
        if (up == Track::kNone)
        {
            manager = index;
            break;
        }

        manager = up;
        up = tracks_[up].parent;
    }

    // The manager that the note plays on: the track's, or its parent's for a track of kind 4. A channel from another
    // manager goes to the track's free list either way, and counts in the manager that the note plays on.
    uint32_t target = index;
    if (track.flags == 4)
    {
        if (parent == Track::kNone)
        {
            return -1;
        }

        target = parent;
    }

    if (manager != target)
    {
        const uint32_t channel = channels_.ListGet(manager, ChannelManager::kFree);
        if (channel != kNone)
        {
            channels_.Manager(manager).count--;
            channels_.ListAdd(index, ChannelManager::kFree, channel);
            channels_.Manager(target).count++;
            channels_.At(channel).manager = target;
        }
        manager = target;
    }

    const uint16_t bank_program = ReadRegDirect(index, kRegBank);
    const uint32_t bank = banks_.PhysicalBank(bank_program >> 8) & 0xffu;
    const uint32_t program = bank_program & 0xffu;
    const uint32_t channel = channels_.Play(manager, bank, program, key, velocity, time);
    track.channels[slot] = channel;
    if (channel == kNone)
    {
        if (program < 0xf0 && channels_.CheckInstWave(bank, program, key, velocity) != 0)
        {
            missing_notes_++;
        }
        return -1;
    }

    track.channel_ids[slot] = channels_.At(channel).id;
    channels_.SetPanPower(channel, static_cast<float>(track.registers[kRegArguments + 0]),
                          static_cast<float>(track.registers[kRegArguments + 1]),
                          static_cast<float>(track.registers[kRegArguments + 2]));

    // The track's oscillators on the channel's: a route below 4 names the channel's oscillator. From 4, the track
    // first takes that oscillator's settings and keeps its release. From 8, it takes them all.
    for (std::size_t i = 0; i < 2; i++)
    {
        uint32_t route = track.osc_routes[i];
        if (route == 15 || route == 14)
        {
            continue;
        }

        if (route >= 8)
        {
            route -= 8;
            if (route >= 4)
            {
                unmodelled_++;
            }
            else if (const OscData* data = channels_.At(channel).oscillators[route]; data != nullptr)
            {
                track.oscillators[i] = *data;
            }
        }
        else if (route >= 4)
        {
            route -= 4;
            const OscTable release = track.oscillators[i].release;
            if (const OscData* data = channels_.At(channel).oscillators[route]; data != nullptr)
            {
                track.oscillators[i] = *data;
                track.oscillators[i].release = release;
            }
        }
        if (route < 4)
        {
            channels_.OverwriteOscillator(channel, &track.oscillators[i], route);
        }
    }

    UpdateTrack(index, kUpdateVolume | kUpdatePitch);
    channels_.ResetInitialVolume(channel);

    return 0;
}

bool Tracks::NoteOff(uint32_t index, uint8_t slot, uint16_t release)
{
    // NoteOFF_R: a channel that still plays the track's note stops, or releases over `release` frames.
    Track& track = tracks_[index];
    const uint32_t channel = track.channels[slot];
    if (channel == Track::kNone)
    {
        return false;
    }

    if (channels_.At(channel).id == track.channel_ids[slot])
    {
        if (release == 0)
        {
            channels_.Stop(channel);
        }
        else
        {
            channels_.StopRelease(channel, release);
        }
    }
    track.channels[slot] = Track::kNone;
    track.channel_ids[slot] = 0;

    return true;
}

int32_t Tracks::GateOn(uint32_t index, uint8_t slot, uint8_t key, uint8_t velocity, int32_t time)
{
    // GateON: a tied note goes on in the slot's channel, whichever note that channel now plays.
    const uint32_t channel = tracks_[index].channels[slot];
    if (channel == Track::kNone)
    {
        return -1;
    }

    channels_.Gate(channel, key, velocity, time);

    return 0;
}

bool Tracks::CheckNoteStop(uint32_t index, uint8_t slot)
{
    Track& track = tracks_[index];
    const uint32_t channel = track.channels[slot];
    if (channel == Track::kNone)
    {
        return true;
    }
    if (channels_.At(channel).id != track.channel_ids[slot])
    {
        track.channels[slot] = Track::kNone;
        track.channel_ids[slot] = 0;
        return true;
    }

    return channels_.At(channel).key == 0xff;
}

// jamosc.

OscTable Tracks::SequenceTable(const Track& track, uint32_t offset)
{
    const std::span<const OscPoint> points = track.sequence->Table(offset);
    return {points.data(), points.size()};
}

void Tracks::UpdateOscParam(Track& track, uint32_t param, float value)
{
    // Osc_Update_Param: timed parameters 6 to 11, the two oscillators' width, rate and vertex.
    if (param < 6 || param > 11)
    {
        return;
    }

    OscData& data = track.oscillators[(param - 6) / 3];
    switch ((param - 6) % 3)
    {
    case 0:
        data.width = value;
        break;
    case 1:
        data.rate = value;
        break;
    default:
        data.vertex = value;
        break;
    }
}

void Tracks::SetupSimple(Track& track, uint8_t kind)
{
    switch (kind)
    {
    case 0:
        track.oscillators[1] = kVibratoDef;
        break;
    case 1:
        track.oscillators[0] = kTremoloDef;
        break;
    case 2:
        track.oscillators[1] = kTremoloDef;
        break;
    default:
        break;
    }
}

void Tracks::SetupSimpleEnv(Track& track, uint8_t kind, uint32_t offset)
{
    switch (kind)
    {
    case 0:
        track.oscillators[0] = kEnvelopeDef;
        track.oscillators[0].attack = SequenceTable(track, offset);
        break;

    case 1:
        track.oscillators[0].release = SequenceTable(track, offset);
        break;

    default:
        break;
    }
}

void Tracks::SetupAdsr(Track& track, const std::array<int16_t, 5>& times)
{
    // Osc_Setup_ADSR: the attack's time, the decay's time, the time to the sustain and its level, and the release's
    // time, in the track's tables.
    track.oscillators[0] = kAdsrDef;
    track.adsr_attack = kAdsTable;
    track.adsr_release = kRelTable;
    track.oscillators[0].attack = TableOf(track.adsr_attack);
    track.oscillators[0].release = TableOf(track.adsr_release);
    track.adsr_attack[0].time = times[0];
    track.adsr_attack[1].time = times[1];
    track.adsr_attack[2].time = times[2];
    track.adsr_attack[2].value = times[3];
    track.adsr_release[0].time = times[4];
    for (std::size_t i = 0; i < track.adsr_attack.size(); i++)
    {
        track.attack_table[3 * i] = track.adsr_attack[i].mode;
        track.attack_table[3 * i + 1] = track.adsr_attack[i].time;
        track.attack_table[3 * i + 2] = track.adsr_attack[i].value;
    }
    for (std::size_t i = 0; i < track.adsr_release.size(); i++)
    {
        track.release_table[3 * i] = track.adsr_release[i].mode;
        track.release_table[3 * i + 1] = track.adsr_release[i].time;
        track.release_table[3 * i + 2] = track.adsr_release[i].value;
    }
}

void Tracks::SetupFull(Track& track, uint8_t flags, uint32_t attack, uint32_t release)
{
    // Osc_Setup_Full: bit 4 picks the oscillator. Bit 7 gives it the default envelope's settings and the target in
    // the low 4 bits, bit 6 the attack table at `attack` and bit 5 the release table at `release`. An offset of 0
    // names the sequence's start, as any other offset does.
    OscData& data = track.oscillators[(flags >> 4) & 1];
    if ((flags & 0x80) != 0)
    {
        data = kEnvelopeDef;
        data.target = flags & 0x0f;
        if ((flags & 0x0f) == 1)
        {
            data.vertex = 1.0f;
        }
    }
    if ((flags & 0x40) != 0)
    {
        data.attack = SequenceTable(track, attack);
    }
    if ((flags & 0x20) != 0)
    {
        data.release = SequenceTable(track, release);
    }
}

void Tracks::ClearOverwrite(Track& track)
{
    track.osc_routes = {15, 15};
}

void Tracks::InitEnv(Track& track)
{
    track.oscillators[0] = kEnvelopeDef;
    ClearOverwrite(track);
}

// The parser.

void Tracks::JumpBack(uint32_t index, uint32_t target)
{
    // The cursor has moved past the jump. A jump to the jump itself or before it goes back. The track's levels below
    // the jump are those of the calls and the counted loops that the jump stays inside.
    const Track& track = tracks_[index];
    if (!loop_hook_ || target >= track.cursor)
    {
        return;
    }

    LoopJump jump;
    jump.target = target;
    jump.depth = track.depth;
    std::copy_n(track.stack.begin(), track.depth, jump.stack.begin());
    std::copy_n(track.loop_counts.begin(), track.depth, jump.counts.begin());
    uint32_t& notes = jump_notes_[index][track.depth];
    jump.played = notes_[index] != notes;
    notes = notes_[index];
    looped_[index] = true;
    if (track.depth == 0)
    {
        quiet_[index] = !jump.played;
    }
    loop_hook_(index, jump);
}

int32_t Tracks::CallF(uint32_t index)
{
    // Cmd_CallF and Cmd_JmpF: a condition in the low nibble of a flag byte, then the target: a 24-bit offset, a
    // register (bit 7), or an entry of a table of 24-bit offsets by a register (bit 6), the table at a 24-bit offset
    // or a register's (bit 5). Only the call pushes the return.
    Track& track = tracks_[index];
    const uint8_t flags = ReadByte(track);
    uint32_t target = 0;
    if ((flags & 0x80) != 0)
    {
        target = ReadRegDirect(index, ReadByte(track));
        if ((flags & 0x40) != 0)
        {
            uint32_t table = 0;
            if ((flags & 0x20) != 0)
            {
                table = ReadRegDirect(index, ReadByte(track));
            }
            else
            {
                table = Read24(track);
            }
            target = Word24At(track, table + target * 3);
        }
    }
    else
    {
        target = Read24(track);
    }

    if (Condition(index, flags & 0x0f))
    {
        if (command_ == 0xc0 + kCallF)
        {
            if (track.depth >= track.stack.size())
            {
                unmodelled_++;
                return kContinue;
            }
            track.stack[track.depth++] = track.cursor;
        }
        else
        {
            JumpBack(index, target);
        }
        track.cursor = target;
    }

    return kContinue;
}

void Tracks::Printf(uint32_t index)
{
    // Cmd_Printf: the format, at most 128 characters, then a byte for each conversion. %r and %R read a register. The
    // model prints nothing.
    Track& track = tracks_[index];
    std::vector<uint8_t> kinds;
    for (uint32_t i = 0; i < 128; i++)
    {
        const uint8_t c = ReadByte(track);
        if (c == 0)
        {
            break;
        }
        if (c == '\\')
        {
            if (ReadByte(track) == 0)
            {
                break;
            }
            continue;
        }
        if (c != '%')
        {
            continue;
        }

        i++;
        const uint8_t conversion = ReadByte(track);
        if (conversion == 0)
        {
            break;
        }
        kinds.push_back(conversion);
    }
    for (const uint8_t kind : kinds)
    {
        const uint8_t value = ReadByte(track);
        if (kind == 'r' || kind == 'R')
        {
            ExchangeRegisterValue(index, value);
        }
    }
}

int32_t Tracks::CommandProcess(uint32_t index, uint8_t command, uint16_t mask)
{
    // Cmd_Process: the command's arguments by its list, each type ORed with `mask`, then the command.
    Track& track = tracks_[index];
    if (command < 0xc0)
    {
        unmodelled_++;
        return kContinue;
    }

    const ArgSpec& spec = kArgs[command - 0xc0];
    uint32_t types = spec.types | mask;
    for (uint32_t i = 0; i < spec.count; i++)
    {
        uint32_t arg = 0;
        switch (types & 3)
        {
        case 0:
            arg = ReadByte(track);
            break;
        case 1:
            arg = Read16(track);
            break;
        case 2:
            arg = Read24(track);
            break;
        default:
            arg = ExchangeRegisterValue(index, ReadByte(track));
            break;
        }
        types >>= 2;
        args_[i] = arg;
    }
    command_ = command;

    return Command(index, static_cast<uint8_t>(command - 0xc0));
}

int32_t Tracks::RegCommandProcess(uint32_t index, bool from_register, uint32_t count)
{
    // RegCmd_Process (commands 0xb0 to 0xbf): a command, from a register with bit 3, and a byte whose top bits turn
    // its first arguments into registers.
    Track& track = tracks_[index];
    auto command = static_cast<uint8_t>(ReadByte(track));
    if (from_register)
    {
        command = static_cast<uint8_t>(ExchangeRegisterValue(index, command));
    }
    uint16_t mask = 0;
    if (!from_register || count != 0)
    {
        uint8_t bits = ReadByte(track);
        uint16_t pair = 3;
        for (uint32_t i = 0; i < count + 1; i++)
        {
            if ((bits & 0x80) != 0)
            {
                mask = static_cast<uint16_t>(mask | pair);
            }
            bits = static_cast<uint8_t>(bits << 1);
            pair = static_cast<uint16_t>(pair << 2);
        }
    }

    return CommandProcess(index, command, mask);
}

int32_t Tracks::Command(uint32_t index, uint8_t command)
{
    Track& track = tracks_[index];
    const std::array<uint32_t, 8>& a = args_;
    switch (command)
    {
    case kOpenTrack:
        OpenTrack(index, a[0], a[1]);
        return kContinue;

    case kOpenTrackBros:
        if (track.parent == Track::kNone)
        {
            return 0x40;
        }
        OpenTrack(track.parent, a[0], a[1]);
        return kContinue;

    case kCall:
        if (track.depth >= track.stack.size())
        {
            unmodelled_++;
            return kContinue;
        }
        track.stack[track.depth++] = track.cursor;
        track.cursor = a[0];
        return kContinue;

    case kCallF:
    case kJmpF:
        return CallF(index);

    case kRet:
        if (track.depth == 0)
        {
            unmodelled_++;
            return kContinue;
        }
        track.cursor = track.stack[--track.depth];
        return kContinue;

    case kRetF:
        if (Condition(index, a[0] & 0x0f))
        {
            if (track.depth == 0)
            {
                unmodelled_++;
                return kContinue;
            }
            track.cursor = track.stack[--track.depth];
        }
        return kContinue;

    case kJmp:
        // The game jumps to the second argument, which the last command with two or more left. Its one argument goes
        // unused.
        JumpBack(index, a[1]);
        track.cursor = a[1];
        return kContinue;

    case kLoopS:
        if (track.depth >= track.stack.size())
        {
            unmodelled_++;
            return kContinue;
        }
        track.stack[track.depth] = track.cursor;
        track.loop_counts[track.depth++] = static_cast<uint16_t>(a[0]);
        return kContinue;

    case kLoopE:
        {
            if (track.depth == 0)
            {
                return 0x80;
            }
            uint16_t count = track.loop_counts[track.depth - 1];
            if (count != 0)
            {
                count--;
            }
            if (count == 0)
            {
                track.depth--;
                return kContinue;
            }
            track.loop_counts[track.depth - 1] = count;
            track.cursor = track.stack[track.depth - 1];
            return kContinue;
        }

    case kReadPort:
        {
            if (a[0] >= 16)
            {
                unmodelled_++;
                return kContinue;
            }
            const uint16_t value = track.ports[a[0]].value;
            track.ports[a[0]].import_flag = 0;
            WriteRegDirect(index, static_cast<uint8_t>(a[1]), value);
            return kContinue;
        }

    case kWritePort:
        if (a[0] >= 16)
        {
            unmodelled_++;
            return kContinue;
        }
        track.ports[a[0]].value = static_cast<uint16_t>(a[1]);
        track.ports[a[0]].export_flag = 1;
        return kContinue;

    case kCheckPortImport:
    case kCheckPortExport:
        if (a[0] >= 16)
        {
            unmodelled_++;
            return kContinue;
        }
        WriteRegDirect(index, kRegValue,
                       command == kCheckPortImport ? track.ports[a[0]].import_flag : track.ports[a[0]].export_flag);
        return kContinue;

    case kWaitReg:
    case kWait3:
        track.wait = static_cast<int32_t>(a[0]);
        return a[0] != 0 ? kEndTick : kContinue;

    case kConnectName:
        track.connection_id = a[0] << 16 | a[1];
        return kContinue;

    case kParentWritePort:
        WritePortAppDirect(track.parent, a[0] & 0x0f, static_cast<uint16_t>(a[1]));
        return kContinue;

    case kChildWritePort:
        {
            const uint32_t child = track.children[(a[0] >> 4) & 0x0f];
            WritePortAppDirect(child, a[0] & 0x0f, static_cast<uint16_t>(a[1]));
            return kContinue;
        }

    case kSetLastNote:
        track.last_note = static_cast<uint8_t>(a[0] + track.final_transpose);
        return kContinue;

    case kTimeRelate:
        track.time_relate = static_cast<uint8_t>(a[0]);
        return kContinue;

    case kSimpleOsc:
        SetupSimple(track, static_cast<uint8_t>(a[0]));
        return kContinue;

    case kSimpleEnv:
        SetupSimpleEnv(track, static_cast<uint8_t>(a[0]), a[1]);
        return kContinue;

    case kSimpleAdsr:
        SetupAdsr(track, {static_cast<int16_t>(a[0]), static_cast<int16_t>(a[1]), static_cast<int16_t>(a[2]),
                          static_cast<int16_t>(a[3]), static_cast<int16_t>(a[4])});
        return kContinue;

    case kTranspose:
        // The child adds its parent's transpose, not the parent's sum.
        track.transpose = static_cast<int8_t>(a[0]);
        track.final_transpose = static_cast<uint8_t>(
            track.parent != Track::kNone ? tracks_[track.parent].transpose + track.transpose : track.transpose);
        return kContinue;

    case kCloseTrack:
        {
            const auto slot = static_cast<uint8_t>(a[0]);
            if (slot >= 16)
            {
                return 0x80;
            }
            CloseTrack(track.children[slot]);
            track.children[slot] = Track::kNone;
            return kContinue;
        }

    case kOutSwitch:
        if (track.outer != Track::kNone)
        {
            outer_[track.outer].switches = static_cast<uint16_t>(a[0]);
            outer_[track.outer].updates = 0xffff;
        }
        return kContinue;

    case kUpdateSync:
        UpdateTrack(index, a[0]);
        return kContinue;

    case kBusConnect:
        if (a[0] < 6)
        {
            channels_.Manager(index).mix_configs[a[0]] = static_cast<uint16_t>(a[1]);
        }
        return kContinue;

    case kPauseStatus:
        track.pause_status = static_cast<uint8_t>(a[0]);
        return kContinue;

    case kSetInterrupt:
        track.enabled = static_cast<uint8_t>(track.enabled | 1u << (a[0] & 31));
        if (a[0] >= 8)
        {
            unmodelled_++;
            return kContinue;
        }
        track.interrupt_targets[a[0]] = a[1];
        return kContinue;

    case kDisInterrupt:
        track.enabled = static_cast<uint8_t>(track.enabled & ~static_cast<uint8_t>(1u << (a[0] & 31)));
        return kContinue;

    case kClrI:
        track.interrupted = 0;
        return kContinue;

    case kSetI:
        track.interrupted = 1;
        return kContinue;

    case kRetI:
        track.wait = track.saved_wait;
        track.interrupted = 0;
        track.cursor = track.saved_cursor;
        return kRetry;

    case kIntTimer:
        track.timer_count = static_cast<uint8_t>(a[0]);
        track.timer = a[1];
        track.timer_reload = a[1];
        return kContinue;

    case kConnectOpen:
        RegistTrack(index, track.connection_id);
        return kContinue;

    case kConnectClose:
        UnRegistTrack(index);
        return kContinue;

    case kSyncCpu:
        {
            const uint16_t result = callback_ ? callback_(index, static_cast<uint16_t>(a[0])) : 0xffff;
            WriteRegDirect(index, kRegValue, result);
            return kContinue;
        }

    case kFlushAll:
        channels_.AllStop(index);
        channels_.FlushRelease(index);
        return kContinue;

    case kFlushRelease:
        channels_.FlushRelease(index);
        return kContinue;

    case kTimeBase:
        track.timebase = static_cast<uint16_t>(a[0]);
        if (track.parent == Track::kNone)
        {
            UpdateTempo(index);
        }
        return kContinue;

    case kTempo:
        track.tempo = static_cast<uint16_t>(a[0]);
        if (track.parent == Track::kNone)
        {
            UpdateTempo(index);
        }
        else
        {
            track.tempo_sync = true;
        }
        return kContinue;

    case kFinish:
        SeqUpdate(index, StepTimed(index));
        return kEndTrack;

    case kNop:
        return kContinue;

    case kPanPowSet:
        for (std::size_t i = 0; i < 3; i++)
        {
            track.registers[kRegArguments + i] = static_cast<uint16_t>(a[i]);
        }
        for (std::size_t i = 3; i < 5; i++)
        {
            const float value = static_cast<float>(a[i]) * 327.67f;
            track.registers[kRegArguments + i] = static_cast<uint16_t>(static_cast<uint32_t>(ToInt(value)));
        }
        return kContinue;

    case kIirSet:
        // The release's code divides each coefficient by 32,767.
        for (std::size_t i = 0; i < 4; i++)
        {
            MoveParam& iir = track.timed[kTimedIir + i];
            iir.target = static_cast<float>(static_cast<int16_t>(a[i])) / kShortMax;
            iir.value = iir.target;
            iir.step = 0.0f;
            iir.time = 1.0f;
        }
        return kContinue;

    case kFirSet:
        if (track.outer != Track::kNone)
        {
            // Jam_SetExtFirFilterD: 8 coefficients from the sequence's data, in the host's order.
            OuterParam& outer = outer_[track.outer];
            outer.updates = static_cast<uint16_t>(outer.updates | OuterParam::kFir);
            outer.switches = static_cast<uint16_t>(outer.switches | OuterParam::kFir);
            for (std::size_t i = 0; i < 8; i++)
            {
                outer.fir[i] = static_cast<int16_t>(track.sequence->Native16(a[0] + 2 * static_cast<uint32_t>(i)));
            }
        }
        return kContinue;

    case kExtSet:
        {
            const uint32_t outer = SequenceOuter(track.sequence, a[0]);
            InitExtBuffer(outer);
            AssignExtBuffer(index, outer);
            return kContinue;
        }

    case kPanSwSet:
        for (std::size_t i = 0; i < 3; i++)
        {
            const uint32_t type = (a[i] >> 5) & 7;
            track.pan_calcs[i] = kOscFlags[kCalcTypes + type];
            track.parent_pan_calcs[i] = kOscFlags[kParentCalcTypes + type];
            channels_.Manager(index).pan_calcs[i] = static_cast<uint8_t>(a[i] & 0x1f);
            track.update_flags |= kUpdatePan;
        }
        return kContinue;

    case kOscRoute:
        {
            const uint8_t route = a[0] & 0x0f;
            const uint32_t oscillator = (a[0] >> 4) & 0x0f;
            if (oscillator >= 2)
            {
                unmodelled_++;
                return kContinue;
            }
            track.osc_routes[oscillator] = route;
            if (route == 14)
            {
                track.osc_states[oscillator].state = OscState::kStart;
            }
            return kContinue;
        }

    case kIirCutOff:
        {
            const uint32_t filter = a[0] & 0xff;
            if (filter >= jaudio1::kCutoffToIir.size() / 4)
            {
                unmodelled_++;
                return kContinue;
            }

            // CUTOFF_TO_IIR_TABLE, at 0x6afe1c in main's data: the table of Pikmin 2's JASPlayer.
            for (std::size_t i = 0; i < 4; i++)
            {
                MoveParam& iir = track.timed[kTimedIir + i];
                iir.target = static_cast<float>(jaudio1::kCutoffToIir[filter * 4 + i]) / kShortMax;
                iir.value = iir.target;
                iir.step = 0.0f;
                iir.time = 1.0f;
            }
            return kContinue;
        }

    case kOscFull:
        SetupFull(track, static_cast<uint8_t>(a[0]), a[1], a[2]);
        return kContinue;

    case kCheckWave:
        {
            // One_CheckInstWave with the bank's number as register 6 holds it.
            const uint16_t bank_program = ReadRegDirect(index, kRegBank);
            const uint32_t result =
                channels_.CheckInstWave(bank_program >> 8, bank_program & 0xffu, (a[0] >> 8) & 0xffu, a[0] & 0xffu);
            WriteRegDirect(index, kRegValue, static_cast<uint8_t>(result));
            return kContinue;
        }

    case kPrintf:
        Printf(index);
        return kContinue;

    default:
        // The table's empty places: the arguments are read, and nothing happens.
        return kContinue;
    }
}

uint32_t Tracks::StepTimed(uint32_t index)
{
    // A tick of the timed parameters. Those of the oscillators, 6 to 10, set their settings at once. The others give
    // update flags. Parameter 11, the second oscillator's vertex, gives a flag too.
    Track& track = tracks_[index];
    uint32_t flags = 0;
    for (std::size_t i = 0; i < track.timed.size(); i++)
    {
        MoveParam& move = track.timed[i];
        if (move.time > 0.0f)
        {
            move.value += move.step;
            move.time -= 1.0f;
            if (i <= 5 || i >= 11)
            {
                flags |= 1u << i;
            }
            else
            {
                UpdateOscParam(track, static_cast<uint32_t>(i), move.value);
            }
        }
    }

    return flags;
}

int32_t Tracks::SeqMain(uint32_t index, bool muted)
{
    // Jam_SeqmainNote: a tick of the track, then of its children.
    Track& track = tracks_[index];
    if (track.parent != Track::kNone && track.tempo_sync)
    {
        float ratio = static_cast<float>(track.tempo) / static_cast<float>(tracks_[track.parent].tempo);
        if (ratio > 1.0f)
        {
            ratio = 1.0f;
        }
        track.tempo_counter += ratio;
        if (track.tempo_counter < 1.0f)
        {
            return 0;
        }
        track.tempo_counter -= 1.0f;
    }

    if (track.parent != Track::kNone)
    {
        track.muted = muted;

        // A child gives one of its free channels back to its parent at each tick.
        if (channels_.Manager(index).count != 0)
        {
            const uint32_t channel = channels_.ListGet(index, ChannelManager::kFree);
            if (channel != kNone)
            {
                channels_.ListAdd(track.parent, ChannelManager::kFree, channel);
                channels_.At(channel).manager = track.parent;
                channels_.Manager(index).count--;
                channels_.Manager(track.parent).count++;
            }
        }

        track.final_transpose = static_cast<uint8_t>(track.transpose + tracks_[track.parent].final_transpose);
    }
    else
    {
        track.final_transpose = static_cast<uint8_t>(track.transpose);
    }

    SetInterrupt(index, 7);
    if (track.timer != 0 && --track.timer == 0)
    {
        SetInterrupt(index, 6);
        if (track.timer_count != 0)
        {
            if (--track.timer_count != 0)
            {
                track.timer = track.timer_reload;
            }
        }
        else
        {
            track.timer = track.timer_reload;
        }
    }

    uint32_t flags = 0;
    bool timed = true;
    for (bool retry = true; retry;)
    {
        retry = false;
        TryInterrupt(index);
        if (track.paused && (track.pause_status & 2) != 0)
        {
            timed = false;
            break;
        }
        track.ticks++;

        if (track.wait == -1)
        {
            if (!CheckNoteStop(index, 0))
            {
                break;
            }
            track.wait = 0;
        }
        if (track.wait > 0)
        {
            if (--track.wait != 0)
            {
                break;
            }
            if (track.note_time != 0xffffffff && track.note_flags == 0)
            {
                for (uint32_t i = 0; i < track.note_played && i < 8; i++)
                {
                    track.notes[i] = kNoNote;
                    track.channels[i] = Track::kNone;
                }
            }
        }

        for (;;)
        {
            const uint8_t command = ReadByte(track);
            if ((command & 0x80) == 0)
            {
                // A note: its key, then a flag byte (bit 7: the key is a register's number, bits 5 and 6: a tie and a
                // sweep from the last note, bits 3 and 4: the bytes of its length, bits 0 to 2: its slot), the
                // velocity, and for slot 0 the gate and the length.
                auto key = static_cast<uint8_t>(command + track.final_transpose);
                const uint8_t note_flags = ReadByte(track);
                if ((note_flags & 0x80) != 0)
                {
                    key = static_cast<uint8_t>(ExchangeRegisterValue(index, key));
                    key = static_cast<uint8_t>(key + track.final_transpose);
                }
                uint8_t sweep_target = 0;
                if (((note_flags >> 5) & 2) != 0)
                {
                    sweep_target = key;
                    key = track.last_note;
                }
                auto velocity = ReadByte(track);
                if (velocity >= 0x80)
                {
                    velocity = static_cast<uint8_t>(ExchangeRegisterValue(index, velocity - 0x80));
                }

                uint8_t slot = note_flags & 7;
                uint8_t gate = 100;
                uint32_t length = 0xffffffff;
                if (slot == 0)
                {
                    gate = ReadByte(track);
                    if (gate >= 0x80)
                    {
                        gate = static_cast<uint8_t>(ExchangeRegisterValue(index, gate - 0x80));
                    }
                    length = 0;
                    const uint32_t bytes = (note_flags >> 3) & 3;
                    for (uint32_t i = 0; i < bytes; i++)
                    {
                        length = length << 8 | ReadByte(track);
                    }
                    if (bytes == 1 && length >= 0x80)
                    {
                        length = ExchangeRegisterValue(index, static_cast<uint8_t>(length - 0x80));
                    }
                }
                else if (((note_flags >> 3) & 3) != 0)
                {
                    slot = static_cast<uint8_t>(ExchangeRegisterValue(index, static_cast<uint8_t>(slot - 1)));
                    if (slot >= 8)
                    {
                        break;
                    }
                }

                track.note_flags = (note_flags >> 5) & 3;
                int32_t time = static_cast<int32_t>(length);
                int32_t result = 0;
                if (track.gate != 0)
                {
                    if ((track.note_flags & 1) != 0)
                    {
                        time = -1;
                    }
                    if (time != -1)
                    {
                        time = SeqTimeToDspTime(index, time, gate);
                    }
                    result = track.paused && (track.pause_status & 0x10) != 0
                                 ? -1
                                 : GateOn(index, slot, key, velocity, time);
                }
                else
                {
                    if (time != -1)
                    {
                        time = SeqTimeToDspTime(index, time, gate);
                    }
                    if ((track.note_flags & 1) != 0)
                    {
                        time = -1;
                    }
                    result = track.paused && (track.pause_status & 0x10) != 0
                                 ? -1
                                 : NoteOn(index, slot, key, velocity, time);
                }
                bool played = false;
                if (result != -1)
                {
                    track.notes[slot] = key;
                    played = true;
                    notes_[index]++;
                }

                track.note_played = played ? 1 : 0;
                track.gate_time = gate;
                track.last_velocity = velocity;
                track.note_time = length;
                track.gate = (track.note_flags & 1) != 0 ? 1 : 0;
                if ((track.note_flags & 2) != 0)
                {
                    int32_t steps = time;
                    if (steps == -1)
                    {
                        steps = SeqTimeToDspTime(index, static_cast<int32_t>(length), track.gate_time);
                    }
                    if (track.channels[0] != Track::kNone)
                    {
                        channels_.SetKeyTarget(track.channels[0],
                                               static_cast<uint8_t>(sweep_target + track.final_transpose),
                                               static_cast<uint32_t>(steps));
                    }
                    key = sweep_target;
                }
                track.last_note = key;

                if (length != 0xffffffff)
                {
                    track.wait = static_cast<int32_t>(length);
                    if (length == 0)
                    {
                        track.wait = -1;
                    }
                    break;
                }
                continue;
            }

            if ((command & 0xf0) == 0x80 || command == 0xf9)
            {
                // A note's end: 0x81 to 0x87 a slot, 0x89 to 0x8f a slot with a release time, 0x80 and 0x88 a wait of
                // a byte or a word. 0xf9 names the slot by a register.
                uint32_t wait_bytes = 1;
                uint16_t release = 0;
                uint8_t code = command;
                if (command == 0xf9)
                {
                    const uint8_t byte = ReadByte(track);
                    const auto slot = static_cast<uint8_t>(ExchangeRegisterValue(index, byte & 7));
                    if (slot > 7 || slot == 0)
                    {
                        if ((byte & 0x80) != 0)
                        {
                            ReadByte(track);
                        }
                        continue;
                    }
                    code = static_cast<uint8_t>(slot + 0x80 + ((byte & 0x80) != 0 ? 8 : 0));
                }
                uint8_t slot = code & 0x0f;
                if (slot == 8)
                {
                    wait_bytes = 2;
                    slot = 0;
                }
                if (slot > 8)
                {
                    slot = static_cast<uint8_t>(slot - 8);
                    release = ReadByte(track);
                    if (release > 100)
                    {
                        release = static_cast<uint16_t>((release - 98) * 20);
                    }
                }
                if (slot == 0)
                {
                    uint32_t wait = 0;
                    for (uint32_t i = 0; i < wait_bytes; i++)
                    {
                        wait = wait << 8 | ReadByte(track);
                    }
                    track.wait = static_cast<int32_t>(wait);
                    if (wait == 0)
                    {
                        continue;
                    }
                    break;
                }
                NoteOff(index, slot, release);
                continue;
            }

            int32_t result = kContinue;
            if ((command & 0xf0) == 0x90)
            {
                WriteTimeParam(index, command & 0x0f);
            }
            else if ((command & 0xf0) == 0xa0)
            {
                WriteRegParam(index, command & 0x0f);
            }
            else if ((command & 0xf0) == 0xb0)
            {
                result = RegCommandProcess(index, (command & 8) != 0, command & 7);
            }
            else
            {
                result = CommandProcess(index, command, 0);
            }

            if (result == kEndTick)
            {
                break;
            }
            if (result == kRetry)
            {
                retry = true;
                break;
            }
            if (result == kEndTrack)
            {
                return -1;
            }
        }
    }

    if (timed)
    {
        flags = StepTimed(index);
        for (std::size_t i = 0; i < 2; i++)
        {
            if (track.osc_routes[i] == 0x0e)
            {
                flags |= kOscFlags[std::min<std::size_t>(track.oscillators[i].target, kOscFlags.size() - 1)];
            }
        }
    }

    track.update_flags |= flags;
    for (std::size_t i = 0; i < track.children.size(); i++)
    {
        const uint32_t child = track.children[i];
        if (child == Track::kNone || tracks_[child].state == Track::kClosed)
        {
            continue;
        }

        const bool child_muted = track.muted || ((track.child_mutes >> i) & 1) != 0;
        if (SeqMain(child, child_muted) == -1)
        {
            CloseTrack(child);
            track.children[i] = Track::kNone;
        }
    }

    return 0;
}

void Tracks::SeqUpdate(uint32_t index, uint32_t flags)
{
    // SeqUpdate: the track's update flags, with those of its outer parameters, then each child's with the parent's.
    Track& track = tracks_[index];
    uint32_t all = flags | track.update_flags;
    if (track.outer != Track::kNone)
    {
        all |= outer_[track.outer].updates;
        outer_[track.outer].updates = 0;
    }
    track.update_flags = 0;
    if (all != 0)
    {
        UpdateTrack(index, all);
    }
    for (const uint32_t child : track.children)
    {
        if (child != Track::kNone && tracks_[child].state != Track::kClosed)
        {
            SeqUpdate(child, all);
        }
    }
}

std::vector<uint32_t> Tracks::FreeTracks() const
{
    std::vector<uint32_t> free;
    free.reserve(remain_);
    for (uint32_t i = 0; i < remain_; i++)
    {
        free.push_back(free_queue_[(get_ + i) % kTracks]);
    }

    return free;
}

} // namespace hsf::jac
