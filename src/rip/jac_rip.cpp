// SPDX-License-Identifier: MIT

// hsfrip's parts for Pikmin (see jac_rip.h).

#include "rip/jac_rip.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/jac/channel.h"
#include "ntdlib/jac/engine.h"
#include "xsflib/ascii.h"
#include "xsflib/rip/length.h"
#include "xsflib/text.h"

#include "hsf/jac.h"
#include "hsf/jaudio_models.h"
#include "rip/game_paths.h"
#include "rip/jaudio_length.h"

namespace hsf::rip
{
namespace
{

// The wave scenes that the game loads as it starts, in its order. Jac_Start (0x266f2c) sets scene 0 of wave system 0.
// As the first title screen starts, Jac_SceneSetup (0x265e30 to 0x265e84) loads scenes 1 and 2 of system 0, scene 0 of
// system 22 and scene 3 of system 0, then sets scene 0 of systems 14, 15 and 2. The title's piece sets its scene before
// them. The game closes that scene before it sets another piece's.
constexpr std::array<JacLoad, 8> kLoads = {{
    {{0, 0}, 1},
    {{0, 1}, 0},
    {{0, 2}, 0},
    {{22, 0}, 0},
    {{0, 3}, 0},
    {{14, 0}, 1},
    {{15, 0}, 1},
    {{2, 0}, 1},
}};

// The diary's first scene. The game sets it as the first title screen starts, and the diary's piece of the cutscenes'
// music (piece 2) plays its waves.
constexpr JacScene kDiaryScene = {14, 0};
constexpr uint16_t kDiaryPart = 2;

// The pieces of the cutscenes' music whose scenes __Prepare_BGM sets (0x261aac): bit n - 1 for piece n.
constexpr uint32_t kPartsWithScenes = 0x3f1;

// The game's numbers of the effects that the game's code handles apart from the others. The model defines them
// (jac/engine.h). The piece of the cutscenes' music kShipPart plays as a part of the ship appears, through the system's
// effect kShipPartEffect. kResumeAll and kPlayerDownPause play no sound.
using jac::kContainerFilled;
using jac::kContainerSound;
using jac::kHighScore;
using jac::kPartSystem;
using jac::kPauseAll;
using jac::kPikminEffect;
using jac::kPlayerDownPause;
using jac::kPluck;
using jac::kResumeAll;
using jac::kShipEvent;
using jac::kShipPart;
using jac::kShipPartEffect;

// The system's effect that kPauseAll plays after it pauses the game's sounds.
constexpr uint16_t kPauseSound = 0x1c;

// The system's effects that the game's code plays with numbers of its own: a cutscene's start (0x261700) and two of
// the cutscenes' code (0x262128 and 0x26219c).
constexpr std::array<uint16_t, 3> kCodeSystemEffects = {0x19, 0x1e, 0x1f};

// The captain's effects that the game's code plays with numbers of its own: a cutscene's start (0x2616a8) and a filled
// container (kContainerSound).
constexpr std::array<uint16_t, 2> kCodeCaptainEffects = {0x0, kContainerSound};

// A captain's effect that the game's table gives a Pikmin's sound for: a Pikmin's sound in the table is one of the
// captain's with bit 15 set (kPikminEffect). The pluck (kPluck) also writes a random cry to port 1. The game draws a
// number from 0 to 3 from the generator of random numbers (GetRandom_ulimit, called at 0x2652d8). The number picks cry
// 0xd, 0xe, 0xf or 0xb. After three equal draws in a row, the game picks one of the cries 0x10 to 0x14 instead. The
// rip takes the cry of the generator's first draw from its state at start-up: 3, and cry 0xb (kPluckCry).
constexpr uint16_t kPluckCry = 0xb;

// The Pikmin's cries that the game's code writes with numbers of its own: the cries of a pluck (0xb to 0x14), and those
// as the game starts (0xc and 0x15 to 0x17, 0x265eac).
constexpr uint16_t kFirstCodeCry = 0xb;
constexpr uint16_t kLastCodeCry = 0x17;

// The formation's sound at the stick's full distance from the centre, the crowd's sizes (0x265560 writes 1 to 29), and
// the captain's footstep (0x80688).
constexpr uint16_t kFullDistance = 127;
constexpr uint16_t kCrowdSizes = 29;
constexpr uint16_t kFootstep = 1;

// An event of type kShipEvent plays its action 4 as it's created (0x2626d0).
constexpr uint16_t kStartAction = 4;

// The event types of the game's table of effects: the system's effects and the captain's.
constexpr int32_t kSystemType = -2;
constexpr int32_t kCaptainType = -1;

// An event's slots, and what Jac_PlayEventAction keeps for each: the row of the action that holds it, and the action's
// group. Row 0 marks a free slot.
constexpr std::size_t kSlots = 16;

struct Slot
{
    std::size_t row = 0;
    uint8_t group = 0;
};

// The bits of an action's flags (Jac_PlayEventAction, 0x2628e0): a limit of the actions of its group that play at
// once, the limit in the low 4 bits (0 for 16), and the action's refusal of a slot that an action of no lower priority
// holds.
constexpr uint8_t kLimited = 0x10;
constexpr uint8_t kRefuses = 0x20;
constexpr uint8_t kLimit = 0xf;

// The priority of the action in row `row` of the actions' table.
uint8_t PriorityOf(const EventActions& actions, std::size_t row)
{
    return actions.actions[row].priority;
}

// The slot that Jac_PlayEventAction picks for an action whose group has a limit (kLimited): the first free slot, but
// the slot with the lowest priority below the action's once the group reaches its limit or no slot is free. Slot 0 if
// no slot has a lower priority. Nothing if the action refuses that slot.
std::optional<std::size_t> LimitedSlot(const EventActions& actions, const std::array<Slot, kSlots>& slots,
                                       const EventAction& status)
{
    std::size_t target = 0;
    while (target < kSlots && slots[target].row != 0)
    {
        ++target;
    }

    const std::size_t limit = (status.flags & kLimit) == 0 ? kSlots : status.flags & kLimit;
    std::size_t playing = 0;
    std::size_t lowest = 0;
    uint8_t lowest_priority = status.priority;
    for (std::size_t i = 0; i < kSlots; ++i)
    {
        if (slots[i].row == 0)
        {
            continue;
        }

        playing += slots[i].group == status.group ? 1 : 0;
        if (lowest_priority > PriorityOf(actions, slots[i].row))
        {
            lowest = i;
            lowest_priority = PriorityOf(actions, slots[i].row);
        }
    }

    if (playing >= limit || target == kSlots)
    {
        if (status.flags & kRefuses)
        {
            return std::nullopt;
        }

        target = lowest;
    }

    return target;
}

// The slot that Jac_PlayEventAction picks for an action whose group has no limit: the first slot that an action of the
// group holds, or else the last free slot. kSlots if there's neither. Nothing if the action refuses the slot of its
// group.
std::optional<std::size_t> GroupSlot(const EventActions& actions, const std::array<Slot, kSlots>& slots,
                                     const EventAction& status)
{
    std::size_t last_free = kSlots;
    for (std::size_t i = 0; i < kSlots; ++i)
    {
        if (slots[i].row == 0)
        {
            last_free = i;
            continue;
        }

        if (slots[i].group != status.group)
        {
            continue;
        }

        if ((status.flags & kRefuses) && status.priority <= PriorityOf(actions, slots[i].row))
        {
            return std::nullopt;
        }

        return i;
    }

    return last_free;
}

// The first slot whose action has the lowest priority, if that priority is no higher than the action's. A free slot
// counts with row 0's priority. The game does the same. kSlots if no slot qualifies.
std::size_t LowestSlot(const EventActions& actions, const std::array<Slot, kSlots>& slots, const EventAction& status)
{
    std::size_t target = kSlots;
    uint32_t lowest_priority = status.priority + 1u;
    for (std::size_t i = 0; i < kSlots; ++i)
    {
        if (PriorityOf(actions, slots[i].row) < lowest_priority)
        {
            lowest_priority = PriorityOf(actions, slots[i].row);
            target = i;
        }
    }

    return target;
}

// The value that Jac_PlayEventAction sends to an event's track for `action` of an event of `type` whose slots are
// `slots`, or nothing if it refuses the action. The value holds the slot in bits 12 and up and the action's command
// below them. It marks the slot as the action's. Every slot's action starts in the same frame.
std::optional<uint16_t> PlayAction(const EventActions& actions, std::array<Slot, kSlots>& slots, uint8_t type,
                                   uint16_t action)
{
    const auto row = static_cast<std::size_t>(actions.first_actions[type] + action);
    if (row >= actions.actions.size())
    {
        return std::nullopt;
    }

    const EventAction& status = actions.actions[row];
    const std::optional<std::size_t> slot =
        status.flags & kLimited ? LimitedSlot(actions, slots, status) : GroupSlot(actions, slots, status);
    if (!slot)
    {
        return std::nullopt;
    }

    const std::size_t target = *slot != kSlots ? *slot : LowestSlot(actions, slots, status);
    if (target == kSlots)
    {
        return std::nullopt;
    }

    slots[target] = {row, status.group};

    return static_cast<uint16_t>(target << 12 | (status.command & 0xfff));
}

// The effect of an object's event: the event's type, the value of `action` on a new event, and the value of the action
// that the event plays as it's created. Nothing if the game refuses the action.
std::optional<JacEffect> EventEffect(const EventActions& actions, uint8_t type, uint16_t action)
{
    JacEffect effect;
    effect.kind = JacEffect::kEvent;
    effect.type = type;
    std::array<Slot, kSlots> slots{};
    if (type == kShipEvent)
    {
        const std::optional<uint16_t> first = PlayAction(actions, slots, type, kStartAction);
        if (first && action == kStartAction)
        {
            effect.value = *first;
            return effect;
        }

        if (first)
        {
            effect.first = *first;
        }
    }

    const std::optional<uint16_t> value = PlayAction(actions, slots, type, action);
    if (!value)
    {
        return std::nullopt;
    }

    effect.value = *value;

    return effect;
}

JacEffect MakeEffect(uint8_t kind, uint16_t value, uint16_t first = JacEffect::kNone)
{
    JacEffect effect;
    effect.kind = kind;
    effect.value = value;
    effect.first = first;

    return effect;
}

std::string Decimal(unsigned value, int digits)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%0*u", digits, value);
    return text;
}

// Whether the bank file's systems hold `scene`: a system at the scene's place with a group at the scene's.
bool HoldsScene(const std::vector<ntdlib::jaudio::WaveSystem>& systems, JacScene scene)
{
    return std::any_of(systems.begin(), systems.end(), [scene](const ntdlib::jaudio::WaveSystem& system)
                       { return system.number == scene.system && scene.scene < system.groups.size(); });
}

// Adds the job of `id` to `selection`, at the next index.
void AddJob(JacSelection& selection, const std::string& label, uint32_t id)
{
    JaudioJob job;
    job.index = static_cast<uint32_t>(selection.jobs.size());
    job.label = label;
    job.id = id;
    selection.jobs.push_back(std::move(job));
}

// Adds the pieces of music to `selection` (see SelectJacSounds). Returns the name of the cutscenes' music file without
// its extension, or "demobgm" if the archive has no such file.
std::string SelectPieces(const JacGameCode& code, const std::vector<ntdlib::jaudio::ArchiveFile>& sequences,
                         const std::vector<ntdlib::jaudio::WaveSystem>& systems, const std::regex& only,
                         JacSelection& selection)
{
    JaudioLibrary3& library = selection.library;
    const uint16_t cutscene_music = library.startup_files[2];
    std::string cutscene_name = "demobgm";
    for (const ntdlib::jaudio::ArchiveFile& file : sequences)
    {
        if (file.id == cutscene_music)
        {
            cutscene_name = WithoutExtension(file.name);
        }

        if (file.id < kFirstPieceFile || file.id == cutscene_music || file.data.empty())
        {
            continue;
        }

        const std::string label = WithoutExtension(file.name);
        if (file.id - kFirstPieceFile >= static_cast<int>(kPieces))
        {
            selection.warnings.push_back(label + " has no settings in the game's code; it's left out");
            continue;
        }

        const JacScene scene{file.id, 0};
        if (!HoldsScene(systems, scene))
        {
            selection.warnings.push_back(label + "'s wave system " + std::to_string(file.id) +
                                         " isn't in the bank file; the piece is left out");
            continue;
        }

        if (!std::regex_search(label, only))
        {
            continue;
        }

        JacPiece piece;
        piece.file = file.id;
        piece.playing_tracks = code.music.playing_tracks[file.id - kFirstPieceFile];
        piece.volume = code.music.track_volumes[file.id - kFirstPieceFile];
        piece.scene = scene;
        AddJob(selection, label, JaudioLibrary3::kPieceSection | static_cast<uint32_t>(library.pieces.size()));
        library.pieces.push_back(piece);
    }

    return cutscene_name;
}

// Adds the pieces of the cutscenes' music to `selection`. Each label starts with `cutscene_name`. The cutscenes' table
// gives each cutscene's piece in the low 4 bits of its music. Bit 7 picks a stream instead, and bit 6 no music.
void SelectParts(const JacGameCode& code, const std::string& cutscene_name,
                 const std::vector<ntdlib::jaudio::WaveSystem>& systems, const std::regex& only,
                 JacSelection& selection)
{
    JaudioLibrary3& library = selection.library;

    std::set<uint16_t> parts = {kShipPart};
    for (const Cutscene& cutscene : code.cutscenes)
    {
        if ((cutscene.music & 0xc0) == 0 && (cutscene.music & 0xf) != 0)
        {
            parts.insert(cutscene.music & 0xf);
        }
    }

    for (const uint16_t number : parts)
    {
        const std::string label = cutscene_name + "-" + std::to_string(number);
        JacPart part;
        part.number = number;
        if (number == kDiaryPart)
        {
            part.scene = kDiaryScene;
        }
        else if (number >= 1 && number <= kParts && (kPartsWithScenes >> (number - 1) & 1))
        {
            part.scene = {kPartSystem, static_cast<uint16_t>(code.music.part_scenes[number - 1])};
        }
        else
        {
            selection.warnings.push_back(label + " has no scene in the game's code; it's left out");
            continue;
        }

        if (!HoldsScene(systems, part.scene))
        {
            selection.warnings.push_back(label + "'s scene isn't in the bank file; the piece is left out");
            continue;
        }

        if (!std::regex_search(label, only))
        {
            continue;
        }

        AddJob(selection, label, JaudioLibrary3::kPartSection | static_cast<uint32_t>(library.parts.size()));
        library.parts.push_back(part);
    }
}

// Adds effects whose labels match a pattern to a selection. An effect that another one plays the same way is left out.
class EffectChoice
{
public:
    EffectChoice(JacSelection& selection, const std::regex& only) : selection_(selection), only_(only)
    {
    }

    // Adds `effect` and its later events, unless the label doesn't match or an earlier effect plays the same way.
    void Add(const std::string& label, const JacEffect& effect, std::vector<JacLaterEvent> later_events = {})
    {
        if (!taken_.insert({effect.kind, effect.type, effect.value, effect.first}).second ||
            !std::regex_search(label, only_))
        {
            return;
        }

        JaudioLibrary3& library = selection_.library;
        AddJob(selection_, label, JaudioLibrary3::kEffectSection | static_cast<uint32_t>(library.effects.size()));
        library.effects.push_back(effect);
        selection_.later_events.push_back(std::move(later_events));
    }

private:
    JacSelection& selection_;
    const std::regex& only_;
    std::set<std::tuple<uint8_t, uint8_t, uint16_t, uint16_t>> taken_; // the kind, type, value and first of each effect
};

// The system's effect `value` as the game plays it, or nothing for one that plays no effect.
std::optional<JacEffect> SystemEffect(uint16_t value)
{
    switch (value)
    {
    case kContainerFilled:
        return MakeEffect(JacEffect::kCaptain, kContainerSound);
    case kShipPartEffect:
    case kHighScore:
    case kResumeAll:
    case kPlayerDownPause:
        return std::nullopt;
    case kPauseAll:
        return MakeEffect(JacEffect::kSystem, kPauseSound);
    default:
        return MakeEffect(JacEffect::kSystem, value);
    }
}

// Whether `row` of the effects' table has a type that TableEffect reads.
bool KnownType(const Effect& row)
{
    return row.event_type == kSystemType || row.event_type == kCaptainType ||
           (row.event_type >= 1 && row.event_type <= JacEffect::kEventTypes);
}

// The effect of `row` of the effects' table, of a known type, as the game plays it. Nothing for a row that plays no
// effect.
std::optional<JacEffect> TableEffect(const Effect& row, const EventActions& actions)
{
    const auto value = static_cast<uint16_t>(row.jaudio);
    if (row.event_type == kSystemType)
    {
        return SystemEffect(value);
    }

    if (row.event_type == kCaptainType)
    {
        if (row.jaudio & kPikminEffect)
        {
            return MakeEffect(JacEffect::kPikmin, value & 0x7fff);
        }

        return MakeEffect(JacEffect::kCaptain, value, value == kPluck ? kPluckCry : JacEffect::kNone);
    }

    return EventEffect(actions, static_cast<uint8_t>(row.event_type), value);
}

// Adds the effects that the game's code plays with numbers of its own, then the formation, the crowds and a footstep.
void AddCodeEffects(EffectChoice& effects)
{
    for (const uint16_t value : kCodeSystemEffects)
    {
        effects.Add("system-" + xsflib::Hex(value, 2), MakeEffect(JacEffect::kSystem, value));
    }

    for (const uint16_t value : kCodeCaptainEffects)
    {
        effects.Add("captain-" + xsflib::Hex(value, 2), MakeEffect(JacEffect::kCaptain, value));
    }

    for (uint16_t value = kFirstCodeCry; value <= kLastCodeCry; ++value)
    {
        effects.Add("pikmin-" + xsflib::Hex(value, 2), MakeEffect(JacEffect::kPikmin, value));
    }

    effects.Add("formation", MakeEffect(JacEffect::kFormation, kFullDistance));
    for (uint16_t size = 1; size <= kCrowdSizes; ++size)
    {
        effects.Add("crowd-" + Decimal(size, 2), MakeEffect(JacEffect::kCrowd, size));
    }

    effects.Add("footstep", MakeEffect(JacEffect::kWalk, kFootstep));
}

// Adds each cutscene's sounds, in the order of their first events. A sound's finish counts the game's frames from the
// sound's first event to the cutscene's finish (FramesToFinish). Its later events are the events after that one that
// queue a value.
void AddCutsceneEffects(const std::vector<Cutscene>& cutscenes, EffectChoice& effects)
{
    for (const Cutscene& cutscene : cutscenes)
    {
        for (std::size_t i = 0; i < cutscene.events.size(); ++i)
        {
            const CutsceneEvent& event = cutscene.events[i];
            if (event.value < 0)
            {
                continue;
            }

            JacEffect effect = MakeEffect(JacEffect::kCutscene, static_cast<uint16_t>(event.value),
                                          static_cast<uint16_t>(cutscene.number));
            effect.type = WritesPortOne(cutscene) ? JacEffect::kCutscenePart : 0;
            effect.finish = FramesToFinish(cutscene, i);
            std::vector<JacLaterEvent> later_events;
            for (std::size_t j = i + 1; j < cutscene.events.size(); ++j)
            {
                if (cutscene.events[j].value >= 0)
                {
                    later_events.push_back(
                        {FramesBetween(cutscene, i, j), static_cast<uint16_t>(cutscene.events[j].value)});
                }
            }

            effects.Add(
                "cutscene-" + Decimal(cutscene.number, 3) + "-" + Decimal(static_cast<unsigned>(event.value), 2),
                effect, std::move(later_events));
        }
    }
}

// The ID of the first set of levels below a jump (JacLengthModel::InnerLoop), above every target.
constexpr uint32_t kInnerLoops = 0x80000000;

// The model of the C JAudio for the length analysis (JaudioModelBase), with JAudio2's rules. A frame's output is what
// AudioOut plays of the frame. The FX lines ring while their delays hold a sample louder than xsflib's level of silence
// (jac::Engine::FxRinging), and the output rings while it has sound. The FX lines' feedback can hold a few quieter
// values for ever. Every loop of the main track counts, in a subroutine or a counted loop too. Such a jump counts by
// the levels below it: their places and counts (jac::LoopJump). Its target doesn't count. A random branch can send an
// endless loop in a subroutine back to either of two places. Each set of levels takes an ID above every target, from
// kInnerLoops on. A change of the levels therefore starts the count again.
class JacLengthModel : public JaudioModelBase<jac::Engine, jac::Channels::kDspChannels, jac::kNone>
{
public:
    explicit JacLengthModel(jac::Engine& engine) : JaudioModelBase(engine)
    {
    }

    bool RunFrame()
    {
        engine_.RunFrame();
        TakeOutput();

        return true;
    }

    bool Ringing() const
    {
        return Sounded() || engine_.FxRinging();
    }

    template <typename Hook>
    void OnLoop(Hook hook)
    {
        engine_.SetLoopHook(
            [this, hook](uint32_t track, const jac::LoopJump& jump)
            {
                if (track == MainTrack() && (jump.played || engine_.Held()))
                {
                    hook(static_cast<int>(track), jump.depth == 0 ? jump.target : InnerLoop(jump));
                }
            });
    }

private:
    // The ID of the levels below a jump in a subroutine or a counted loop.
    uint32_t InnerLoop(const jac::LoopJump& jump)
    {
        jac::LoopJump levels;
        levels.depth = jump.depth;
        const auto depth = static_cast<std::ptrdiff_t>(std::min<std::size_t>(jump.depth, levels.stack.size()));
        std::copy_n(jump.stack.begin(), depth, levels.stack.begin());
        std::copy_n(jump.counts.begin(), depth, levels.counts.begin());
        auto found = std::find(levels_.begin(), levels_.end(), levels);
        if (found == levels_.end())
        {
            found = levels_.insert(levels_.end(), levels);
        }

        return kInnerLoops + static_cast<uint32_t>(found - levels_.begin());
    }

    std::vector<jac::LoopJump> levels_; // the levels below the inner loops, by their IDs from kInnerLoops
};

// The later events of the sound with ID `sound`, as the model queues them. An effect's ID holds no bits above its
// place.
std::vector<jac::DemoEvent> DemoEventsOf(const std::vector<std::vector<JacLaterEvent>>& later_events, uint32_t sound)
{
    std::vector<jac::DemoEvent> events;
    if (sound < later_events.size())
    {
        for (const JacLaterEvent& event : later_events[sound])
        {
            events.push_back({event.frames, event.value});
        }
    }

    return events;
}

// Measures the sound with ID `sound` with the events `queued` unheard. The count of notes without a wave goes to
// `missing_bank_notes`.
std::optional<Timing> MeasureJacSound(const jac::SoundFiles& files, const JaudioSettings& jaudio, uint32_t sound,
                                      const std::vector<jac::DemoEvent>& queued, uint32_t& missing_bank_notes)
{
    const auto run = [&]() -> std::optional<Timing>
    {
        jac::Engine engine(files, JacEngineSettings(jaudio));
        if (StartJacSound(engine, jaudio.library3, sound))
        {
            return std::nullopt;
        }

        engine.QueueDemoEvents(queued, true);

        JacLengthModel model(engine);
        const std::optional<Timing> timing = xsflib::rip::MeasureLength(model, JaudioLengthRules());
        missing_bank_notes = engine.MissingBankNotes();

        return timing;
    };

    return xsflib::rip::TryMeasure(run);
}

// The first frame before `end_frame` in which the sound with ID `sound` plays differently with the events `queued`
// unheard and without them. Two engines play the sound side by side. `end_frame` if they agree up to there.
uint64_t FirstDifferentFrame(const jac::SoundFiles& files, const JaudioSettings& jaudio, uint32_t sound,
                             const std::vector<jac::DemoEvent>& queued, uint64_t end_frame)
{
    jac::Engine game(files, JacEngineSettings(jaudio));
    jac::Engine rip(files, JacEngineSettings(jaudio));
    StartJacSound(game, jaudio.library3, sound);
    StartJacSound(rip, jaudio.library3, sound);
    game.QueueDemoEvents(queued, true);
    uint64_t frame = 0;
    for (; frame < end_frame; ++frame)
    {
        game.RunFrame();
        rip.RunFrame();
        if (!std::ranges::equal(game.Output(), rip.Output()))
        {
            break;
        }
    }

    return frame;
}

} // namespace

std::optional<JacPaths> FindJacFiles(const std::vector<std::string>& files)
{
    std::optional<JacPaths> paths;
    for (const std::string& path : files)
    {
        const std::string folder = FolderOf(path);
        if (xsflib::AsciiLower(FileNameOf(path)) != "pikibank.bx" || xsflib::AsciiLower(FileNameOf(folder)) != "banks")
        {
            continue;
        }

        paths.emplace();
        paths->bank = path;
        paths->waves = folder;
        paths->sequences = InFolder(FolderOf(folder), "Seqs/pikiseq.arc");
        const std::string sequences = xsflib::AsciiLower(paths->sequences);
        for (const std::string& other : files)
        {
            if (xsflib::AsciiLower(other) == sequences)
            {
                paths->sequences = other;
            }
        }
        break;
    }

    if (!paths)
    {
        return std::nullopt;
    }

    for (const std::string& path : files)
    {
        if (xsflib::AsciiLower(FileNameOf(path)) == "main" && xsflib::AsciiLower(FileNameOf(FolderOf(path))) == "exefs")
        {
            paths->main = path;
            break;
        }
    }

    return paths;
}

std::string NsoBuildId(std::span<const uint8_t> file)
{
    constexpr std::size_t kBuildIdAt = 0x40;
    constexpr std::size_t kBuildIdSize = 20;
    if (file.size() < kBuildIdAt + kBuildIdSize ||
        std::string_view(reinterpret_cast<const char*>(file.data()), 4) != "NSO0")
    {
        return {};
    }

    std::string id;
    for (const uint8_t byte : file.subspan(kBuildIdAt, kBuildIdSize))
    {
        char text[3];
        std::snprintf(text, sizeof(text), "%02x", static_cast<unsigned>(byte));
        id += text;
    }

    return id;
}

JacGameCode ReadJacGameCode(const ntdlib::nx::Nso& main, const JacTableAddresses& at)
{
    const std::map<uint64_t, uint64_t> pointers = RelativePointers(main);
    JacGameCode code;
    code.sequence_index = ReadSequenceIndex(main, at);
    code.music = ReadMusicTables(main, at, pointers);
    code.fx_lines = ReadFxLines(main, at);
    code.cutscenes = ReadCutscenes(main, at, pointers);
    code.actions = ReadEventActions(main, at);
    code.effects = ReadEffects(main, at, pointers);

    return code;
}

JaudioSettings JacSettings(const JacGameCode& code)
{
    JaudioSettings settings;
    settings.library = JaudioSettings::kPikminLibrary;
    settings.dsp_level = kJacDspLevel;
    settings.fx_lines = code.fx_lines;

    return settings;
}

JacSelection SelectJacSounds(const JacGameCode& code, const std::vector<ntdlib::jaudio::ArchiveFile>& sequences,
                             const std::vector<ntdlib::jaudio::WaveSystem>& systems, const std::regex& only, bool bgm)
{
    JacSelection selection;
    JaudioLibrary3& library = selection.library;
    library.effect_level = code.music.effect_level;
    library.cutscene_level = code.music.cutscene_level;
    library.music_volume = code.music.music_volume;
    library.loads.assign(kLoads.begin(), kLoads.end());

    const std::string cutscene_name = SelectPieces(code, sequences, systems, only, selection);
    SelectParts(code, cutscene_name, systems, only, selection);
    if (bgm)
    {
        return selection;
    }

    // The effects of the effects' table.
    EffectChoice effects(selection, only);
    for (const Effect& row : code.effects)
    {
        if (!KnownType(row))
        {
            selection.warnings.push_back(row.name + " has an unknown type, " + std::to_string(row.event_type) +
                                         "; it's left out");
            continue;
        }

        const std::optional<JacEffect> effect = TableEffect(row, code.actions);
        if (effect)
        {
            effects.Add(row.name, *effect);
        }
    }

    AddCodeEffects(effects);
    AddCutsceneEffects(code.cutscenes, effects);

    return selection;
}

JacLengthAnalyzer::JacLengthAnalyzer(const jac::SoundFiles& files, ArchiveDescriptor settings,
                                     std::vector<std::vector<JacLaterEvent>> later_events)
    : files_(files), settings_(std::move(settings)), later_events_(std::move(later_events))
{
}

JaudioTiming JacLengthAnalyzer::Analyze(uint32_t sound) const
{
    const std::vector<jac::DemoEvent> events = DemoEventsOf(later_events_, sound);
    JaudioTiming result;
    result.timing = MeasureJacSound(files_, settings_.jaudio, sound, events, result.missing_bank_notes);
    if (events.empty() || !result.timing || result.timing->silent || result.timing->fade_ms != 0)
    {
        return result;
    }

    // A second run without the events plays the sound as a player plays the rip. A sound that plays on past the end of
    // the first run is one that an event stops.
    uint32_t missing_alone = 0;
    const std::optional<Timing> alone = MeasureJacSound(files_, settings_.jaudio, sound, {}, missing_alone);
    Timing& timing = *result.timing;
    if (!alone || alone->length_ms + alone->fade_ms <= timing.length_ms)
    {
        return result;
    }

    // The rip of such a sound fades out from the frame in which the game's sound first differs from the rip's. The fade
    // ends where the game's sound ends, without the tail. Two engines play the sound side by side up to there, one with
    // the events unheard and one without them.
    const xsflib::rip::LengthRules rules = JaudioLengthRules();
    const auto end_ms = timing.length_ms - static_cast<long long>(rules.tail_ms);
    const auto end_frame = static_cast<uint64_t>(static_cast<double>(end_ms) / rules.frame_ms);
    const uint64_t frame = FirstDifferentFrame(files_, settings_.jaudio, sound, events, end_frame);
    timing.length_ms = std::min(static_cast<long long>(static_cast<double>(frame) * rules.frame_ms), end_ms);
    timing.fade_ms = end_ms - timing.length_ms;

    return result;
}

} // namespace hsf::rip
