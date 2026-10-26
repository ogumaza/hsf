// SPDX-License-Identifier: MIT

// The tables of Pikmin's Switch release in its main (see jac_tables.h).

#include "rip/jac_tables.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/nx/nso.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

namespace hsf::rip
{
namespace
{

// The module's header (MOD0): its tag, read little-endian, and the place of the dynamic section's offset in it.
constexpr uint32_t kMod0 = 0x30444f4d;
constexpr uint64_t kMod0At = 4;
constexpr uint64_t kDynamicOffsetAt = 4;

// The entries of the dynamic section (Elf64_Dyn) and the relocations (Elf64_Rela).
constexpr std::size_t kDynamicEntry = 16;
constexpr uint64_t kDtNull = 0;
constexpr uint64_t kDtRela = 7;
constexpr uint64_t kDtRelaSize = 8;
constexpr std::size_t kRelocation = 24;
constexpr uint32_t kRelative = 1027; // R_AARCH64_RELATIVE

// The places in a row of the cutscenes' table, the events' size, and the values that end a cutscene's events.
constexpr std::size_t kEventsAt = 8;
constexpr std::size_t kEvent = 4;
constexpr int16_t kFinish = -1;
constexpr int16_t kEndEvents = -2;
constexpr int16_t kEndEventsToo = -3;

// The places in a row of the game's table of effects.
constexpr std::size_t kNameAt = 8;
constexpr std::size_t kLoopAt = 0x10;
constexpr std::size_t kEventTypeAt = 0x14;

// The bytes from `address` to the end of the segment that holds it, or an empty span if no segment does. An address at
// a segment's end gives an empty span too.
std::span<const uint8_t> SegmentFrom(const ntdlib::nx::Nso& main, uint64_t address)
{
    for (const ntdlib::nx::NsoSegment* segment : {&main.text, &main.rodata, &main.data})
    {
        if (address >= segment->address && address - segment->address <= segment->bytes.size())
        {
            return std::span<const uint8_t>(segment->bytes)
                .subspan(static_cast<std::size_t>(address - segment->address));
        }
    }

    return {};
}

[[noreturn]] void ThrowOutside(std::string_view what)
{
    throw xsflib::FormatError("main's segments don't hold " + std::string(what));
}

uint32_t Le32(const ntdlib::nx::Nso& main, uint64_t address, std::string_view what)
{
    return xsflib::LoadLe32(MainBytes(main, address, 4, what).data());
}

uint64_t Le64(const ntdlib::nx::Nso& main, uint64_t address, std::string_view what)
{
    return xsflib::LoadLe64(MainBytes(main, address, 8, what).data());
}

// The pointer at `address` as the loader leaves it: the target of its relocation, or 0 for a null pointer. Throws
// FormatError if the pointer isn't null and isn't relocated.
uint64_t PointerAt(const ntdlib::nx::Nso& main, const std::map<uint64_t, uint64_t>& pointers, uint64_t address,
                   std::string_view what)
{
    if (const auto it = pointers.find(address); it != pointers.end())
    {
        return it->second;
    }

    if (Le64(main, address, what) != 0)
    {
        throw xsflib::FormatError("the pointer to " + std::string(what) + " isn't relocated");
    }

    return 0;
}

// The target of the relocated pointer at `address`. Throws FormatError if the loader doesn't write the pointer, as for
// a null pointer.
uint64_t RelocatedAt(const std::map<uint64_t, uint64_t>& pointers, uint64_t address, std::string_view what)
{
    if (const auto it = pointers.find(address); it != pointers.end())
    {
        return it->second;
    }

    throw xsflib::FormatError("the pointer to " + std::string(what) + " isn't relocated");
}

} // namespace

std::span<const uint8_t> MainBytes(const ntdlib::nx::Nso& main, uint64_t address, std::size_t size,
                                   std::string_view what)
{
    for (const ntdlib::nx::NsoSegment* segment : {&main.text, &main.rodata, &main.data})
    {
        if (address >= segment->address && address - segment->address <= segment->bytes.size() &&
            size <= segment->bytes.size() - (address - segment->address))
        {
            return std::span<const uint8_t>(segment->bytes)
                .subspan(static_cast<std::size_t>(address - segment->address), size);
        }
    }

    ThrowOutside(what);
}

std::string MainString(const ntdlib::nx::Nso& main, uint64_t address, std::string_view what)
{
    const std::span<const uint8_t> rest = SegmentFrom(main, address);
    const auto end = std::find(rest.begin(), rest.end(), uint8_t{0});
    if (end == rest.end())
    {
        ThrowOutside(what);
    }

    return std::string(rest.begin(), end);
}

std::map<uint64_t, uint64_t> RelativePointers(const ntdlib::nx::Nso& main)
{
    const uint32_t mod0 = Le32(main, kMod0At, "the offset of the module's header");
    if (Le32(main, mod0, "the module's header") != kMod0)
    {
        throw xsflib::FormatError("main has no MOD0 header");
    }

    const auto offset = static_cast<int32_t>(Le32(main, mod0 + kDynamicOffsetAt, "the module's header"));
    uint64_t rela = 0;
    uint64_t rela_size = 0;
    bool has_rela = false;
    bool has_size = false;
    for (uint64_t at = mod0 + static_cast<uint64_t>(static_cast<int64_t>(offset));; at += kDynamicEntry)
    {
        const std::span<const uint8_t> entry = MainBytes(main, at, kDynamicEntry, "the dynamic section");
        const uint64_t tag = xsflib::LoadLe64(entry.data());
        const uint64_t value = xsflib::LoadLe64(entry.data() + 8);
        if (tag == kDtNull)
        {
            break;
        }

        if (tag == kDtRela)
        {
            rela = value;
            has_rela = true;
        }
        else if (tag == kDtRelaSize)
        {
            rela_size = value;
            has_size = true;
        }
    }

    if (!has_rela || !has_size)
    {
        throw xsflib::FormatError("main's dynamic section has no DT_RELA or no DT_RELASZ");
    }
    if (rela_size % kRelocation != 0)
    {
        throw xsflib::FormatError("main's relocations don't take 24 bytes each");
    }

    const std::span<const uint8_t> table =
        MainBytes(main, rela, static_cast<std::size_t>(rela_size), "the relocations");
    std::map<uint64_t, uint64_t> pointers;
    for (std::size_t at = 0; at < table.size(); at += kRelocation)
    {
        const uint64_t info = xsflib::LoadLe64(table.data() + at + 8);
        if ((info & 0xffffffff) == kRelative)
        {
            pointers[xsflib::LoadLe64(table.data() + at)] = xsflib::LoadLe64(table.data() + at + 16);
        }
    }

    return pointers;
}

std::vector<Cutscene> ReadCutscenes(const ntdlib::nx::Nso& main, const std::map<uint64_t, uint64_t>& pointers)
{
    const std::span<const uint8_t> table =
        MainBytes(main, kCutscenesAt, kCutscenes * kCutsceneRow, "the cutscenes' table");
    std::vector<Cutscene> cutscenes;
    for (std::size_t i = 0; i < kCutscenes; ++i)
    {
        const uint8_t* row = table.data() + i * kCutsceneRow;
        Cutscene& cutscene = cutscenes.emplace_back();
        cutscene.number = static_cast<uint32_t>(i);
        cutscene.start_effect = row[0];
        cutscene.music_fade = row[1];
        cutscene.game_sounds = row[2];
        cutscene.music = row[3];
        cutscene.sequence_flags = row[4];

        const std::string what = "the events of cutscene " + std::to_string(i);
        uint64_t at = PointerAt(main, pointers, kCutscenesAt + i * kCutsceneRow + kEventsAt, what);
        if (at == 0)
        {
            continue;
        }

        for (;; at += kEvent)
        {
            const std::span<const uint8_t> bytes = MainBytes(main, at, kEvent, what);
            const CutsceneEvent event{xsflib::LoadLe16(bytes.data()),
                                      static_cast<int16_t>(xsflib::LoadLe16(bytes.data() + 2))};
            cutscene.events.push_back(event);
            if (event.value == kFinish || event.value == kEndEvents || event.value == kEndEventsToo)
            {
                break;
            }
        }
    }

    return cutscenes;
}

bool WritesPortOne(const Cutscene& cutscene)
{
    return (cutscene.sequence_flags | 2) == 3;
}

uint16_t FramesToFinish(const Cutscene& cutscene, std::size_t index)
{
    if (cutscene.events.empty() || cutscene.events.back().value != kFinish || index >= cutscene.events.size())
    {
        return 0;
    }

    uint16_t reached = 0;
    for (std::size_t i = 0; i <= index; ++i)
    {
        reached = std::max(reached, cutscene.events[i].frame);
    }
    uint16_t finish = reached;
    for (std::size_t i = index; i < cutscene.events.size(); ++i)
    {
        finish = std::max(finish, cutscene.events[i].frame);
    }

    return static_cast<uint16_t>(finish - reached);
}

EventActions ReadEventActions(const ntdlib::nx::Nso& main)
{
    EventActions tables;
    const std::span<const uint8_t> first =
        MainBytes(main, kFirstActionsAt, kEventTypes * 4, "the first actions of the events' types");
    for (std::size_t i = 0; i < kEventTypes; ++i)
    {
        tables.first_actions.push_back(static_cast<int32_t>(xsflib::LoadLe32(first.data() + 4 * i)));
    }

    const std::span<const uint8_t> actions = MainBytes(main, kActionsAt, kActions * kActionRow, "the events' actions");
    for (std::size_t i = 0; i < kActions; ++i)
    {
        const uint8_t* row = actions.data() + i * kActionRow;
        tables.actions.push_back(EventAction{row[0], row[1], row[2], xsflib::LoadLe16(row + 4)});
    }

    return tables;
}

std::vector<Effect> ReadEffects(const ntdlib::nx::Nso& main, const std::map<uint64_t, uint64_t>& pointers)
{
    const std::span<const uint8_t> table =
        MainBytes(main, kEffectsAt, kEffects * kEffectRow, "the game's table of effects");
    std::vector<Effect> effects;
    for (std::size_t i = 0; i < kEffects; ++i)
    {
        const uint8_t* row = table.data() + i * kEffectRow;
        const std::string what = "the name of effect " + std::to_string(i);
        const uint64_t name = RelocatedAt(pointers, kEffectsAt + i * kEffectRow + kNameAt, what);

        Effect& effect = effects.emplace_back();
        effect.number = static_cast<int32_t>(xsflib::LoadLe32(row));
        effect.jaudio = static_cast<int32_t>(xsflib::LoadLe32(row + 4));
        effect.name = MainString(main, name, what);
        effect.loop = static_cast<int32_t>(xsflib::LoadLe32(row + kLoopAt));
        effect.event_type = static_cast<int32_t>(xsflib::LoadLe32(row + kEventTypeAt));
    }

    return effects;
}

std::vector<uint8_t> ReadSequenceIndex(const ntdlib::nx::Nso& main)
{
    const std::span<const uint8_t> head = MainBytes(main, kSequenceIndexAt, kIndexEntriesAt, "the sequence index");
    if (std::string_view(reinterpret_cast<const char*>(head.data()), 4) != "BARC")
    {
        throw xsflib::FormatError("main's sequence index doesn't start with BARC");
    }

    const uint32_t count = xsflib::LoadLe32(head.data() + kIndexCountAt);
    const std::span<const uint8_t> index = MainBytes(
        main, kSequenceIndexAt, kIndexEntriesAt + static_cast<std::size_t>(count) * kIndexEntry, "the sequence index");
    return std::vector<uint8_t>(index.begin(), index.end());
}

MusicTables ReadMusicTables(const ntdlib::nx::Nso& main, const std::map<uint64_t, uint64_t>& pointers)
{
    MusicTables tables;
    for (std::size_t i = 0; i < kPieces; ++i)
    {
        const std::string piece = std::to_string(i + kFirstPieceFile);
        const uint64_t tracks =
            RelocatedAt(pointers, kPlayingTracksAt + 8 * i, "the child tracks of the piece of file " + piece);
        const uint64_t volumes =
            RelocatedAt(pointers, kTrackVolumesAt + 8 * i, "the volumes of the piece of file " + piece);
        tables.playing_tracks.push_back(
            xsflib::LoadLe16(MainBytes(main, tracks, 2, "the child tracks of the piece of file " + piece).data()));
        tables.track_volumes.push_back(std::bit_cast<float>(
            xsflib::LoadLe32(MainBytes(main, volumes, 4, "the volumes of the piece of file " + piece).data())));
    }

    tables.music_volume = std::bit_cast<float>(Le32(main, kMusicVolumesAt + 4 * kDefaultLevel, "the music's volumes"));
    tables.cutscene_level = xsflib::LoadLe16(
        MainBytes(main, kCutsceneLevelsAt + 2 * kDefaultLevel, 2, "the levels of the cutscenes' music").data());
    tables.effect_level =
        xsflib::LoadLe16(MainBytes(main, kEffectLevelsAt + 2 * kDefaultLevel, 2, "the effects' levels").data());
    for (std::size_t i = 0; i < kParts; ++i)
    {
        tables.part_scenes.push_back(Le32(main, kPartScenesAt + 4 * i, "the scenes of the cutscenes' music"));
    }

    return tables;
}

std::array<JaudioFxLine, JaudioSettings::kFxLines> ReadFxLines(const ntdlib::nx::Nso& main)
{
    const std::span<const uint8_t> buses = MainBytes(main, kBusesAt, 2 * kBuses, "the table of the DSP's buses");
    std::array<JaudioFxLine, JaudioSettings::kFxLines> lines{};
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const std::string what = "the settings of FX line " + std::to_string(i);
        const std::span<const uint8_t> settings =
            MainBytes(main, kFxLinesAt + i * kFxLineSettings, kFxLineSettings, what);
        JaudioFxLine& line = lines[i];
        line.flags = settings[0];
        line.subframes = static_cast<uint16_t>(xsflib::LoadLe32(settings.data() + 0xc));
        for (std::size_t k = 0; k < line.buses.size(); ++k)
        {
            const uint16_t bus = xsflib::LoadLe16(settings.data() + 2 + 4 * k);
            if (bus >= kBuses)
            {
                throw xsflib::FormatError(what + " name a bus past the table's end");
            }
            line.buses[k] = xsflib::LoadLe16(buses.data() + 2 * bus);
            line.levels[k] = xsflib::LoadLe16(settings.data() + 4 + 4 * k);
        }
        for (std::size_t k = 0; k < line.filter.size(); ++k)
        {
            line.filter[k] = static_cast<int16_t>(xsflib::LoadLe16(settings.data() + 0x10 + 2 * k));
        }
    }

    return lines;
}

} // namespace hsf::rip
