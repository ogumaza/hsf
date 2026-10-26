// SPDX-License-Identifier: MIT

#include "jaudio1/conductor.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

namespace hsf::jaudio1
{

namespace
{

// The bytes of each object in a conductor file: before its values, its values, and after them.
constexpr std::size_t kConductorValues = 3;
constexpr std::size_t kConductorAfter = 17;
constexpr std::size_t kTrackBefore = 3;
constexpr std::size_t kTrackValues = 14;
constexpr std::size_t kTrackAfter = 3;
constexpr std::size_t kModuleValues = 19;
constexpr std::size_t kModuleAfter = 21;
constexpr std::size_t kNameBytes = 8;

// The requests to the conductor's code.
constexpr uint32_t kSyncConductor = 0x300;
constexpr uint32_t kSyncTrack = 0x600;
constexpr uint32_t kSyncModule = 0x800;
constexpr uint32_t kSyncCycleLoop = 0x900;
constexpr uint32_t kSyncCycleTop = 0xa00;
constexpr uint32_t kSyncClearScene = 0xb00;
constexpr uint32_t kSyncPlay = 0xc00;
constexpr uint32_t kSyncCloser = 0xd00;

// The registers that hold a module's handle (PSSystem::setObject with 20), and the ports that the requests use.
constexpr uint32_t kModuleRegister = 20;
constexpr uint32_t kPortTipTable = 2;
constexpr uint32_t kPortSceneTable = 4;
constexpr uint32_t kPortModule = 3;
constexpr uint32_t kPortTip = 6;
constexpr uint32_t kPortScene = 8;

// The track's timed levels that a module sets (JASTrack::setParam).
constexpr uint32_t kParamVolume = 0;
constexpr uint32_t kParamPan = 3;

// The answers' waits in ticks: a unit, and the half unit that a start can add.
constexpr uint32_t kStartUnit = 120;
constexpr uint32_t kHalfStart = 60;

// The end of a list of tips (3 bytes each) and of wave scenes (2 bytes each), and the longest list that the game's
// count can reach.
constexpr uint8_t kEnd = 0xff;
constexpr uint32_t kMaxList = 255;

constexpr uint16_t kNone = 0xffff;

uint32_t Read24(const Sequence& sequence, uint32_t offset)
{
    return uint32_t{sequence.U8(offset)} << 16 | uint32_t{sequence.U8(offset + 1)} << 8 | sequence.U8(offset + 2);
}

uint16_t Read16(const Sequence& sequence, uint32_t offset)
{
    return static_cast<uint16_t>(sequence.U8(offset) << 8 | sequence.U8(offset + 1));
}

uint16_t StartWait(uint8_t half_start, uint16_t start)
{
    return static_cast<uint16_t>((half_start == 1 ? kHalfStart : 0) + start * kStartUnit);
}

} // namespace

ConductorFile ConductorFile::Parse(std::span<const uint8_t> bytes)
{
    const xsflib::Reader r(bytes, false);
    ConductorFile file;
    std::size_t at = 0;
    file.beat_interval = r.U8(at);
    file.tempo = r.U8(at + 1);
    const uint8_t track_count = r.U8(at + 2);
    at += kConductorValues + kConductorAfter;

    for (uint32_t i = 0; i < track_count; i++)
    {
        Track track;
        at += kTrackBefore;
        const uint8_t module_count = r.U8(at);
        track.start = r.U16(at + 1);
        track.half_start = r.U8(at + 3);
        for (std::size_t c = 0; c < kNameBytes && r.U8(at + 4 + c) != 0; c++)
        {
            track.name += static_cast<char>(r.U8(at + 4 + c));
        }
        track.unison = r.U8(at + 12);
        track.fxmix = r.U8(at + 13);
        at += kTrackValues + kTrackAfter;

        for (uint32_t j = 0; j < module_count; j++)
        {
            // The byte at +4 is a value that no request reads.
            Module module;
            module.start = r.U16(at);
            module.beats = r.S16(at + 2);
            module.in_order = r.U8(at + 5);
            module.port_high = r.U8(at + 6);
            module.port_low = r.U8(at + 7);
            module.tip_list = r.U16(at + 8);
            module.scene_list = r.U16(at + 10);
            module.repeats = {r.U8(at + 12), r.U8(at + 13)};
            module.transpose = r.S16(at + 14);
            module.volume = r.U8(at + 16);
            module.pan = r.U8(at + 17);
            module.half_start = r.U8(at + 18);
            at += kModuleValues + kModuleAfter;
            track.modules.push_back(module);
        }
        file.tracks.push_back(std::move(track));
    }

    return file;
}

Conductor::Conductor(ConductorFile file) : file_(std::move(file))
{
    for (const ConductorFile::Track& track : file_.tracks)
    {
        TrackState state;
        state.modules.resize(track.modules.size());
        tracks_.push_back(std::move(state));
    }
}

std::optional<float> Conductor::Fxmix(uint32_t number) const
{
    if (number >= file_.tracks.size())
    {
        return std::nullopt;
    }

    return std::clamp(static_cast<float>(file_.tracks[number].fxmix) / 127.0f, 0.0f, 1.0f);
}

void Conductor::CountBeat()
{
    for (TrackState& track : tracks_)
    {
        if (track.current < track.modules.size())
        {
            track.modules[track.current].beats++;
        }
    }
}

std::optional<uint16_t> Conductor::Sync(Tracks& tracks, uint32_t index, uint32_t value)
{
    switch (value)
    {
    case kSyncConductor:
        return StartConductor(tracks, index);

    case kSyncTrack:
    case kSyncModule:
        {
            // Conductor::seqCpuSync_AutoBgm passes the request to the conductor's track for the slot in the requesting
            // track's route. The game's code fails on a slot that the conductor has no track for.
            const uint32_t number = tracks.TrackAt(index).route & 0xf;
            if (number >= tracks_.size())
            {
                unmodelled_++;
                return uint16_t{0};
            }
            return value == kSyncTrack ? StartTrack(number) : StartModule(tracks, index, number);
        }

    case kSyncCycleLoop:
    case kSyncCycleTop:
    case kSyncClearScene:
    case kSyncPlay:
    case kSyncCloser:
        {
            const std::optional<ModuleRef> ref = FindModule(tracks, index);
            if (!ref)
            {
                unmodelled_++;
                return uint16_t{0};
            }

            ModuleState& module = tracks_[ref->track].modules[ref->module];
            switch (value)
            {
            case kSyncCycleLoop:
                return CycleLoop(*ref);

            case kSyncCycleTop:
                // CycleBase::cycleTop: the cycle's wave scenes start again from the first.
                module.scene[module.cycle] = 0;
                return uint16_t{0};

            case kSyncClearScene:
                module.scene_value = kNone;
                return uint16_t{0};

            case kSyncPlay:
                return Play(tracks, index, *ref);

            default:
                return CheckCloser(*ref);
            }
        }

    default:
        return std::nullopt;
    }
}

std::optional<Conductor::ModuleRef> Conductor::FindModule(const Tracks& tracks, uint32_t index) const
{
    const Track& track = tracks.TrackAt(index);
    const uint32_t handle =
        uint32_t{track.registers[kModuleRegister]} << 16 | uint32_t{track.registers[kModuleRegister + 1]};
    if (handle == 0)
    {
        return std::nullopt;
    }

    const ModuleRef ref{(handle - 1) >> 8, (handle - 1) & 0xff};
    if (ref.track >= tracks_.size() || ref.module >= tracks_[ref.track].modules.size())
    {
        return std::nullopt;
    }

    return ref;
}

uint16_t Conductor::StartConductor(Tracks& tracks, uint32_t index)
{
    // The conductor's start: the track takes the conductor's tempo, and ports 2 to 5 give the offsets of the tables
    // of the lists of tips and of wave scenes in the sequence's data, high halves first (createTables). The answer is
    // the number of the conductor's tracks.
    tracks.SetTempo(index, file_.tempo);
    tip_table_ = uint32_t{tracks.ReadPortAppDirect(index, kPortTipTable)} << 16 |
                 tracks.ReadPortAppDirect(index, kPortTipTable + 1);
    scene_table_ = uint32_t{tracks.ReadPortAppDirect(index, kPortSceneTable)} << 16 |
                   tracks.ReadPortAppDirect(index, kPortSceneTable + 1);

    return static_cast<uint16_t>(tracks_.size());
}

uint16_t Conductor::StartTrack(uint32_t number)
{
    // Track::seqCpuSync_AutoBgm_Track at the track's start: no module yet, the unison track, and a fresh look at each
    // module's lists. The answer is the track's wait. A unison track gives the half unit.
    TrackState& track = tracks_[number];
    const ConductorFile::Track& file = file_.tracks[number];
    track.current = kNoModule;
    track.unison = file.unison != kNoUnison && file.unison < tracks_.size() ? file.unison : kNoUnison;
    for (ModuleState& module : track.modules)
    {
        module.tables = false;
    }

    const uint8_t half = track.unison != kNoUnison ? file_.tracks[track.unison].half_start : file.half_start;
    return StartWait(half, file.start);
}

uint16_t Conductor::StartModule(Tracks& tracks, uint32_t index, uint32_t number)
{
    // Track::incCurModule: the track's next module, or its last again.
    TrackState& track = tracks_[number];
    const auto count = static_cast<int32_t>(track.modules.size());
    if (track.current == kNoModule)
    {
        track.current = 0;
    }
    else if (static_cast<int32_t>(track.current) != count - 1)
    {
        track.current = static_cast<uint8_t>(track.current + 1);
    }
    if (track.current >= track.modules.size())
    {
        return 0;
    }

    // Module::seqCpuSync_AutoBgm_Module: the track takes the module's handle, the value for port 3, the volume and the
    // pan at once, and the transpose. The cycles start again. The answer is the module's wait. A unison track's first
    // module gives the half unit.
    const ModuleRef ref{number, track.current};
    const ConductorFile::Module& file = file_.tracks[number].modules[track.current];
    ModuleState& module = track.modules[track.current];
    const uint32_t handle = 1 + (number << 8) + track.current;
    tracks.WriteRegDirect(index, kModuleRegister, handle >> 16);
    tracks.WriteRegDirect(index, kModuleRegister + 1, handle & 0xffff);
    tracks.WritePortAppDirect(index, kPortModule, static_cast<uint16_t>(file.port_low | file.port_high << 8));
    tracks.SetParam(index, kParamVolume, static_cast<float>(std::min<uint8_t>(file.volume, 127)) / 127.0f, -1);
    tracks.SetParam(index, kParamPan, static_cast<float>(std::min<uint8_t>(file.pan, 127)) / 127.0f, -1);
    tracks.TrackAt(index).transpose = static_cast<int8_t>(file.transpose & 0xff);
    module.beats = 0;
    module.cycle = 0;
    module.tip = kNone;
    module.repeat = {0, 0};
    if (!module.tables)
    {
        SetTables(tracks.TrackAt(index).sequence, ref);
        module.tables = true;
    }

    uint8_t half = file.half_start;
    if (track.unison != kNoUnison)
    {
        const std::vector<ConductorFile::Module>& unison = file_.tracks[track.unison].modules;
        half = unison.empty() ? 0 : unison[0].half_start;
    }

    return StartWait(half, file.start);
}

void Conductor::SetTables(const Sequence* sequence, const ModuleRef& ref)
{
    // Module::setTableAddress: the tables give each list's offset in 3 bytes, high byte first. A list ends with an
    // entry of 0xff bytes.
    ModuleState& module = tracks_[ref.track].modules[ref.module];
    const ConductorFile::Module& file = file_.tracks[ref.track].modules[ref.module];
    if (sequence == nullptr)
    {
        return;
    }

    module.tips = Read24(*sequence, tip_table_ + 3 * uint32_t{file.tip_list});
    module.scenes = Read24(*sequence, scene_table_ + 3 * uint32_t{file.scene_list});
    uint32_t count = 0;
    while (count < kMaxList &&
           !(sequence->U8(module.tips + 3 * count) == kEnd && sequence->U8(module.tips + 3 * count + 1) == kEnd &&
             sequence->U8(module.tips + 3 * count + 2) == kEnd))
    {
        count++;
    }
    module.tip_count = static_cast<uint8_t>(count);

    count = 0;
    while (count < kMaxList &&
           !(sequence->U8(module.scenes + 2 * count) == kEnd && sequence->U8(module.scenes + 2 * count + 1) == kEnd))
    {
        count++;
    }
    module.scene_count = static_cast<uint8_t>(count);
}

uint16_t Conductor::CycleLoop(const ModuleRef& ref)
{
    // Module::cycleLoop. 1: the track moves on to its next module, once the module has counted its beats. 2: the
    // module moves on to its other cycle. 3: the on cycle's first repeat. 0: another repeat.
    const TrackState& track = tracks_[ref.track];
    ModuleState& module = tracks_[ref.track].modules[ref.module];
    const ConductorFile::Module& file = file_.tracks[ref.track].modules[ref.module];
    if (static_cast<int32_t>(track.current) < static_cast<int32_t>(track.modules.size()) - 1 &&
        static_cast<int32_t>(module.beats) >= file.beats)
    {
        return 1;
    }

    const auto next_cycle = [&module]
    {
        module.cycle = static_cast<uint8_t>(module.cycle + 1 >= 2 ? 0 : module.cycle + 1);
    };
    const uint8_t repeats = file.repeats[module.cycle];
    if (repeats == 0)
    {
        next_cycle();
        return 2;
    }

    uint8_t& repeat = module.repeat[module.cycle];
    const auto next = static_cast<uint8_t>(repeat + 1);
    repeat = static_cast<uint8_t>(next > repeats ? 0 : next);
    if (repeat == 0)
    {
        next_cycle();
        return 2;
    }
    if (repeat == 1 && module.cycle == 0)
    {
        return 3;
    }

    return 0;
}

const Conductor::ModuleState* Conductor::UnisonModule(uint32_t track)
{
    const uint8_t unison = tracks_[track].unison;
    if (unison == kNoUnison)
    {
        return nullptr;
    }

    const TrackState& state = tracks_[unison];
    if (state.current >= state.modules.size())
    {
        unmodelled_++;
        static const ModuleState kEmpty;
        return &kEmpty;
    }

    return &state.modules[state.current];
}

uint16_t Conductor::PlayScene(Tracks& tracks, uint32_t index, const ModuleRef& ref)
{
    // CycleBase::play: the wave scene for port 8. A module in order takes the cycle's next scene, and answers 4 once
    // the cycle has taken them all. Otherwise the scene is drawn at random. A unison track copies its unison track's
    // scene. The melody arranger can mark a scene to avoid (bit 15). It stays off: the director that turns it on, by
    // the number of Pikmin, does nothing while the game leaves the piece alone.
    ModuleState& module = tracks_[ref.track].modules[ref.module];
    const ConductorFile::Module& file = file_.tracks[ref.track].modules[ref.module];
    const Sequence* sequence = tracks.TrackAt(index).sequence;
    const auto scene = [&](uint32_t number) -> uint16_t
    {
        return sequence != nullptr ? Read16(*sequence, module.scenes + 2 * number) : kNone;
    };

    uint16_t value = 0;
    uint16_t answer = 1;
    if (file.in_order == 1)
    {
        uint8_t& next = module.scene[module.cycle];
        if (next == module.scene_count)
        {
            return 4;
        }

        const uint8_t number = next++;
        const ModuleState* unison = UnisonModule(ref.track);
        value = unison != nullptr ? unison->scene_value : scene(number);
        answer = 3;
    }
    else if (const ModuleState* unison = UnisonModule(ref.track))
    {
        value = unison->scene_value;
    }
    else
    {
        const auto number =
            static_cast<uint16_t>(static_cast<uint32_t>(random_.Next() * static_cast<float>(module.scene_count)));
        value = scene(number);
    }

    module.scene_value = value;
    tracks.WritePortAppDirect(index, kPortScene, value);

    return answer;
}

void Conductor::SetTip(Tracks& tracks, uint32_t index, const ModuleRef& ref)
{
    // OnCycle::setTip: the tip for ports 6 and 7, high byte first. A list of one tip gives it. Otherwise the tip is
    // drawn at random, and drawn again from the others if the last two draws gave it both times
    // (historiesAreSameAll). A unison track copies its unison track's tip.
    ModuleState& module = tracks_[ref.track].modules[ref.module];
    uint16_t tip = 0;
    if (const ModuleState* unison = UnisonModule(ref.track))
    {
        tip = unison->tip;
    }
    else if (module.tip_count > 1)
    {
        const auto count = static_cast<float>(module.tip_count);
        uint32_t drawn = static_cast<uint32_t>(random_.Next() * count);
        uint16_t same = kNone;
        if (module.history.size() >= kHistory &&
            std::all_of(module.history.begin(), module.history.end(),
                        [&module](uint16_t t) { return t == module.history.front(); }))
        {
            same = module.history.front();
        }
        if (same != kNone && same == static_cast<uint16_t>(drawn))
        {
            const auto other = static_cast<uint32_t>(random_.Next() * static_cast<float>(module.tip_count - 1));
            drawn = same <= static_cast<uint16_t>(other) ? other + 1 : other;
        }
        tip = static_cast<uint16_t>(drawn);
        module.history.push_back(tip);
        if (module.history.size() > kHistory)
        {
            module.history.erase(module.history.begin());
        }
    }

    module.tip = tip;
    const Sequence* sequence = tracks.TrackAt(index).sequence;
    const uint32_t value = sequence != nullptr ? Read24(*sequence, module.tips + 3 * uint32_t{tip}) : 0xffffff;
    tracks.WritePortAppDirect(index, kPortTip, static_cast<uint16_t>(value >> 16));
    tracks.WritePortAppDirect(index, kPortTip + 1, static_cast<uint16_t>(value & 0xffff));
}

uint16_t Conductor::Play(Tracks& tracks, uint32_t index, const ModuleRef& ref)
{
    // The cycle's play. The off cycle plays a wave scene (CycleBase::play). The on cycle also sets a tip
    // (OnCycle::play): after the scene for a module in order, before it otherwise.
    ModuleState& module = tracks_[ref.track].modules[ref.module];
    const ConductorFile::Module& file = file_.tracks[ref.track].modules[ref.module];
    if (module.cycle != 0)
    {
        return PlayScene(tracks, index, ref);
    }

    if (file.in_order == 1)
    {
        if (PlayScene(tracks, index, ref) == 4)
        {
            return 4;
        }

        SetTip(tracks, index, ref);
        return 2;
    }
    SetTip(tracks, index, ref);
    PlayScene(tracks, index, ref);

    return 0;
}

uint16_t Conductor::CheckCloser(const ModuleRef& ref) const
{
    // CycleBase::checkCloser: 1 at the on cycle's last repeat, once a module in order has taken all its scenes.
    const ModuleState& module = tracks_[ref.track].modules[ref.module];
    const ConductorFile::Module& file = file_.tracks[ref.track].modules[ref.module];
    if (file.in_order == 1 && module.scene[module.cycle] != module.scene_count)
    {
        return 0;
    }

    return module.repeat[module.cycle] == file.repeats[module.cycle] && module.cycle == 0 ? 1 : 0;
}

} // namespace hsf::jaudio1
