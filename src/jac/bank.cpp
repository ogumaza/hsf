// SPDX-License-Identifier: MIT

#include "jac/bank.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace hsf::jac
{

namespace
{

// The places of the game's tables of banks and wave systems.
constexpr uint32_t kPlaces = 0x100;

// The deepest chain of scenes that the model follows through their dependencies. The game's search and close follow a
// chain without limit, and a cycle never ends there.
constexpr uint32_t kMaxDepth = 64;

constexpr uint32_t Align32(uint32_t size)
{
    return (size + 0x1f) & ~0x1fu;
}

jaudio::WaveInfo WaveInfoOf(const ntdlib::jaudio::WaveInfo& wave)
{
    jaudio::WaveInfo info;
    info.format = static_cast<uint8_t>(wave.format);
    info.base_key = wave.base_key;
    info.sample_rate = wave.sample_rate;
    info.loop_flags = wave.loop ? 0xffffffff : 0;
    info.loop_start = wave.loop_start;
    info.loop_end = wave.loop_end;
    info.sample_count = wave.sample_count;
    info.last = wave.loop_last;
    info.penult = wave.loop_penult;

    return info;
}

std::vector<OscPoint> PointsOf(const std::vector<ntdlib::jaudio::OscPoint>& points)
{
    std::vector<OscPoint> converted;
    converted.reserve(points.size());
    for (const ntdlib::jaudio::OscPoint& p : points)
    {
        converted.push_back({p.mode, p.time, p.value});
    }

    return converted;
}

OscTable TableOf(const std::vector<OscPoint>& points)
{
    return points.empty() ? OscTable{} : OscTable{points.data(), points.size()};
}

// The handle of `id` in a list: the first whose word holds the id in its low half, or null.
template <typename Handles>
auto FirstHandle(Handles& list, uint16_t id) -> decltype(&list[0])
{
    for (auto& handle : list)
    {
        if (static_cast<uint16_t>(handle.id) == id)
        {
            return &handle;
        }
    }

    return nullptr;
}

} // namespace

Aram::Aram(uint32_t aram_size)
{
    Heap mother;
    mother.start = kStart;
    mother.size = aram_size > kStart + 0x20 ? aram_size - kStart - 0x20 : 0;
    heaps_.push_back(mother);
}

uint32_t Aram::NewHeap()
{
    heaps_.emplace_back();

    return static_cast<uint32_t>(heaps_.size() - 1);
}

bool Aram::Alloc(uint32_t heap, uint32_t size)
{
    Heap& mother = heaps_[kMother];
    Heap& h = heaps_[heap];
    if (mother.start == 0 || (h.start != 0 && h.start != 0xffffffff))
    {
        return false;
    }

    const uint32_t aligned = Align32(size);
    if (mother.size - mother.used >= aligned)
    {
        // At the end of what the mother heap has given. The heap becomes the mother heap's last child.
        h.start = mother.start + mother.used;
        h.used = 0;
        h.size = aligned;
        h.children = 0;
        h.first_child = kNone;
        h.parent = kMother;
        h.next = kNone;
        if (mother.first_child == kNone)
        {
            mother.first_child = heap;
        }
        else
        {
            uint32_t last = mother.first_child;
            while (heaps_[last].next != kNone)
            {
                last = heaps_[last].next;
            }
            heaps_[last].next = heap;
        }

        mother.used += aligned;
        mother.children++;

        return true;
    }

    // The tightest gap before a child that holds the heap. The search starts at the mother heap's start, and leaves
    // out the space after the last child.
    uint32_t best = kNone;
    uint32_t best_start = 0;
    uint32_t best_spare = 0x0fffffff;
    uint32_t end = mother.start;
    for (uint32_t child = mother.first_child; child != kNone; child = heaps_[child].next)
    {
        const uint32_t gap = heaps_[child].start - end;
        const uint32_t spare = gap - aligned;
        if (aligned <= gap && spare < best_spare)
        {
            best = child;
            best_start = end;
            best_spare = spare;
        }
        end = heaps_[child].start + heaps_[child].size;
    }

    if (best == kNone)
    {
        return false;
    }

    h.next = best;
    if (mother.first_child == best)
    {
        mother.first_child = heap;
    }
    else
    {
        uint32_t before = mother.first_child;
        while (heaps_[before].next != best)
        {
            before = heaps_[before].next;
        }
        heaps_[before].next = heap;
    }

    h.start = best_start;
    h.used = 0;
    h.size = aligned;
    h.children = 0;
    h.first_child = kNone;
    h.parent = kMother;
    mother.children++;

    return true;
}

bool Aram::SelfAlloc(uint32_t heap, uint32_t parent, uint32_t size, uint32_t start)
{
    Heap& h = heaps_[heap];
    Heap& p = heaps_[parent];
    if (h.start != 0 && h.start != 0xffffffff)
    {
        return false;
    }

    h.start = start;
    h.used = 0;
    h.size = size;
    h.children = 0;
    h.first_child = kNone;
    h.parent = parent;

    // Before the first child that starts after the heap. A heap at the end moves the parent's end of use.
    bool last = false;
    if (p.first_child == kNone)
    {
        p.first_child = heap;
        h.next = kNone;
        last = true;
    }
    else if (heaps_[p.first_child].start > start)
    {
        h.next = p.first_child;
        p.first_child = heap;
    }
    else
    {
        uint32_t before = p.first_child;
        for (;;)
        {
            const uint32_t after = heaps_[before].next;
            if (after == kNone)
            {
                h.next = kNone;
                heaps_[before].next = heap;
                last = true;
                break;
            }
            if (heaps_[after].start > start)
            {
                h.next = after;
                heaps_[before].next = heap;
                break;
            }
            before = after;
        }
    }

    if (last)
    {
        p.used = (h.start - p.start) + h.size;
    }
    p.children++;

    return true;
}

void Aram::SelfInit(uint32_t heap, uint32_t start, uint32_t size)
{
    Heap& h = heaps_[heap];
    h.start = start;
    h.used = 0;
    h.size = size;
    h.children = 0;
    h.first_child = kNone;
    h.parent = kNone;
    h.next = kNone;
    h.group = kNone;
    h.first_member = kNone;
    h.next_member = kNone;
}

bool Aram::SetGroup(uint32_t heap, uint32_t owner)
{
    Heap& h = heaps_[heap];
    if (h.group != kNone || h.next_member != kNone)
    {
        return false;
    }

    h.group = owner;
    h.next_member = heaps_[owner].first_member;
    heaps_[owner].first_member = heap;

    return true;
}

bool Aram::Delete(uint32_t heap)
{
    if (heaps_[heap].start == 0)
    {
        return false;
    }

    for (uint32_t child = heaps_[heap].first_child; child != kNone;)
    {
        const uint32_t next = heaps_[child].next;
        Delete(child);
        child = next;
    }
    heaps_[heap].first_child = kNone;

    for (uint32_t member = heaps_[heap].first_member; member != kNone;)
    {
        const uint32_t next = heaps_[member].next_member;
        Delete(member);
        member = next;
    }
    heaps_[heap].first_member = kNone;

    Heap& h = heaps_[heap];
    if (h.parent != kNone)
    {
        Heap& p = heaps_[h.parent];
        if (p.first_child == heap)
        {
            p.first_child = h.next;
            if (h.next == kNone)
            {
                p.used = 0;
            }
        }
        else
        {
            uint32_t before = p.first_child;
            for (;;)
            {
                if (before == kNone)
                {
                    // The parent lacks the heap.
                    h.start = 0;
                    return false;
                }
                if (heaps_[before].next == heap)
                {
                    break;
                }

                before = heaps_[before].next;
            }

            heaps_[before].next = h.next;
            if (h.next == kNone)
            {
                p.used = heaps_[before].start + heaps_[before].size - p.start;
            }
        }
        p.children--;
    }

    if (h.group != kNone)
    {
        Heap& owner = heaps_[h.group];
        if (owner.first_member == heap)
        {
            owner.first_member = h.next_member;
        }
        else
        {
            uint32_t before = owner.first_member;
            for (;;)
            {
                if (before == kNone)
                {
                    // The group's owner lacks the heap. The heap keeps its start.
                    return false;
                }
                if (heaps_[before].next_member == heap)
                {
                    break;
                }

                before = heaps_[before].next_member;
            }
            heaps_[before].next_member = h.next_member;
        }

        h.group = kNone;
        h.next_member = kNone;
    }

    h.start = 0;

    return true;
}

Banks::Banks(std::vector<ntdlib::jaudio::Bank> banks, std::vector<ntdlib::jaudio::WaveSystem> systems,
             const WaveArchives& archives, DspMemory& memory, uint32_t aram_base, uint32_t aram_size)
    : banks_(std::move(banks)),
      systems_(std::move(systems)),
      bank_places_(kPlaces, kNone),
      virtual_banks_(kPlaces, 0xffff),
      archives_(archives),
      memory_(memory),
      aram_base_(aram_base),
      aram_(aram_size)
{
    // The wave systems first (Bank_Setup). Each system's records start with no status. Each archive and each handle has
    // a heap that holds nothing, and each handle has no record: the file holds 0xffffffff there.
    uint32_t places = 0;
    for (const ntdlib::jaudio::WaveSystem& system : systems_)
    {
        if (system.number < kPlaces)
        {
            places = std::max(places, system.number + 1);
        }
    }
    states_.resize(places);

    for (const ntdlib::jaudio::WaveSystem& system : systems_)
    {
        if (system.number >= kPlaces || states_[system.number].system != nullptr)
        {
            continue;
        }

        const uint32_t place = system.number;
        WaveSystemState& state = states_[place];
        state.system = &system;

        for (std::size_t g = 0; g < system.groups.size(); g++)
        {
            const ntdlib::jaudio::WaveGroup& group = system.groups[g];
            WaveArchiveState archive;
            archive.name = group.archive;
            archive.heap = aram_.NewHeap();
            for (const ntdlib::jaudio::WaveInfo& wave : group.archive_waves)
            {
                WaveRecord record;
                record.info = WaveInfoOf(wave);
                record.offset = wave.offset;
                record.size = wave.size;
                record.system = place;
                record.archive = static_cast<uint32_t>(g);
                archive.records.push_back(record);
            }
            state.archives.push_back(std::move(archive));

            WaveSceneState scene;
            scene.external_mode = group.scene.external_mode;
            scene.dependencies = group.scene.dependencies;
            const auto handles = [&](const std::optional<std::vector<uint32_t>>& words, std::vector<WaveHandle>& list)
            {
                if (!words)
                {
                    return;
                }

                for (const uint32_t word : *words)
                {
                    WaveHandle handle;
                    handle.id = word;
                    handle.system = place;
                    handle.heap = aram_.NewHeap();
                    list.push_back(handle);
                }
            };
            handles(group.scene.cdf, scene.cdf);
            handles(group.scene.cex, scene.cex);
            state.scenes.push_back(std::move(scene));
        }
    }

    // Then the banks. The oscillators' tables go into their vectors before any setting points at them.
    points_.resize(banks_.size());
    oscillators_.resize(banks_.size());
    for (std::size_t i = 0; i < banks_.size(); i++)
    {
        const ntdlib::jaudio::Bank& bank = banks_[i];
        if (bank.number < kPlaces && bank_places_[bank.number] == kNone)
        {
            bank_places_[bank.number] = static_cast<uint32_t>(i);
            virtual_banks_[bank.number] = static_cast<uint16_t>(bank.number);
        }

        for (const ntdlib::jaudio::Oscillator& osc : bank.oscillators)
        {
            points_[i].push_back(PointsOf(osc.attack));
            points_[i].push_back(PointsOf(osc.release));
        }
        for (std::size_t k = 0; k < bank.oscillators.size(); k++)
        {
            const ntdlib::jaudio::Oscillator& osc = bank.oscillators[k];
            OscData data;
            data.target = osc.target;
            data.rate = osc.rate;
            data.attack = TableOf(points_[i][2 * k]);
            data.release = osc.release_is_attack ? data.attack : TableOf(points_[i][2 * k + 1]);
            data.width = osc.scale;
            data.vertex = osc.vertex;
            oscillators_[i].push_back(data);
        }
    }
}

uint16_t Banks::PhysicalBank(uint32_t number) const
{
    return number < kPlaces ? virtual_banks_[number] : 0xffff;
}

const ntdlib::jaudio::Bank* Banks::BankAt(uint32_t bank) const
{
    return bank < kPlaces && bank_places_[bank] != kNone ? &banks_[bank_places_[bank]] : nullptr;
}

const ntdlib::jaudio::Instrument* Banks::FindInstrument(uint32_t bank, uint32_t program) const
{
    const ntdlib::jaudio::Bank* b = BankAt(bank);
    if (b == nullptr)
    {
        return nullptr;
    }

    const auto found = b->programs.find(program);

    return found != b->programs.end() ? std::get_if<ntdlib::jaudio::Instrument>(&found->second) : nullptr;
}

const ntdlib::jaudio::DrumSet* Banks::FindDrums(uint32_t bank, uint32_t program) const
{
    const ntdlib::jaudio::Bank* b = BankAt(bank);
    if (b == nullptr)
    {
        return nullptr;
    }

    const auto found = b->programs.find(program);

    return found != b->programs.end() ? std::get_if<ntdlib::jaudio::DrumSet>(&found->second) : nullptr;
}

uint8_t Banks::InstrumentFlags(uint32_t bank, uint32_t program) const
{
    const ntdlib::jaudio::Instrument* instrument = FindInstrument(bank, program);

    return instrument != nullptr ? instrument->flags : 0;
}

const OscData* Banks::Oscillator(uint32_t bank, std::size_t index) const
{
    if (bank >= kPlaces || bank_places_[bank] == kNone)
    {
        return nullptr;
    }

    const std::vector<OscData>& list = oscillators_[bank_places_[bank]];

    return index < list.size() ? &list[index] : nullptr;
}

uint32_t Banks::BankSystem(uint32_t bank) const
{
    const ntdlib::jaudio::Bank* b = BankAt(bank);

    return b != nullptr && HasSystem(b->wave_system) ? b->wave_system : kNone;
}

bool Banks::HasSystem(uint32_t system) const
{
    return system < states_.size() && states_[system].system != nullptr;
}

bool Banks::SetScene(uint32_t system, uint32_t scene, bool set)
{
    // WaveScene_Set (0x2735c0 in main) and WaveScene_Load (0x273610).
    if (!HasSystem(system) || scene >= states_[system].scenes.size() || scene >= states_[system].archives.size())
    {
        return false;
    }

    return SceneSet(states_[system], scene, set);
}

void Banks::CloseScene(uint32_t system, uint32_t scene)
{
    // WaveScene_Erase (0x273660 in main).
    if (!HasSystem(system) || scene >= states_[system].scenes.size())
    {
        return;
    }

    SceneClose(states_[system], scene, 0);
}

bool Banks::SceneSet(WaveSystemState& system, uint32_t scene, bool set)
{
    // Jac_SceneSet (0x272940 in main). An archive that holds its heap and whose load has finished points its records at
    // its status. Otherwise the archive's load starts again from a status of 0.
    WaveArchiveState& archive = system.archives[scene];
    const bool loaded = aram_.At(archive.heap).start != 0 && archive.status != 0;
    if (!loaded)
    {
        archive.status = 0;
    }
    for (WaveRecord& record : archive.records)
    {
        record.status = WaveRecord::kArchiveStatus;
    }
    if (!loaded && !LoadAram(archive.name, 0, 0, archive.status, archive.heap))
    {
        return false;
    }

    // C-DF names the archive's first records in order, and C-EX with an external mode of 0 names the next ones.
    WaveSceneState& s = system.scenes[scene];
    const auto index = static_cast<uint32_t>(&archive - system.archives.data());
    for (std::size_t i = 0; i < s.cdf.size(); i++)
    {
        UpdateWave(system, s.cdf[i], index, static_cast<uint32_t>(i));
    }

    const auto base = static_cast<uint32_t>(s.cdf.size());
    if (s.external_mode == 0)
    {
        for (std::size_t j = 0; j < s.cex.size(); j++)
        {
            UpdateWave(system, s.cex[j], index, base + static_cast<uint32_t>(j));
        }
    }
    else
    {
        for (std::size_t j = 0; j < s.cex.size(); j++)
        {
            UpdateWaveExtern(system, scene, static_cast<uint32_t>(j));
        }
    }

    if (set)
    {
        system.current_scene = scene;
    }

    return true;
}

void Banks::UpdateWave(WaveSystemState& system, WaveHandle& handle, uint32_t archive, uint32_t record)
{
    // The game reads past the archive's records for a list that names more waves than the archive holds. No scene of
    // the Switch's file does.
    const WaveArchiveState& a = system.archives[archive];
    if (record >= a.records.size())
    {
        return;
    }

    handle.archive = archive;
    handle.record = record;
    const uint32_t start = aram_.At(a.heap).start;
    if (start != 0)
    {
        const WaveRecord& r = a.records[record];
        aram_.SelfAlloc(handle.heap, a.heap, Align32(r.size), start + r.offset);
    }
}

void Banks::UpdateWaveExtern(WaveSystemState& system, uint32_t scene, uint32_t index)
{
    // UpdateWave_Extern, in Jac_SceneSet. A handle whose heap holds space keeps it. Otherwise the word's high half
    // names another scene. The handle takes the wave at the place where that scene's C-DF first holds the same id.
    WaveHandle& handle = system.scenes[scene].cex[index];
    if (aram_.At(handle.heap).start != 0)
    {
        return;
    }

    const uint32_t other = handle.id >> 16;
    if (other >= system.scenes.size() || other >= system.archives.size())
    {
        return;
    }

    const std::vector<WaveHandle>& list = system.scenes[other].cdf;
    const WaveHandle* found = FirstHandle(list, static_cast<uint16_t>(handle.id));
    if (found == nullptr)
    {
        return;
    }

    const auto k = static_cast<uint32_t>(found - list.data());
    WaveArchiveState& archive = system.archives[other];
    if (aram_.At(found->heap).start == 0)
    {
        // A load of the wave's samples alone, into a heap from the mother heap. The record's status pointer moves to
        // the handle's status.
        if (k >= archive.records.size())
        {
            return;
        }

        WaveRecord& record = archive.records[k];
        handle.load_status = 0;
        handle.archive = other;
        handle.record = k;
        record.status = scene << 16 | index;
        LoadAram(archive.name, record.offset, record.size, handle.load_status, handle.heap);
        aram_.SetGroup(handle.heap, archive.heap);
    }
    else
    {
        // The other handle's samples, with a heap outside the tree in that handle's group.
        handle.archive = found->archive;
        handle.record = found->record;
        aram_.SelfInit(handle.heap, aram_.At(found->heap).start, 0);
        aram_.SetGroup(handle.heap, found->heap);
    }
}

bool Banks::LoadAram(const std::string& name, uint32_t offset, uint32_t size, uint32_t& status, uint32_t heap)
{
    // LoadAram_Default (0x272170 in main). A load with no offset and no size takes the whole file.
    const std::vector<uint8_t>* file = archives_(name);
    const auto file_size = file != nullptr ? static_cast<uint32_t>(file->size()) : 0u;
    const uint32_t length = (offset | size) != 0 ? size : file_size;
    if (!aram_.Alloc(heap, length))
    {
        return false;
    }

    if (file == nullptr)
    {
        // The DVD thread's error.
        status = 0xffffffff;
        return true;
    }

    const uint32_t bytes = size != 0 ? size : (offset < file_size ? file_size - offset : 0u);
    const uint64_t destination = uint64_t{aram_base_} + aram_.At(heap).start;
    const uint64_t available = offset < file_size ? file_size - offset : 0u;
    const uint64_t room = destination < memory_.Size() ? memory_.Size() - destination : 0u;
    const uint64_t copied = std::min({uint64_t{bytes}, available, room});
    std::copy_n(file->begin() + static_cast<std::ptrdiff_t>(offset), copied,
                memory_.Bytes().begin() + static_cast<std::ptrdiff_t>(destination));
    status = Align32(bytes);

    return true;
}

void Banks::SceneClose(WaveSystemState& system, uint32_t scene, uint32_t depth)
{
    // Jac_SceneClose (0x2728b0 in main) with its recursion: the archive's heap and the heaps in it, then the scenes
    // that the scene depends on. The handles keep their records, and the records their status pointers.
    if (scene >= system.archives.size() || scene >= system.scenes.size() || depth > kMaxDepth)
    {
        return;
    }

    WaveArchiveState& archive = system.archives[scene];
    aram_.Delete(archive.heap);
    archive.status = 0;

    for (const uint32_t dependency : system.scenes[scene].dependencies)
    {
        SceneClose(system, dependency, depth + 1);
    }
}

const WaveHandle* Banks::FindHandle(const WaveSystemState& system, uint32_t scene, uint16_t id, uint32_t depth) const
{
    // __GetSoundHandle (0x272c20 in main): the first handle of the id in C-DF, then in C-EX, if it has a record. Then
    // the scenes that the scene depends on, in turn.
    if (scene >= system.scenes.size() || depth > kMaxDepth)
    {
        return nullptr;
    }

    const WaveSceneState& s = system.scenes[scene];
    for (const std::vector<WaveHandle>* list : {&s.cdf, &s.cex})
    {
        const WaveHandle* handle = FirstHandle(*list, id);
        if (handle != nullptr && handle->record != WaveHandle::kNone)
        {
            return handle;
        }
    }

    for (const uint32_t dependency : s.dependencies)
    {
        const WaveHandle* handle = FindHandle(system, dependency, id, depth + 1);
        if (handle != nullptr && handle->record != WaveHandle::kNone)
        {
            return handle;
        }
    }

    return nullptr;
}

const WaveHandle* Banks::GetSoundHandle(const WaveSystemState& system, uint16_t id) const
{
    // GetSoundHandle (0x272d50 in main): the handle in the current scene, if its record points at a status and the
    // load has finished.
    const WaveHandle* handle = FindHandle(system, system.current_scene, id, 0);
    if (handle == nullptr)
    {
        return nullptr;
    }

    const WaveRecord* record = Record(*handle);
    if (record == nullptr || record->status == WaveRecord::kNoStatus || RecordStatus(*record) == 0)
    {
        return nullptr;
    }

    return handle;
}

const WaveHandle* Banks::FindSound(uint32_t system, uint16_t id) const
{
    return HasSystem(system) ? GetSoundHandle(states_[system], id) : nullptr;
}

const WaveRecord* Banks::Record(const WaveHandle& handle) const
{
    if (handle.record == WaveHandle::kNone || !HasSystem(handle.system))
    {
        return nullptr;
    }

    const std::vector<WaveArchiveState>& archives = states_[handle.system].archives;
    if (handle.archive >= archives.size() || handle.record >= archives[handle.archive].records.size())
    {
        return nullptr;
    }

    return &archives[handle.archive].records[handle.record];
}

uint32_t Banks::HandleStart(const WaveHandle& handle) const
{
    return aram_.At(handle.heap).start;
}

uint32_t Banks::RecordStatus(const WaveRecord& record) const
{
    const WaveSystemState& system = states_[record.system];
    if (record.status == WaveRecord::kNoStatus)
    {
        return 0;
    }
    if (record.status == WaveRecord::kArchiveStatus)
    {
        return system.archives[record.archive].status;
    }

    const uint32_t scene = record.status >> 16;
    const uint32_t index = record.status & 0xffff;

    return system.scenes[scene].cex[index].load_status;
}

} // namespace hsf::jac
