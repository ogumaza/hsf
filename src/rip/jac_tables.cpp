// SPDX-License-Identifier: MIT

// The tables of Pikmin's Switch release in its main (see jac_tables.h).

#include "rip/jac_tables.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
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
    const std::optional<std::span<const uint8_t>> bytes = ntdlib::nx::SegmentBytes(main, address, size);
    if (!bytes)
    {
        ThrowOutside(what);
    }

    return *bytes;
}

std::string MainString(const ntdlib::nx::Nso& main, uint64_t address, std::string_view what)
{
    // The string and its terminating 0 lie in one segment.
    for (std::size_t size = 1;; ++size)
    {
        const std::optional<std::span<const uint8_t>> bytes = ntdlib::nx::SegmentBytes(main, address, size);
        if (!bytes)
        {
            ThrowOutside(what);
        }

        if (bytes->back() == 0)
        {
            return std::string(bytes->begin(), bytes->end() - 1);
        }
    }
}

std::map<uint64_t, uint64_t> RelativePointers(const ntdlib::nx::Nso& main)
{
    const std::map<int64_t, uint64_t> entries = ntdlib::nx::DynamicEntries(main);
    const auto rela_at = entries.find(ntdlib::nx::kDtRela);
    const auto rela_size_at = entries.find(ntdlib::nx::kDtRelaSize);
    if (rela_at == entries.end() || rela_size_at == entries.end())
    {
        throw xsflib::FormatError("main's dynamic section has no DT_RELA or no DT_RELASZ");
    }

    const uint64_t rela = rela_at->second;
    const uint64_t rela_size = rela_size_at->second;
    if (rela_size % ntdlib::nx::kRelocationSize != 0)
    {
        throw xsflib::FormatError("main's relocations don't take 24 bytes each");
    }

    const std::span<const uint8_t> table =
        MainBytes(main, rela, static_cast<std::size_t>(rela_size), "the relocations");
    std::map<uint64_t, uint64_t> pointers;
    for (std::size_t at = 0; at < table.size(); at += ntdlib::nx::kRelocationSize)
    {
        const uint64_t info = xsflib::LoadLe64(table.data() + at + 8);
        if ((info & 0xffffffff) == ntdlib::nx::kRelocationRelative)
        {
            pointers[xsflib::LoadLe64(table.data() + at)] = xsflib::LoadLe64(table.data() + at + 16);
        }
    }

    return pointers;
}

const JacBuild* FindJacBuild(std::string_view build_id)
{
    for (const JacBuild& build : kJacBuilds)
    {
        if (build.build_id == build_id)
        {
            return &build;
        }
    }

    return nullptr;
}

std::vector<Cutscene> ReadCutscenes(const ntdlib::nx::Nso& main, const JacTableAddresses& at,
                                    const std::map<uint64_t, uint64_t>& pointers)
{
    const std::span<const uint8_t> table =
        MainBytes(main, at.cutscenes, kCutscenes * kCutsceneRow, "the cutscenes' table");
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
        uint64_t event_at = PointerAt(main, pointers, at.cutscenes + i * kCutsceneRow + kEventsAt, what);
        if (event_at == 0)
        {
            continue;
        }

        for (;; event_at += kEvent)
        {
            const std::span<const uint8_t> bytes = MainBytes(main, event_at, kEvent, what);
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

uint16_t FramesBetween(const Cutscene& cutscene, std::size_t from, std::size_t to)
{
    uint16_t reached = 0;
    for (std::size_t i = 0; i <= from; ++i)
    {
        reached = std::max(reached, cutscene.events[i].frame);
    }

    uint16_t later = reached;
    for (std::size_t i = from; i <= to; ++i)
    {
        later = std::max(later, cutscene.events[i].frame);
    }

    return static_cast<uint16_t>(later - reached);
}

uint16_t FramesToFinish(const Cutscene& cutscene, std::size_t index)
{
    if (cutscene.events.empty() || cutscene.events.back().value != kFinish || index >= cutscene.events.size())
    {
        return 0;
    }

    return FramesBetween(cutscene, index, cutscene.events.size() - 1);
}

EventActions ReadEventActions(const ntdlib::nx::Nso& main, const JacTableAddresses& at)
{
    EventActions tables;
    const std::span<const uint8_t> first =
        MainBytes(main, at.first_actions, kEventTypes * 4, "the first actions of the events' types");
    for (std::size_t i = 0; i < kEventTypes; ++i)
    {
        tables.first_actions.push_back(static_cast<int32_t>(xsflib::LoadLe32(first.data() + 4 * i)));
    }

    const std::span<const uint8_t> actions = MainBytes(main, at.actions, kActions * kActionRow, "the events' actions");
    for (std::size_t i = 0; i < kActions; ++i)
    {
        const uint8_t* row = actions.data() + i * kActionRow;
        tables.actions.push_back(EventAction{row[0], row[1], row[2], xsflib::LoadLe16(row + 4)});
    }

    return tables;
}

std::vector<Effect> ReadEffects(const ntdlib::nx::Nso& main, const JacTableAddresses& at,
                                const std::map<uint64_t, uint64_t>& pointers)
{
    const std::span<const uint8_t> table =
        MainBytes(main, at.effects, kEffects * kEffectRow, "the game's table of effects");
    std::vector<Effect> effects;
    for (std::size_t i = 0; i < kEffects; ++i)
    {
        const uint8_t* row = table.data() + i * kEffectRow;
        const std::string what = "the name of effect " + std::to_string(i);
        const uint64_t name = RelocatedAt(pointers, at.effects + i * kEffectRow + kNameAt, what);

        Effect& effect = effects.emplace_back();
        effect.number = static_cast<int32_t>(xsflib::LoadLe32(row));
        effect.jaudio = static_cast<int32_t>(xsflib::LoadLe32(row + 4));
        effect.name = MainString(main, name, what);
        effect.loop = static_cast<int32_t>(xsflib::LoadLe32(row + kLoopAt));
        effect.event_type = static_cast<int32_t>(xsflib::LoadLe32(row + kEventTypeAt));
    }

    return effects;
}

std::vector<uint8_t> ReadSequenceIndex(const ntdlib::nx::Nso& main, const JacTableAddresses& at)
{
    const std::span<const uint8_t> head = MainBytes(main, at.sequence_index, kIndexEntriesAt, "the sequence index");
    if (std::string_view(reinterpret_cast<const char*>(head.data()), 4) != "BARC")
    {
        throw xsflib::FormatError("main's sequence index doesn't start with BARC");
    }

    const uint32_t count = xsflib::LoadLe32(head.data() + kIndexCountAt);
    const std::span<const uint8_t> index = MainBytes(
        main, at.sequence_index, kIndexEntriesAt + static_cast<std::size_t>(count) * kIndexEntry, "the sequence index");
    return std::vector<uint8_t>(index.begin(), index.end());
}

MusicTables ReadMusicTables(const ntdlib::nx::Nso& main, const JacTableAddresses& at,
                            const std::map<uint64_t, uint64_t>& pointers)
{
    MusicTables tables;
    for (std::size_t i = 0; i < kPieces; ++i)
    {
        const std::string piece = std::to_string(i + kFirstPieceFile);
        const uint64_t tracks =
            RelocatedAt(pointers, at.playing_tracks + 8 * i, "the child tracks of the piece of file " + piece);
        const uint64_t volumes =
            RelocatedAt(pointers, at.track_volumes + 8 * i, "the volumes of the piece of file " + piece);
        tables.playing_tracks.push_back(
            xsflib::LoadLe16(MainBytes(main, tracks, 2, "the child tracks of the piece of file " + piece).data()));
        tables.track_volumes.push_back(std::bit_cast<float>(
            xsflib::LoadLe32(MainBytes(main, volumes, 4, "the volumes of the piece of file " + piece).data())));
    }

    tables.music_volume = std::bit_cast<float>(Le32(main, at.music_volumes + 4 * kDefaultLevel, "the music's volumes"));
    tables.cutscene_level = xsflib::LoadLe16(
        MainBytes(main, at.cutscene_levels + 2 * kDefaultLevel, 2, "the levels of the cutscenes' music").data());
    tables.effect_level =
        xsflib::LoadLe16(MainBytes(main, at.effect_levels + 2 * kDefaultLevel, 2, "the effects' levels").data());
    for (std::size_t i = 0; i < kParts; ++i)
    {
        tables.part_scenes.push_back(Le32(main, at.part_scenes + 4 * i, "the scenes of the cutscenes' music"));
    }

    return tables;
}

std::array<JaudioFxLine, JaudioSettings::kFxLines> ReadFxLines(const ntdlib::nx::Nso& main, const JacTableAddresses& at)
{
    const std::span<const uint8_t> buses = MainBytes(main, at.buses, 2 * kBuses, "the table of the DSP's buses");
    std::array<JaudioFxLine, JaudioSettings::kFxLines> lines{};
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const std::string what = "the settings of FX line " + std::to_string(i);
        const std::span<const uint8_t> settings =
            MainBytes(main, at.fx_lines + i * kFxLineSettings, kFxLineSettings, what);
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
