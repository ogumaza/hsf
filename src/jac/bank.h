// SPDX-License-Identifier: MIT

// The bank file of the C JAudio as the Switch release of Pikmin builds it (Banks/pikibank.bx): its instrument banks,
// its wave systems with their scenes, and ARAM, where the game loads the scenes' wave archives. ntdlib reads the file
// (ntdlib/jaudio/bx.h). A sequence names a bank by its virtual number. The game maps the virtual numbers of the banks
// and of the wave systems to their places in the file (Jac_BnkVirtualToPhysical and Jac_WsVirtualToPhysical, from the
// tables that Bank_Setup fills through Jac_BnkConnectTableSet and Jac_WsConnectTableSet).
//
// A scene of a wave system holds handles of waves (WaveID_) in up to three lists. WaveScene_Set and WaveScene_Load
// (Jac_SceneSet) load the whole of the scene's wave archive into ARAM and give the scene's handles their waves'
// records and heaps. WaveScene_Set also makes the scene its system's current scene. A note's wave plays if the
// system's current scene, or a scene that it depends on, has a handle of the wave, and the load of the wave's samples
// has finished (GetSoundHandle). Otherwise the note takes no channel. WaveScene_Erase and WaveScene_Close
// (Jac_SceneClose) free a scene's archive again.
//
// ntdlib gives a wave system its place in the file as its number, and a bank its header's number and the place of
// the system whose waves it plays (ntdlib/jaudio/bx.h). The model places each wave system by its place and each bank
// by its number. Every bank of the Switch's file has its place as its number, and so does every wave system but those
// at places 14 and 15. Their headers give the number 0xffff. The game then maps no virtual number to them. No wave of
// the file names them by number.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ntdlib/jaudio/bank.h"
#include "ntdlib/jaudio/wave_system.h"

#include "jac/oscillator.h"
#include "jaudio/channel.h"
#include "jaudio/dsp.h"

namespace hsf::jac
{

using jaudio::DspMemory;

// ARAM's heaps (jaheap_): the mother heap (aram_mother), and the heaps that the wave systems' archives and handles
// hold. The first load into ARAM sets up the mother heap. The model refers to a heap by its index. Jac_AllocHeap gives
// a heap the space at the end of what the mother heap has given, rounded up to 32 bytes. A full mother heap gives the
// tightest of the gaps before its children instead. Jac_SelfAllocHeap places a heap at a given address inside a parent
// heap. Jac_SetGroupHeap makes a heap a member of another heap's group: Jac_DeleteHeap deletes a heap's children, then
// the members of its group.
class Aram
{
public:
    static constexpr uint32_t kNone = 0xffffffff;

    // The mother heap's index.
    static constexpr uint32_t kMother = 0;

    // Where the Switch's build starts the mother heap (Jac_InitARAM keeps the ARAM below it for the system).
    static constexpr uint32_t kStart = 0xc000;

    // A heap's fields. A heap whose start is 0 holds nothing. The game's start is 64 bits wide. ARAM's addresses fit in
    // 32.
    struct Heap
    {
        uint32_t start = 0;
        uint32_t used = 0; // the bytes from the start to the end of the last child
        uint32_t size = 0;
        uint16_t children = 0;
        uint32_t parent = kNone;
        uint32_t first_child = kNone;
        uint32_t next = kNone; // the next child of the parent, in the order of the children's starts
        uint32_t group = kNone;
        uint32_t first_member = kNone;
        uint32_t next_member = kNone;
    };

    // The mother heap for an ARAM of `aram_size` bytes (Jac_SetAudioARAMSize): from kStart to 32 bytes before ARAM's
    // end (Jac_InitARAM, then the first load's call of Jac_InitMotherHeap).
    explicit Aram(uint32_t aram_size);

    // Jac_InitHeap: a new heap that holds nothing. Returns its index.
    uint32_t NewHeap();

    // Jac_AllocHeap: `size` bytes, rounded up to 32, from the mother heap. False if the heap already holds space or
    // the mother heap has no room.
    bool Alloc(uint32_t heap, uint32_t size);

    // Jac_SelfAllocHeap: `size` bytes at `start` as a child of `parent`. False if the heap already holds space.
    bool SelfAlloc(uint32_t heap, uint32_t parent, uint32_t size, uint32_t start);

    // Jac_SelfInitHeap: the heap at `start`, outside the tree.
    void SelfInit(uint32_t heap, uint32_t start, uint32_t size);

    // Jac_SetGroupHeap: the heap joins the group of `owner`. False if it's in a group already.
    bool SetGroup(uint32_t heap, uint32_t owner);

    // Jac_DeleteHeap: the heap's children and its group's members, then the heap itself. False if it held nothing, or
    // if its parent or its group's owner lacks it.
    bool Delete(uint32_t heap);

    const Heap& At(uint32_t heap) const
    {
        return heaps_[heap];
    }

private:
    std::vector<Heap> heaps_;
};

// A wave's record in its archive (Wave_ in WINF): the wave's settings, where its samples lie in the archive's file,
// and the status of the load that the record points at. The game keeps a pointer to a status in each record:
// WaveScene_Set points the records at their archive's status, and UpdateWave_Extern points a record at the status of a
// handle's load of the record's samples. A channel points at a record. The game's channel does the same.
struct WaveRecord
{
    // What the record's status pointer points at: nothing (the file's null), its archive's status, or the status of
    // handle `index` of scene `scene`'s C-EX: (scene << 16) | index.
    static constexpr uint32_t kNoStatus = 0xffffffff;
    static constexpr uint32_t kArchiveStatus = 0xfffffffe;

    jaudio::WaveInfo info;
    uint32_t offset = 0; // in the archive's file
    uint32_t size = 0;
    uint32_t system = 0;  // the place of the record's wave system
    uint32_t archive = 0; // and its archive
    uint32_t status = kNoStatus;
};

// A wave's handle in a scene's list (WaveID_): the word of the list, its heap, the status of the handle's load of its
// wave, and its record. A handle with no record holds kNone there: the file starts every handle so.
struct WaveHandle
{
    static constexpr uint32_t kNone = 0xffffffff;

    uint32_t id = 0;     // the list's word: the wave's id in the low half
    uint32_t system = 0; // the place of the handle's wave system
    uint32_t heap = 0;   // in Aram
    uint32_t load_status = 0;
    uint32_t archive = kNone; // the record: its archive, by the archive's group in the system
    uint32_t record = kNone;  // and its place in the archive's records
};

// A scene of a wave system (SCNE) and the handles of its lists C-DF and C-EX. The steps of the game that the model
// follows don't read its third list, C-ST. A list that the scene lacks is empty. The game skips an empty list as it
// skips a missing one.
struct WaveSceneState
{
    uint32_t external_mode = 0;
    std::vector<uint32_t> dependencies;
    std::vector<WaveHandle> cdf;
    std::vector<WaveHandle> cex;
};

// A wave archive of a wave system (WaveArchive_): its name, its heap in ARAM, the status of its load and its records.
// The game's status is 0 while the load runs. The model's loads finish at once. The game's DVD thread leaves the bytes
// that a load read, rounded up to 32, in the status. For an archive that the set lacks, it leaves 0xffffffff. The
// model does the same.
struct WaveArchiveState
{
    std::string name;
    uint32_t heap = 0; // in Aram
    uint32_t status = 0;
    std::vector<WaveRecord> records;
};

// A wave system (WSYS) with its scenes (WBCT): the current scene, each scene's handles and each archive's state. A
// place of the bank file without a system holds no system.
struct WaveSystemState
{
    const ntdlib::jaudio::WaveSystem* system = nullptr;
    uint32_t current_scene = 0;
    std::vector<WaveSceneState> scenes;     // by the place of the scene, the place of its group
    std::vector<WaveArchiveState> archives; // by group
};

// The banks and the wave systems of the bank file, the virtual numbers' tables, and the waves in ARAM. ARAM's address 0
// is at `aram_base` in the DSP's memory. The DSP reads the samples there (DsetVARAM).
class Banks
{
public:
    static constexpr uint32_t kNone = 0xffffffff;

    // A wave archive's bytes by its name in the wave systems, or null for an archive that the set lacks.
    using WaveArchives = std::function<const std::vector<uint8_t>*(const std::string& name)>;

    // `banks` and `systems` as ntdlib's ReadBxBanks and ReadBxWaveSystems give them. Bank_Setup registers each wave
    // system, then each bank, at its place. Each bank's number then maps to its place (Jac_BnkConnectTableSet). The
    // first bank or system to give a number keeps it. ntdlib maps the numbers of the wave systems
    // (Jac_WsConnectTableSet). `aram_size` is the size that the game gives Jac_SetAudioARAMSize.
    Banks(std::vector<ntdlib::jaudio::Bank> banks, std::vector<ntdlib::jaudio::WaveSystem> systems,
          const WaveArchives& archives, DspMemory& memory, uint32_t aram_base, uint32_t aram_size);

    Banks(const Banks&) = delete;
    Banks& operator=(const Banks&) = delete;

    // Jac_BnkVirtualToPhysical: a bank's place in the file, or 0xffff for a number that no bank gives.
    uint16_t PhysicalBank(uint32_t number) const;

    // InstChange and PercChange: program `program` of the bank at place `bank`, or null if the bank lacks it.
    // Programs from 0xe4 to 0xef are drum sets.
    const ntdlib::jaudio::Instrument* FindInstrument(uint32_t bank, uint32_t program) const;
    const ntdlib::jaudio::DrumSet* FindDrums(uint32_t bank, uint32_t program) const;

    // An instrument's flags (Inst_'s word at +4): the class of its voices' limit in the low 4 bits (polys_table), and
    // what a note does past the limit in bits 4 to 7.
    uint8_t InstrumentFlags(uint32_t bank, uint32_t program) const;

    // The settings of oscillator `index` of the bank at place `bank`, by its index in ntdlib's bank. A channel points
    // at them. The game's channel points at the bank's data in the same way.
    const OscData* Oscillator(uint32_t bank, std::size_t index) const;

    // The place of the wave system whose waves the bank at place `bank` plays (WaveidToWavegroup), or kNone.
    uint32_t BankSystem(uint32_t bank) const;

    // Whether the place `system` holds a wave system (its WBCT in the game).
    bool HasSystem(uint32_t system) const;

    // WaveScene_Set (`set`) and WaveScene_Load: Jac_SceneSet for scene `scene` of the system at place `system`. Loads
    // the scene's archive into ARAM if it isn't there, and gives the scene's handles their records and heaps. A set
    // makes the scene the system's current scene. Returns false for a scene that the file lacks, or if ARAM has no
    // room for the archive.
    bool SetScene(uint32_t system, uint32_t scene, bool set);

    // WaveScene_Erase: Jac_SceneClose for scene `scene` of the system at place `system`, and for the scenes that the
    // scene depends on. Frees each scene's archive in ARAM. Nothing in the model closes a scene alone
    // (WaveScene_Close).
    void CloseScene(uint32_t system, uint32_t scene);

    // GetSoundHandle for the wave `id` of the system at place `system`: its handle in the current scenes, or null. The
    // handle stays in place.
    const WaveHandle* FindSound(uint32_t system, uint16_t id) const;

    // A handle's record, or null, and the start of its heap.
    const WaveRecord* Record(const WaveHandle& handle) const;
    uint32_t HandleStart(const WaveHandle& handle) const;

    // The status that a record points at, or 0 for none.
    uint32_t RecordStatus(const WaveRecord& record) const;

    // The wave systems by their places.
    const std::vector<WaveSystemState>& Systems() const
    {
        return states_;
    }

private:
    const ntdlib::jaudio::Bank* BankAt(uint32_t bank) const;

    // Jac_SceneSet and Jac_SceneClose on a system. SceneClose also closes the scenes that the scene depends on.
    bool SceneSet(WaveSystemState& system, uint32_t scene, bool set);
    void SceneClose(WaveSystemState& system, uint32_t scene, uint32_t depth);

    // UpdateWave and UpdateWave_Extern for one handle.
    void UpdateWave(WaveSystemState& system, WaveHandle& handle, uint32_t archive, uint32_t record);
    void UpdateWaveExtern(WaveSystemState& system, uint32_t scene, uint32_t index);

    // LoadAram_Default: the bytes from `offset` (`size` of them, or the whole file for 0 and 0) of the archive `name`
    // into a heap from the mother heap. Leaves the load's status in `status`. False if the mother heap gives no space.
    bool LoadAram(const std::string& name, uint32_t offset, uint32_t size, uint32_t& status, uint32_t heap);

    // __GetSoundHandle and GetSoundHandle.
    const WaveHandle* FindHandle(const WaveSystemState& system, uint32_t scene, uint16_t id, uint32_t depth) const;
    const WaveHandle* GetSoundHandle(const WaveSystemState& system, uint16_t id) const;

    std::vector<ntdlib::jaudio::Bank> banks_;
    std::vector<ntdlib::jaudio::WaveSystem> systems_;
    std::vector<uint32_t> bank_places_; // the index in banks_ of each place, or kNone
    std::vector<WaveSystemState> states_;
    std::vector<std::vector<std::vector<OscPoint>>> points_; // by bank, then by oscillator: attack, then release
    std::vector<std::vector<OscData>> oscillators_;          // by bank, then by ntdlib's index
    std::vector<uint16_t> virtual_banks_;
    WaveArchives archives_;
    DspMemory& memory_;
    uint32_t aram_base_;
    Aram aram_;
};

} // namespace hsf::jac
