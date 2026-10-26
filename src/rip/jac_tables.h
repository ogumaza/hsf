// SPDX-License-Identifier: MIT

// Tables in the main of Pikmin's Switch release that set how its C JAudio plays the cutscenes' sounds and the game's
// effects: the cutscenes' table, the actions of the events, and the game's table of effects. Each address counts from
// the module's start, in the build of main that hsfrip rips. The caller checks main's build ID first. The loader
// writes each pointer in the tables, for a relative relocation.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/nx/nso.h"

#include "hsf/format.h"

namespace hsf::rip
{

// The cutscenes' table (DEMO_STATUS): a row of 16 bytes for each cutscene, in the order of their numbers. Jac_StartDemo
// (0x261560) and Jac_DemoFrame (0x261b00) read it. The cutscenes' events follow the table.
inline constexpr uint64_t kCutscenesAt = 0x6ae690;
inline constexpr std::size_t kCutscenes = 115;
inline constexpr std::size_t kCutsceneRow = 16;

// The number of the first action of each type of event (EVENT_OFFSET): a 32-bit number for each of the 8 types.
// Jac_PlayEventAction (0x2628e0) reads it.
inline constexpr uint64_t kFirstActionsAt = 0x5b74f8;
inline constexpr std::size_t kEventTypes = 8;

// The actions of the events (ACTION_STATUS): 6 bytes for each action. Jac_PlayEventAction (0x2628e0) reads them. The
// game doesn't store their number. The GameCube's release holds the same 278 actions.
inline constexpr uint64_t kActionsAt = 0x5be260;
inline constexpr std::size_t kActions = 278;
inline constexpr std::size_t kActionRow = 6;

// The game's table of effects: 0x18 bytes for each effect. SeSystem's functions that play and stop an effect (0x7c694
// to 0x7c6dc) read it, and so do SeContext::playSound (0x7b714) and stopSound (0x7b85c). Those functions read only the
// number at +4 of a row. The game doesn't store the number of rows. The number at +0 of each row is the row's place in
// the table, from 0 to 331.
inline constexpr uint64_t kEffectsAt = 0x68df28;
inline constexpr std::size_t kEffects = 332;
inline constexpr std::size_t kEffectRow = 0x18;

// The `size` bytes of `main` at `address`. Throws xsflib::FormatError, with a message that names `what`, unless
// they're all in one of main's segments.
std::span<const uint8_t> MainBytes(const ntdlib::nx::Nso& main, uint64_t address, std::size_t size,
                                   std::string_view what);

// The NUL-terminated string at `address` in `main`, without its NUL. Throws xsflib::FormatError, with a message that
// names `what`, unless the string and its NUL are all in one of main's segments.
std::string MainString(const ntdlib::nx::Nso& main, uint64_t address, std::string_view what);

// The pointers that the loader writes into `main` for its relative relocations (R_AARCH64_RELATIVE), by the address
// of each. The word at 4 of the text segment is the address of the module's MOD0 header. The header's word at 4 is the
// place of the dynamic section, counted from the header. The section's DT_RELA and DT_RELASZ give the relocations.
// Throws xsflib::FormatError if any of them is missing or out of main's segments.
std::map<uint64_t, uint64_t> RelativePointers(const ntdlib::nx::Nso& main);

// A timed event of a cutscene. When the cutscene reaches `frame`, Jac_DemoFrame acts on `value`. A value from -7 to -1
// is a command to Jac_DemoFrame: -1, -2 and -3 end the events, and -4 to -7 act on the cutscene's music. Jac_DemoFrame
// queues any other value for port 2 of the cutscenes' effect track (0x1000f). It compares the frames as unsigned.
struct CutsceneEvent
{
    uint16_t frame = 0;
    int16_t value = 0;
};

// A row of the cutscenes' table, with the values that the row stores. Jac_StartDemo writes the cutscene's number to
// port 0 of the cutscenes' effect track (0x1000f). It writes the number to port 1 as well if WritesPortOne gives true.
struct Cutscene
{
    uint32_t number = 0; // the row's place in the table

    uint8_t start_effect = 0;   // +0: Jac_StartDemo plays system effect 25 as the cutscene starts unless it's 0
    uint8_t music_fade = 0;     // +1: how Jac_StartDemo fades the music, 0 to 5
    uint8_t game_sounds = 0;    // +2: what Jac_StartDemo does to the game's events and tracks, 0 to 2
    uint8_t music = 0;          // +3: the cutscene's music, and how it starts
    uint8_t sequence_flags = 0; // +4

    // The events, from the pointer at +8, in their order. The last is the first with a value of -1, -2 or -3. None if
    // the pointer is null.
    std::vector<CutsceneEvent> events;
};

// Each row of the cutscenes' table. `pointers` are main's relative pointers. Throws xsflib::FormatError if a row or
// its events run out of main's segments, or if a row's pointer of events isn't null and isn't relocated.
std::vector<Cutscene> ReadCutscenes(const ntdlib::nx::Nso& main, const std::map<uint64_t, uint64_t>& pointers);

// Whether Jac_StartDemo writes the cutscene's number to port 1 of the cutscenes' effect track as well as to port 0. It
// does if the cutscene's sequence flags are 1 or 3.
bool WritesPortOne(const Cutscene& cutscene);

// The game's frames from the cutscene's event `index` to the cutscene's finish, or 0 if the cutscene has none.
// Jac_DemoFrame takes the events in their order. It acts on an event in the first frame that reaches the frames of
// that event and of each event before it. An event of -1 finishes the cutscene: Jac_DemoFrame then calls
// Jac_FinishDemo (0x261f90). That writes 0 to port 0 of the cutscenes' effect track. Events that end with -2 or -3 stop
// there and finish nothing.
uint16_t FramesToFinish(const Cutscene& cutscene, std::size_t index);

// An action of an event, with the values that its row stores. Jac_PlayEventAction gives an event of type t action a
// from row a + the first action of type t. It sends `command` to the event's track, with the number of the event's slot
// that the action takes (0 to 15) in bits 12 and up. The flags, the priority and the group decide the slot.
struct EventAction
{
    uint8_t flags = 0;    // +0
    uint8_t priority = 0; // +1
    uint8_t group = 0;    // +2
    uint16_t command = 0; // +4
};

// The tables of the events' actions, with the values that they store.
struct EventActions
{
    std::vector<int32_t> first_actions; // by the type of event, from 0 to 7
    std::vector<EventAction> actions;   // by the row
};

// The tables of the events' actions. Throws xsflib::FormatError if they run out of main's segments.
EventActions ReadEventActions(const ntdlib::nx::Nso& main);

// A row of the game's table of effects, with the values that it stores. The game gives JAudio the number `jaudio`. For
// an effect of `event_type` -2, one of the system's, it's the number that Jac_PlaySystemSe takes. For type -1, an
// effect of the captain's, it's the number that Jac_PlayOrimaSe takes. For the types from 1 to 7, it's the action that
// Jac_PlayEventAction plays on an object's event of that type.
struct Effect
{
    int32_t number = 0;     // +0: the game's number of the effect
    int32_t jaudio = 0;     // +4
    std::string name;       // from the pointer at +8
    int32_t loop = 0;       // +0x10: 1 for an effect that loops
    int32_t event_type = 0; // +0x14
};

// Each row of the game's table of effects. `pointers` are main's relative pointers. Throws xsflib::FormatError if a row
// or its name runs out of main's segments, or if a row's pointer of a name isn't relocated.
std::vector<Effect> ReadEffects(const ntdlib::nx::Nso& main, const std::map<uint64_t, uint64_t>& pointers);

// The sequence index (BARC) that Jac_Start (0x266db0) gives the sequence archive. The index is little-endian, with a
// 64-bit word at +8 (ntdlib/jaudio/barc.h). Its count of sequences is at +16, and its entries of 32 bytes start at +36.
inline constexpr uint64_t kSequenceIndexAt = 0x6af870;
inline constexpr std::size_t kIndexCountAt = 16;
inline constexpr std::size_t kIndexEntriesAt = 36;
inline constexpr std::size_t kIndexEntry = 32;

// The sequence index as main holds it: its header and its entries. Throws xsflib::FormatError if the index doesn't
// start with "BARC", or if it runs out of main's segments.
std::vector<uint8_t> ReadSequenceIndex(const ntdlib::nx::Nso& main);

// The tables that set the child tracks of a piece of music. Jac_PlayBgm (0x2642e0) plays file n of the sequence index
// as the game's piece n - 2, from file 2 on. The function at 0x264870 sets the piece's child tracks from two tables of
// a pointer for each piece. The first table's pointer points at four sets of the child tracks that play, a bit each.
// The second's points at two volumes for those tracks. The game picks a set and a volume by the piece's mode. A piece
// starts in mode 0, with the first set and the first volume. The other child tracks get a volume of 0.
inline constexpr uint64_t kPlayingTracksAt = 0x6af298;
inline constexpr uint64_t kTrackVolumesAt = 0x6af338;
inline constexpr std::size_t kPieces = 20;
inline constexpr uint16_t kFirstPieceFile = 2;

// The tables of the options' levels (0x264cc0), 11 levels each, from 0 to 10: the root track's volume in a piece of
// music (32-bit floats), and what the game writes to port 2 of the cutscenes' root track and to port 0 of the effects'
// root tracks (16 bits each). A new save sets the options' levels of the music and of the effects to kDefaultLevel
// (0x1b000).
inline constexpr uint64_t kMusicVolumesAt = 0x5be8f0;
inline constexpr uint64_t kCutsceneLevelsAt = 0x5be91c;
inline constexpr uint64_t kEffectLevelsAt = 0x5be948;
inline constexpr std::size_t kLevels = 11;
inline constexpr std::size_t kDefaultLevel = 8;

// The scenes of wave system 13 that __Prepare_BGM (0x261a10) sets for the pieces of the cutscenes' music: a 32-bit
// scene for each piece from 1 to 10. It sets the scene of pieces 1 and 5 to 10 alone.
inline constexpr uint64_t kPartScenesAt = 0x5be238;
inline constexpr std::size_t kParts = 10;

// What the music's tables give, with the levels at kDefaultLevel.
struct MusicTables
{
    std::vector<uint16_t> playing_tracks; // the first set of each piece, from file 2 on
    std::vector<float> track_volumes;     // the first volume of each piece, from file 2 on
    float music_volume = 1.0f;
    uint16_t cutscene_level = 0;
    uint16_t effect_level = 0;
    std::vector<uint32_t> part_scenes; // by the piece of the cutscenes' music, from 1 on
};

// The music's tables. `pointers` are main's relative pointers. Throws xsflib::FormatError if a table runs out of main's
// segments, or if a pointer of the first two tables isn't relocated. A null pointer isn't.
MusicTables ReadMusicTables(const ntdlib::nx::Nso& main, const std::map<uint64_t, uint64_t>& pointers);

// The settings of the FX lines that the audio thread's start-up (0x266c70) gives the DSP: 0x20 bytes for each of the
// 4 lines. A line's settings hold its mode at +0 (0 for off), the places in the table of buses of the two buses that it
// feeds at +2 and +6, its levels into them at +4 and +8, its delay in sub-frames at +0xc and its filter's 8
// coefficients at +0x10. The table of buses (kBusesAt) holds the DSP's address of each bus, 16 bits each. Its last
// address, 0, feeds no bus. The start-up gives each line a buffer of its delay.
inline constexpr uint64_t kFxLinesAt = 0x6af7f0;
inline constexpr std::size_t kFxLineSettings = 0x20;
inline constexpr uint64_t kBusesAt = 0x5bfa40;
inline constexpr std::size_t kBuses = 12;

// The FX lines as the DSP gets them. Throws xsflib::FormatError if the settings or the table of buses run out of main's
// segments, or if a line names a bus past the table's end.
std::array<JaudioFxLine, JaudioSettings::kFxLines> ReadFxLines(const ntdlib::nx::Nso& main);

} // namespace hsf::rip
