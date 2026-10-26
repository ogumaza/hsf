// SPDX-License-Identifier: MIT

// The tables in the main of Pikmin's Switch release (src/rip/jac_tables.*), read from a synthetic module. Its three
// segments sit at the release's addresses. The text segment holds a MOD0 header, a dynamic section and the
// relocations. The read-only segment holds the first actions, the actions and the effects' names. The data segment
// holds the game's table of effects, the cutscenes' table and the cutscenes' events. A second module adds the
// sequence index, the music's tables and the FX lines. The test then damages the modules in each way that the readers
// refuse.

#include "rip/jac_tables.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ntdlib/nx/nso.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

using namespace xsflib::testing;

namespace
{

namespace rip = hsf::rip;

// The synthetic module's layout.
constexpr uint64_t kMod0At = 0x10;
constexpr uint64_t kDynamicAt = 0x40;
constexpr uint64_t kRelocationsAt = 0x100;
constexpr std::size_t kTextSize = 0x4000;
constexpr uint64_t kRodataAt = 0x5b7000;
constexpr std::size_t kRodataSize = 0x8000;
constexpr uint64_t kNamesAt = 0x5b8000;
constexpr uint64_t kName = 16;
constexpr uint64_t kDataAt = 0x68d000;
constexpr std::size_t kDataSize = 0x22000;
constexpr uint64_t kEventsAt = 0x6aee00;

// Relocation types: a relative one, and one that the readers leave out (R_AARCH64_GLOB_DAT).
constexpr uint64_t kRelative = 1027;
constexpr uint64_t kGlobalData = 1025;

// The release's first actions of the 8 types of event.
const std::vector<int32_t> kFirstActions = {0, 1, 173, 190, 205, 215, 219, 261};

struct Relocation
{
    uint64_t at = 0;
    uint64_t type = kRelative;
    uint64_t target = 0;
};

// A module as the test builds it: its segments, and the relocations that Finish writes into the text segment.
struct Module
{
    ntdlib::nx::Nso nso;
    std::vector<Relocation> relocations;
};

uint8_t* At(Module& module, uint64_t address)
{
    for (ntdlib::nx::NsoSegment* segment : {&module.nso.text, &module.nso.rodata, &module.nso.data})
    {
        if (address >= segment->address && address < segment->address + segment->bytes.size())
        {
            return segment->bytes.data() + (address - segment->address);
        }
    }

    return nullptr;
}

void Put16(Module& module, uint64_t address, uint16_t value)
{
    xsflib::StoreLe16(At(module, address), value);
}

void Put32(Module& module, uint64_t address, uint32_t value)
{
    xsflib::StoreLe32(At(module, address), value);
}

void Put64(Module& module, uint64_t address, uint64_t value)
{
    xsflib::StoreLe64(At(module, address), value);
}

// Writes the MOD0 header, a dynamic section that gives `rela_size` as DT_RELASZ, and the module's relocations.
void Finish(Module& module, uint64_t rela_size)
{
    Put32(module, 4, static_cast<uint32_t>(kMod0At));
    Put32(module, kMod0At, 0x30444f4d);
    Put32(module, kMod0At + 4, static_cast<uint32_t>(kDynamicAt - kMod0At));

    Put64(module, kDynamicAt, 7);
    Put64(module, kDynamicAt + 8, kRelocationsAt);
    Put64(module, kDynamicAt + 16, 8);
    Put64(module, kDynamicAt + 24, rela_size);
    Put64(module, kDynamicAt + 32, 0);

    for (std::size_t i = 0; i < module.relocations.size(); ++i)
    {
        const Relocation& relocation = module.relocations[i];
        Put64(module, kRelocationsAt + 24 * i, relocation.at);
        Put64(module, kRelocationsAt + 24 * i + 8, relocation.type);
        Put64(module, kRelocationsAt + 24 * i + 16, relocation.target);
    }
}

void Finish(Module& module)
{
    Finish(module, 24 * module.relocations.size());
}

// The values that the synthetic tables hold: each action's, each effect's name, and each cutscene's values besides its
// events.
rip::EventAction ActionOf(std::size_t row)
{
    return {static_cast<uint8_t>(row % 0x40), static_cast<uint8_t>(row % 5), static_cast<uint8_t>(row % 31),
            static_cast<uint16_t>(0x100 * (row % 3) + row)};
}

std::string NameOf(std::size_t row)
{
    return "SE_TEST_" + std::to_string(row);
}

rip::Cutscene CutsceneOf(std::size_t row)
{
    rip::Cutscene cutscene;
    cutscene.number = static_cast<uint32_t>(row);
    cutscene.start_effect = static_cast<uint8_t>(row % 5);
    cutscene.music_fade = static_cast<uint8_t>(row % 6);
    cutscene.game_sounds = static_cast<uint8_t>(row % 3);
    cutscene.music = static_cast<uint8_t>(row * 37);
    cutscene.sequence_flags = static_cast<uint8_t>(row % 4);

    return cutscene;
}

// Cutscene 1's events end with -1, cutscene 2's with -2 and cutscene 3's with -3. Cutscene 3 has an event at frame
// 40,000, past the frames of 15 bits, and an event after its last that a reader mustn't take. The other cutscenes have
// no events.
const std::vector<std::vector<rip::CutsceneEvent>> kEvents = {
    {{4, -6}, {2000, -1}},
    {{0, 0}, {160, 1}, {600, -2}},
    {{40000, 5}, {65535, -3}},
};

// The module that the readers read whole.
Module GoodModule()
{
    Module module;
    module.nso.text = {0, std::vector<uint8_t>(kTextSize)};
    module.nso.rodata = {static_cast<uint32_t>(kRodataAt), std::vector<uint8_t>(kRodataSize)};
    module.nso.data = {static_cast<uint32_t>(kDataAt), std::vector<uint8_t>(kDataSize)};

    for (std::size_t i = 0; i < kFirstActions.size(); ++i)
    {
        Put32(module, rip::kFirstActionsAt + 4 * i, static_cast<uint32_t>(kFirstActions[i]));
    }

    // Each action's fourth byte isn't one of its values.
    for (std::size_t i = 0; i < rip::kActions; ++i)
    {
        const rip::EventAction action = ActionOf(i);
        uint8_t* row = At(module, rip::kActionsAt + rip::kActionRow * i);
        row[0] = action.flags;
        row[1] = action.priority;
        row[2] = action.group;
        row[3] = 0xee;
        xsflib::StoreLe16(row + 4, action.command);
    }

    for (std::size_t i = 0; i < rip::kEffects; ++i)
    {
        const uint64_t row = rip::kEffectsAt + rip::kEffectRow * i;
        const std::string name = NameOf(i);
        std::copy(name.begin(), name.end(), At(module, kNamesAt + kName * i));
        Put32(module, row, static_cast<uint32_t>(i));
        Put32(module, row + 4, static_cast<uint32_t>(1000 + i));
        module.relocations.push_back({row + 8, kRelative, kNamesAt + kName * i});
        Put32(module, row + 0x10, static_cast<uint32_t>(i % 2));
        Put32(module, row + 0x14, static_cast<uint32_t>(static_cast<int32_t>(i % 9) - 2));
    }

    for (std::size_t i = 0; i < rip::kCutscenes; ++i)
    {
        const rip::Cutscene cutscene = CutsceneOf(i);
        uint8_t* row = At(module, rip::kCutscenesAt + rip::kCutsceneRow * i);
        row[0] = cutscene.start_effect;
        row[1] = cutscene.music_fade;
        row[2] = cutscene.game_sounds;
        row[3] = cutscene.music;
        row[4] = cutscene.sequence_flags;
    }

    // The events of cutscenes 1 to 3, one list after another, and the event that follows the last list.
    uint64_t at = kEventsAt;
    for (std::size_t i = 0; i < kEvents.size(); ++i)
    {
        module.relocations.push_back({rip::kCutscenesAt + rip::kCutsceneRow * (i + 1) + 8, kRelative, at});
        for (const rip::CutsceneEvent& event : kEvents[i])
        {
            Put16(module, at, event.frame);
            Put16(module, at + 2, static_cast<uint16_t>(event.value));
            at += 4;
        }
    }
    Put16(module, at, 1);
    Put16(module, at + 2, 7);

    // A relocation of another type, which the readers leave out.
    module.relocations.push_back({kDataAt, kGlobalData, kNamesAt});
    Finish(module);

    return module;
}

bool SameAction(const rip::EventAction& a, const rip::EventAction& b)
{
    return a.flags == b.flags && a.priority == b.priority && a.group == b.group && a.command == b.command;
}

bool SameCutscene(const rip::Cutscene& a, const rip::Cutscene& b)
{
    if (a.number != b.number || a.start_effect != b.start_effect || a.music_fade != b.music_fade ||
        a.game_sounds != b.game_sounds || a.music != b.music || a.sequence_flags != b.sequence_flags ||
        a.events.size() != b.events.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < a.events.size(); ++i)
    {
        if (a.events[i].frame != b.events[i].frame || a.events[i].value != b.events[i].value)
        {
            return false;
        }
    }

    return true;
}

void TestRelocations()
{
    const Module module = GoodModule();

    const std::map<uint64_t, uint64_t> pointers = rip::RelativePointers(module.nso);

    Check(pointers.size() == rip::kEffects + kEvents.size(),
          "the relative relocations, " + std::to_string(pointers.size()));
    Check(pointers.count(kDataAt) == 0, "a relocation of another type is left out");
    const auto name = pointers.find(rip::kEffectsAt + rip::kEffectRow * 5 + 8);
    Check(name != pointers.end() && name->second == kNamesAt + kName * 5, "effect 5's name");
}

void TestCutscenes()
{
    const Module module = GoodModule();

    const std::vector<rip::Cutscene> cutscenes = rip::ReadCutscenes(module.nso, rip::RelativePointers(module.nso));

    XSFLIB_CHECK(cutscenes.size() == rip::kCutscenes);
    for (std::size_t i = 0; i < cutscenes.size(); ++i)
    {
        rip::Cutscene expected = CutsceneOf(i);
        if (i >= 1 && i <= kEvents.size())
        {
            expected.events = kEvents[i - 1];
        }
        Check(SameCutscene(cutscenes[i], expected), "cutscene " + std::to_string(i));
        Check(rip::WritesPortOne(cutscenes[i]) == (i % 4 == 1 || i % 4 == 3),
              "cutscene " + std::to_string(i) + "'s port 1");
    }
}

// Each sound's frames to the finish, from the frame in which Jac_DemoFrame reaches the sound's event. An event whose
// frame comes before an earlier event's waits for that one.
void TestFramesToFinish()
{
    rip::Cutscene cutscene;
    cutscene.events = {{5, -6}, {2, 0}, {100, 1}, {100, 2}, {700, -1}};

    XSFLIB_CHECK(rip::FramesToFinish(cutscene, 1) == 695);
    XSFLIB_CHECK(rip::FramesToFinish(cutscene, 2) == 600);
    XSFLIB_CHECK(rip::FramesToFinish(cutscene, 3) == 600);
    XSFLIB_CHECK(rip::FramesToFinish(cutscene, 4) == 0);
    XSFLIB_CHECK(rip::FramesToFinish(cutscene, 5) == 0);

    for (const int16_t end : {-2, -3})
    {
        cutscene.events.back().value = end;
        Check(rip::FramesToFinish(cutscene, 1) == 0, "events that end with " + std::to_string(end));
    }

    cutscene.events.clear();
    XSFLIB_CHECK(rip::FramesToFinish(cutscene, 0) == 0);
}

void TestActions()
{
    const Module module = GoodModule();

    const rip::EventActions tables = rip::ReadEventActions(module.nso);

    XSFLIB_CHECK(tables.first_actions == kFirstActions);
    XSFLIB_CHECK(tables.actions.size() == rip::kActions);
    for (std::size_t i = 0; i < tables.actions.size(); ++i)
    {
        Check(SameAction(tables.actions[i], ActionOf(i)), "action " + std::to_string(i));
    }
}

void TestEffects()
{
    const Module module = GoodModule();

    const std::vector<rip::Effect> effects = rip::ReadEffects(module.nso, rip::RelativePointers(module.nso));

    XSFLIB_CHECK(effects.size() == rip::kEffects);
    for (std::size_t i = 0; i < effects.size(); ++i)
    {
        const rip::Effect& e = effects[i];
        Check(e.number == static_cast<int32_t>(i) && e.jaudio == static_cast<int32_t>(1000 + i) &&
                  e.name == NameOf(i) && e.loop == static_cast<int32_t>(i % 2) &&
                  e.event_type == static_cast<int32_t>(i % 9) - 2,
              "effect " + std::to_string(i));
    }
}

void TestBounds()
{
    const Module module = GoodModule();
    const uint64_t kEnd = kRodataAt + kRodataSize;

    XSFLIB_CHECK(rip::MainBytes(module.nso, kRodataAt, kRodataSize, "it").size() == kRodataSize);
    XSFLIB_CHECK(rip::MainBytes(module.nso, kEnd, 0, "it").empty());
    XSFLIB_CHECK(rip::MainString(module.nso, kNamesAt, "it") == NameOf(0));
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { rip::MainBytes(module.nso, kEnd - 1, 2, "a test's bytes"); }) ==
                 "main's segments don't hold a test's bytes");
    XSFLIB_CHECK(!ErrorOf<xsflib::FormatError>([&] { rip::MainBytes(module.nso, kEnd + 1, 0, "it"); }).empty());
    XSFLIB_CHECK(!ErrorOf<xsflib::FormatError>([&] { rip::MainBytes(module.nso, kTextSize, 1, "it"); }).empty());
    XSFLIB_CHECK(!ErrorOf<xsflib::FormatError>([&] { rip::MainString(module.nso, kEnd, "it"); }).empty());
}

// The message that the readers give for the good module after `damage` changes it.
template <typename Damage>
std::string ErrorAfter(Damage damage)
{
    Module module = GoodModule();
    damage(module);

    const auto read = [&]
    {
        const std::map<uint64_t, uint64_t> pointers = rip::RelativePointers(module.nso);
        rip::ReadCutscenes(module.nso, pointers);
        rip::ReadEventActions(module.nso);
        rip::ReadEffects(module.nso, pointers);
    };

    return ErrorOf<xsflib::FormatError>(read);
}

void TestDamage()
{
    // The module's header and its dynamic section.
    XSFLIB_CHECK(ErrorAfter([](Module& m) { Put32(m, kMod0At, 0); }) == "main has no MOD0 header");
    XSFLIB_CHECK(ErrorAfter([](Module& m) { Put32(m, 4, static_cast<uint32_t>(kTextSize)); }) ==
                 "main's segments don't hold the module's header");
    XSFLIB_CHECK(ErrorAfter([](Module& m) { Put64(m, kDynamicAt + 16, 0); }) ==
                 "main's dynamic section has no DT_RELA or no DT_RELASZ");
    XSFLIB_CHECK(ErrorAfter([](Module& m) { Finish(m, 24 * m.relocations.size() + 1); }) ==
                 "main's relocations don't take 24 bytes each");
    XSFLIB_CHECK(ErrorAfter([](Module& m) { Finish(m, 24 * 1000); }) == "main's segments don't hold the relocations");

    // The tables, cut short.
    XSFLIB_CHECK(ErrorAfter([](Module& m) { m.nso.data.bytes.resize(rip::kCutscenesAt + 0x700 - kDataAt); }) ==
                 "main's segments don't hold the cutscenes' table");
    XSFLIB_CHECK(ErrorAfter([](Module& m) { m.nso.rodata.bytes.resize(rip::kActionsAt + 0x100 - kRodataAt); }) ==
                 "main's segments don't hold the events' actions");
    XSFLIB_CHECK(ErrorAfter([](Module& m) { m.nso.rodata.bytes.resize(0x400); }) ==
                 "main's segments don't hold the first actions of the events' types");

    // Cutscene 3's events without their last, up to the data segment's end, and a pointer of events that isn't
    // relocated.
    const auto endless = [](Module& m)
    {
        for (uint64_t at = kEventsAt + 4 * 5; at < kDataAt + kDataSize; at += 4)
        {
            Put32(m, at, 1);
        }
    };
    XSFLIB_CHECK(ErrorAfter(endless) == "main's segments don't hold the events of cutscene 3");
    XSFLIB_CHECK(ErrorAfter([](Module& m) { Put64(m, rip::kCutscenesAt + rip::kCutsceneRow * 4 + 8, kEventsAt); }) ==
                 "the pointer to the events of cutscene 4 isn't relocated");

    // An effect whose name has a relocation of another type, and a name without its NUL at the read-only segment's
    // end.
    const auto unrelocated = [](Module& m)
    {
        m.relocations[7].type = kGlobalData;
        Finish(m);
    };
    XSFLIB_CHECK(ErrorAfter(unrelocated) == "the pointer to the name of effect 7 isn't relocated");
    const auto unended = [](Module& m)
    {
        const uint64_t kLast = kRodataAt + kRodataSize - 4;
        Put32(m, kLast, 0x41414141);
        m.relocations[9].target = kLast;
        Finish(m);
    };
    XSFLIB_CHECK(ErrorAfter(unended) == "main's segments don't hold the name of effect 9");
}

// The second module's segments are longer. Its read-only segment holds each piece's sets of child tracks and volumes
// at these addresses, and the table of buses. Its data segment holds the sequence index, the pointers of the music's
// tables and the FX lines.
constexpr std::size_t kLongRodataSize = 0x9000;
constexpr std::size_t kLongDataSize = 0x23000;
constexpr uint64_t kTrackSetsAt = 0x5bf100;
constexpr uint64_t kVolumePairsAt = 0x5bf300;
constexpr uint32_t kSequences = 3;

// The values of the second module's tables: a piece's first set of child tracks and its first volume, the music's
// volume and the two levels at each option's level, the scene of a piece of the cutscenes' music, a bus's address, and
// the settings of an FX line.
uint16_t TracksOf(std::size_t piece)
{
    return static_cast<uint16_t>(0x8000u >> piece | piece);
}

float VolumeOf(std::size_t piece)
{
    return 0.05f * static_cast<float>(piece);
}

float MusicVolumeOf(std::size_t level)
{
    return 0.1f * static_cast<float>(level);
}

uint16_t CutsceneLevelOf(std::size_t level)
{
    return static_cast<uint16_t>(1000 * level + 1);
}

uint16_t EffectLevelOf(std::size_t level)
{
    return static_cast<uint16_t>(1000 * level + 2);
}

uint32_t PartSceneOf(std::size_t part)
{
    return static_cast<uint32_t>(3 * part + 1);
}

uint16_t BusOf(std::size_t place)
{
    return place + 1 < rip::kBuses ? static_cast<uint16_t>(0x100 * (place + 1) + 0x40) : uint16_t{0};
}

// FX line i feeds the bus at place 2i and the last place, which feeds no bus.
hsf::JaudioFxLine LineOf(std::size_t line)
{
    hsf::JaudioFxLine settings;
    settings.flags = static_cast<uint16_t>(line);
    settings.subframes = static_cast<uint16_t>(10 * line + 3);
    settings.buses = {BusOf(2 * line), BusOf(rip::kBuses - 1)};
    settings.levels = {static_cast<uint16_t>(0x1000 * (line + 1)), static_cast<uint16_t>(0x200 + line)};
    for (std::size_t k = 0; k < settings.filter.size(); ++k)
    {
        settings.filter[k] = static_cast<int16_t>(1000 * (static_cast<int>(k) - 4) + static_cast<int>(line));
    }

    return settings;
}

// The sequence index's bytes: its tag, its count of sequences at +16 and their entries from +36.
std::vector<uint8_t> IndexBytes()
{
    std::vector<uint8_t> index(rip::kIndexEntriesAt + kSequences * rip::kIndexEntry);
    for (std::size_t i = 0; i < index.size(); ++i)
    {
        index[i] = static_cast<uint8_t>(7 * i + 1);
    }
    std::copy_n("BARC", 4, index.begin());
    xsflib::StoreLe32(index.data() + rip::kIndexCountAt, kSequences);

    return index;
}

// The second module.
Module MusicModule()
{
    Module module = GoodModule();
    module.nso.rodata.bytes.resize(kLongRodataSize);
    module.nso.data.bytes.resize(kLongDataSize);

    // The sequence index, then a byte that the reader mustn't take.
    const std::vector<uint8_t> index = IndexBytes();
    std::copy(index.begin(), index.end(), At(module, rip::kSequenceIndexAt));
    *At(module, rip::kSequenceIndexAt + index.size()) = 0x55;

    // Each piece's pointers, with a set and a volume of another mode after the first.
    for (std::size_t i = 0; i < rip::kPieces; ++i)
    {
        module.relocations.push_back({rip::kPlayingTracksAt + 8 * i, kRelative, kTrackSetsAt + 8 * i});
        module.relocations.push_back({rip::kTrackVolumesAt + 8 * i, kRelative, kVolumePairsAt + 8 * i});
        Put16(module, kTrackSetsAt + 8 * i, TracksOf(i));
        Put16(module, kTrackSetsAt + 8 * i + 2, 0x1111);
        Put32(module, kVolumePairsAt + 8 * i, std::bit_cast<uint32_t>(VolumeOf(i)));
        Put32(module, kVolumePairsAt + 8 * i + 4, std::bit_cast<uint32_t>(2.0f));
    }

    for (std::size_t level = 0; level < rip::kLevels; ++level)
    {
        Put32(module, rip::kMusicVolumesAt + 4 * level, std::bit_cast<uint32_t>(MusicVolumeOf(level)));
        Put16(module, rip::kCutsceneLevelsAt + 2 * level, CutsceneLevelOf(level));
        Put16(module, rip::kEffectLevelsAt + 2 * level, EffectLevelOf(level));
    }
    for (std::size_t i = 0; i < rip::kParts; ++i)
    {
        Put32(module, rip::kPartScenesAt + 4 * i, PartSceneOf(i));
    }

    // The table of buses and the FX lines' settings. Each line's second byte isn't one of its values.
    for (std::size_t place = 0; place < rip::kBuses; ++place)
    {
        Put16(module, rip::kBusesAt + 2 * place, BusOf(place));
    }
    for (std::size_t i = 0; i < hsf::JaudioSettings::kFxLines; ++i)
    {
        const uint64_t line = rip::kFxLinesAt + rip::kFxLineSettings * i;
        const hsf::JaudioFxLine settings = LineOf(i);
        *At(module, line) = static_cast<uint8_t>(settings.flags);
        *At(module, line + 1) = 0xee;
        Put16(module, line + 2, static_cast<uint16_t>(2 * i));
        Put16(module, line + 4, settings.levels[0]);
        Put16(module, line + 6, static_cast<uint16_t>(rip::kBuses - 1));
        Put16(module, line + 8, settings.levels[1]);
        Put32(module, line + 0xc, settings.subframes);
        for (std::size_t k = 0; k < settings.filter.size(); ++k)
        {
            Put16(module, line + 0x10 + 2 * k, static_cast<uint16_t>(settings.filter[k]));
        }
    }

    Finish(module);

    return module;
}

void TestSequenceIndex()
{
    const Module module = MusicModule();

    XSFLIB_CHECK(rip::ReadSequenceIndex(module.nso) == IndexBytes());
}

void TestMusicTables()
{
    const Module module = MusicModule();

    const rip::MusicTables tables = rip::ReadMusicTables(module.nso, rip::RelativePointers(module.nso));

    XSFLIB_CHECK(tables.playing_tracks.size() == rip::kPieces && tables.track_volumes.size() == rip::kPieces);
    for (std::size_t i = 0; i < rip::kPieces && i < tables.playing_tracks.size() && i < tables.track_volumes.size();
         ++i)
    {
        Check(tables.playing_tracks[i] == TracksOf(i) && tables.track_volumes[i] == VolumeOf(i),
              "the piece of file " + std::to_string(i + rip::kFirstPieceFile));
    }
    XSFLIB_CHECK(tables.music_volume == MusicVolumeOf(rip::kDefaultLevel));
    XSFLIB_CHECK(tables.cutscene_level == CutsceneLevelOf(rip::kDefaultLevel));
    XSFLIB_CHECK(tables.effect_level == EffectLevelOf(rip::kDefaultLevel));
    XSFLIB_CHECK(
        tables.part_scenes ==
        (std::vector<uint32_t>{PartSceneOf(0), PartSceneOf(1), PartSceneOf(2), PartSceneOf(3), PartSceneOf(4),
                               PartSceneOf(5), PartSceneOf(6), PartSceneOf(7), PartSceneOf(8), PartSceneOf(9)}));
}

void TestFxLines()
{
    const Module module = MusicModule();

    const std::array<hsf::JaudioFxLine, hsf::JaudioSettings::kFxLines> lines = rip::ReadFxLines(module.nso);

    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        Check(lines[i] == LineOf(i), "FX line " + std::to_string(i));
    }
}

// The message that the readers of the second module's tables give after `damage` changes it.
template <typename Damage>
std::string MusicErrorAfter(Damage damage)
{
    Module module = MusicModule();
    damage(module);

    const auto read = [&]
    {
        rip::ReadSequenceIndex(module.nso);
        rip::ReadMusicTables(module.nso, rip::RelativePointers(module.nso));
        rip::ReadFxLines(module.nso);
    };

    return ErrorOf<xsflib::FormatError>(read);
}

// Removes the relocation of the pointer at `at`, and leaves `value` in its place.
void Unrelocate(Module& module, uint64_t at, uint64_t value)
{
    std::erase_if(module.relocations, [at](const Relocation& relocation) { return relocation.at == at; });
    Put64(module, at, value);
    Finish(module);
}

void TestMusicDamage()
{
    XSFLIB_CHECK(MusicErrorAfter([](Module&) {}).empty());

    // The sequence index without its tag, and with more sequences than the data segment holds.
    XSFLIB_CHECK(MusicErrorAfter([](Module& m) { *At(m, rip::kSequenceIndexAt) = 'b'; }) ==
                 "main's sequence index doesn't start with BARC");
    XSFLIB_CHECK(MusicErrorAfter([](Module& m) { Put32(m, rip::kSequenceIndexAt + rip::kIndexCountAt, 1000); }) ==
                 "main's segments don't hold the sequence index");

    // A pointer of the music's tables without its relocation, null or not, and a table past the read-only segment.
    XSFLIB_CHECK(MusicErrorAfter([](Module& m) { Unrelocate(m, rip::kTrackVolumesAt + 8 * 5, kVolumePairsAt); }) ==
                 "the pointer to the volumes of the piece of file 7 isn't relocated");
    XSFLIB_CHECK(MusicErrorAfter([](Module& m) { Unrelocate(m, rip::kPlayingTracksAt + 8 * 2, 0); }) ==
                 "the pointer to the child tracks of the piece of file 4 isn't relocated");
    const auto far = [](Module& m)
    {
        for (Relocation& relocation : m.relocations)
        {
            if (relocation.at == rip::kPlayingTracksAt + 8 * 3)
            {
                relocation.target = kRodataAt + kLongRodataSize - 1;
            }
        }
        Finish(m);
    };
    XSFLIB_CHECK(MusicErrorAfter(far) == "main's segments don't hold the child tracks of the piece of file 5");

    // An FX line that names a bus past the table's end, the table of buses cut short and the FX lines cut short.
    XSFLIB_CHECK(MusicErrorAfter([](Module& m) { Put16(m, rip::kFxLinesAt + rip::kFxLineSettings * 2 + 6, 12); }) ==
                 "the settings of FX line 2 name a bus past the table's end");
    XSFLIB_CHECK(MusicErrorAfter([](Module& m) { m.nso.rodata.bytes.resize(rip::kBusesAt + 4 - kRodataAt); }) ==
                 "main's segments don't hold the table of the DSP's buses");

    // The data segment cut in FX line 3's settings. The sequence index lies past them.
    Module cut = MusicModule();
    cut.nso.data.bytes.resize(rip::kFxLinesAt + rip::kFxLineSettings * 3 + 4 - kDataAt);
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { rip::ReadFxLines(cut.nso); }) ==
                 "main's segments don't hold the settings of FX line 3");
}

} // namespace

int main()
{
    const auto tests = []
    {
        TestRelocations();
        TestCutscenes();
        TestFramesToFinish();
        TestActions();
        TestEffects();
        TestBounds();
        TestDamage();
        TestSequenceIndex();
        TestMusicTables();
        TestFxLines();
        TestMusicDamage();
    };

    return RunTest("jac_tables", tests);
}
