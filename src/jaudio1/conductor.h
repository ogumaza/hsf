// SPDX-License-Identifier: MIT

// The conductor of Pikmin 2's cave music (PSAutoBgm). The child tracks of a cave's piece request their phrases from
// the game's code. A file of the conductor archive (a .cnd file) holds the settings of the answers. It gives each child
// track a list of modules. A module sets the track's volume, pan and transpose. It also names two lists in the
// sequence's data: values for ports 6 and 7 (the game's tips) and values for port 8 (the game's wave scenes). A module
// plays in two kinds of cycle, an on cycle and an off cycle, each with a number of repeats. The on cycle draws a tip at
// random. Either cycle draws a wave scene at random, or takes the scenes in their order. After a number of counted
// beats, the track moves on to its next module. The game draws from one generator for all its sound code
// (PSSystem::oRandom). The generator's state as a piece starts depends on what the game played before. The model
// starts it from its value at the game's start.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "jaudio1/calc.h"
#include "jaudio1/track.h"

namespace hsf::jaudio1
{

// A conductor file: JADUtility's serialization of the conductor's tree. Each object holds bytes before its values,
// its values in their list's order, bytes after them, then its children. The values of 16 bits are little-endian.
// The fields that no request reads are left out.
struct ConductorFile
{
    struct Module
    {
        uint16_t start = 0;      // the wait that the module's start answers, in units of 120 ticks
        int16_t beats = 64;      // the counted beats after which the track moves on, unless this is its last module
        uint8_t in_order = 0;    // 1: the cycles take the wave scenes in their order, once each
        uint8_t port_high = 0;   // the high byte of the value for port 3
        uint8_t port_low = 0;    // the low byte of the value for port 3
        uint16_t tip_list = 0;   // the module's list of tips, by its number in the sequence's table of them
        uint16_t scene_list = 0; // the module's list of wave scenes, by its number in the sequence's table of them
        std::array<uint8_t, 2> repeats{}; // the repeats of the on cycle and of the off cycle
        int16_t transpose = 0;            // the low byte becomes the track's transpose
        uint8_t volume = 96;              // over 127
        uint8_t pan = 64;                 // over 127
        uint8_t half_start = 0;           // 1 adds 60 ticks to the start's wait

        bool operator==(const Module&) const = default;
    };

    struct Track
    {
        uint16_t start = 0;     // the wait that the track's start answers, in units of 120 ticks
        uint8_t half_start = 0; // 1 adds 60 ticks to the start's wait
        std::string name;
        uint8_t unison = 16; // the track whose choices this one copies, or 16 for none
        uint8_t fxmix = 0;   // the child track's effect send, over 127
        std::vector<Module> modules;

        bool operator==(const Track&) const = default;
    };

    uint8_t beat_interval = 60; // the ticks between the root track's requests for a beat
    uint8_t tempo = 100;        // the tempo that the conductor gives the track that starts it
    std::vector<Track> tracks;  // by the child tracks' slots

    // Throws xsflib::FormatError for a file that can't be read.
    static ConductorFile Parse(std::span<const uint8_t> bytes);

    bool operator==(const ConductorFile&) const = default;
};

// The conductor's objects as a piece plays (PSAutoBgm::Conductor, Track and Module, with each module's two cycles).
class Conductor
{
public:
    explicit Conductor(ConductorFile file);

    // The conductor's answer to a request of track `index` (PSGame's seqCpuSync for an AutoBgm piece), or nullopt for a
    // request that the conductor doesn't take. A module's requests find the module through the track's registers 20
    // and 21, where the module's start puts its handle (PSSystem::setObject).
    std::optional<uint16_t> Sync(Tracks& tracks, uint32_t index, uint32_t value);

    // The count at every other beat of the root track (the conductor's beatUpdate): each track's module counts a beat.
    void CountBeat();

    uint16_t BeatInterval() const
    {
        return file_.beat_interval;
    }

    // The effect send that the piece's start gives the child track in slot `number` (AutoBgm::startSeq), or nullopt for
    // a slot that the conductor has no track for.
    std::optional<float> Fxmix(uint32_t number) const;

    // The requests on which the game's code fails: a module's request from a track without a module, and a copy from
    // a track that hasn't started a module.
    uint32_t Unmodelled() const
    {
        return unmodelled_;
    }

private:
    static constexpr uint8_t kNoModule = 0xff;
    static constexpr uint8_t kNoUnison = 16;
    static constexpr std::size_t kHistory = 2;

    struct ModuleState
    {
        uint16_t beats = 0;
        uint8_t cycle = 0; // 0 for the on cycle, 1 for the off cycle
        std::array<uint8_t, 2> repeat{};
        std::array<uint8_t, 2> scene{}; // the next wave scene of each cycle
        uint16_t scene_value = 0xffff;  // the last wave scene's value for port 8
        uint16_t tip = 0xffff;          // the last tip's number
        bool tables = false;
        uint32_t tips = 0; // the list of tips: its offset in the sequence's data, and its count
        uint8_t tip_count = 0;
        uint32_t scenes = 0;
        uint8_t scene_count = 0;
        std::vector<uint16_t> history; // the on cycle's last tips drawn at random, oldest first
    };

    struct TrackState
    {
        uint8_t current = kNoModule;
        uint8_t unison = kNoUnison;
        std::vector<ModuleState> modules;
    };

    struct ModuleRef
    {
        uint32_t track = 0;
        uint32_t module = 0;
    };

    uint16_t StartConductor(Tracks& tracks, uint32_t index);
    uint16_t StartTrack(uint32_t number);
    uint16_t StartModule(Tracks& tracks, uint32_t index, uint32_t number);
    uint16_t CycleLoop(const ModuleRef& ref);
    uint16_t Play(Tracks& tracks, uint32_t index, const ModuleRef& ref);
    uint16_t PlayScene(Tracks& tracks, uint32_t index, const ModuleRef& ref);
    uint16_t CheckCloser(const ModuleRef& ref) const;
    void SetTip(Tracks& tracks, uint32_t index, const ModuleRef& ref);
    void SetTables(const Sequence* sequence, const ModuleRef& ref);
    std::optional<ModuleRef> FindModule(const Tracks& tracks, uint32_t index) const;

    // The module that the track's unison track plays, or null. A copy from a unison track that hasn't started a module
    // counts as unmodelled.
    const ModuleState* UnisonModule(uint32_t track);

    ConductorFile file_;
    std::vector<TrackState> tracks_;
    uint32_t tip_table_ = 0; // the sequence's tables of the lists of tips and of wave scenes: offsets in its data
    uint32_t scene_table_ = 0;
    RandomFast random_;
    uint32_t unmodelled_ = 0;
};

} // namespace hsf::jaudio1
