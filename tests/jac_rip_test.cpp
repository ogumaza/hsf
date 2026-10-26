// SPDX-License-Identifier: MIT

// hsfrip's parts for Pikmin (src/rip/jac_rip.*): the search for the game's files, the build ID of an NSO, the settings
// and the selection of sounds from synthetic tables. The tables hold a few rows of each kind that the selection treats
// differently.

#include "rip/jac_rip.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/jaudio/rarc.h"
#include "ntdlib/jaudio/wave_system.h"
#include "xsflib/testing/check.h"

#include "hsf/format.h"

using namespace xsflib::testing;

namespace
{

namespace rip = hsf::rip;
using hsf::JacEffect;
using hsf::JaudioLibrary3;

void TestFindFiles()
{
    const std::vector<std::string> files = {
        "romfs/game/SndData/Banks/pikibank.bx",
        "romfs/game/SndData/Banks/waves_0.aw",
        "romfs/game/SndData/Seqs/pikiseq.arc",
        "exefs/main.npdm",
        "exefs/main",
        "exefs/sdk",
    };
    const std::optional<rip::JacPaths> paths = rip::FindJacFiles(files);
    XSFLIB_CHECK(paths.has_value());
    XSFLIB_CHECK(paths->bank == "romfs/game/SndData/Banks/pikibank.bx");
    XSFLIB_CHECK(paths->waves == "romfs/game/SndData/Banks");
    XSFLIB_CHECK(paths->sequences == "romfs/game/SndData/Seqs/pikiseq.arc");
    XSFLIB_CHECK(paths->main == "exefs/main");

    // The names' case doesn't count. The paths keep the case of the game's files.
    const std::optional<rip::JacPaths> upper =
        rip::FindJacFiles({"ExeFS/MAIN", "RomFS/BANKS/PIKIBANK.BX", "RomFS/SEQS/PIKISEQ.ARC"});
    XSFLIB_CHECK(upper.has_value());
    XSFLIB_CHECK(upper->bank == "RomFS/BANKS/PIKIBANK.BX" && upper->sequences == "RomFS/SEQS/PIKISEQ.ARC");
    XSFLIB_CHECK(upper->main == "ExeFS/MAIN");

    // Without main, and without a sequence archive among the files.
    const std::optional<rip::JacPaths> alone = rip::FindJacFiles({"Banks/pikibank.bx", "main"});
    XSFLIB_CHECK(alone.has_value());
    XSFLIB_CHECK(alone->bank == "Banks/pikibank.bx" && alone->waves == "Banks");
    XSFLIB_CHECK(alone->sequences == "Seqs/pikiseq.arc" && alone->main.empty());

    // A bank file outside a folder named Banks isn't Pikmin's.
    XSFLIB_CHECK(!rip::FindJacFiles({"pikibank.bx"}).has_value());
    XSFLIB_CHECK(!rip::FindJacFiles({"Waves/pikibank.bx", "exefs/main"}).has_value());
}

void TestBuildId()
{
    std::vector<uint8_t> header(0x100, 0xee);
    header[0] = 'N';
    header[1] = 'S';
    header[2] = 'O';
    header[3] = '0';
    for (std::size_t i = 0; i < 20; ++i)
    {
        header[0x40 + i] = static_cast<uint8_t>(0x10 + i);
    }
    XSFLIB_CHECK(rip::NsoBuildId(header) == "101112131415161718191a1b1c1d1e1f20212223");

    header[3] = '1';
    XSFLIB_CHECK(rip::NsoBuildId(header).empty());
    header[3] = '0';
    header.resize(0x53);
    XSFLIB_CHECK(rip::NsoBuildId(header).empty());
}

// The synthetic tables of the selection's test. No sequence shares another's data.
const std::vector<uint8_t> kSequence = {0xc3};

std::vector<ntdlib::jaudio::ArchiveFile> Sequences()
{
    return {{0, "effects.jam", kSequence, 0},   {1, "events.jam", kSequence, 1},   {2, "empty", {}, 2},
            {4, "piece4.jam", kSequence, 4},    {6, "piece6.jam", kSequence, 6},   {13, "parts.jam", kSequence, 13},
            {21, "piece21.jam", kSequence, 21}, {22, "piece22.jam", kSequence, 22}};
}

std::vector<ntdlib::jaudio::WaveSystem> Systems()
{
    std::vector<ntdlib::jaudio::WaveSystem> systems;
    for (const auto [number, groups] : {std::pair{4u, 1u}, std::pair{13u, 9u}, std::pair{14u, 1u}, std::pair{21u, 1u}})
    {
        ntdlib::jaudio::WaveSystem& system = systems.emplace_back();
        system.number = number;
        system.groups.resize(groups);
    }

    return systems;
}

rip::Cutscene MakeCutscene(uint32_t number, uint8_t music, uint8_t flags, std::vector<rip::CutsceneEvent> events)
{
    rip::Cutscene cutscene;
    cutscene.number = number;
    cutscene.music = music;
    cutscene.sequence_flags = flags;
    cutscene.events = std::move(events);

    return cutscene;
}

rip::Effect MakeRow(int32_t number, int32_t jaudio, std::string name, int32_t event_type)
{
    rip::Effect row;
    row.number = number;
    row.jaudio = jaudio;
    row.name = std::move(name);
    row.event_type = event_type;

    return row;
}

rip::JacGameCode Code()
{
    rip::JacGameCode code;
    for (uint16_t i = 0; i < rip::kPieces; ++i)
    {
        code.music.playing_tracks.push_back(static_cast<uint16_t>(0x1000 + i));
        code.music.track_volumes.push_back(static_cast<float>(i) / 100.0f);
    }
    code.music.music_volume = 0.75f;
    code.music.cutscene_level = 8000;
    code.music.effect_level = 20000;
    code.music.part_scenes = {7, 7, 7, 7, 3, 4, 5, 6, 1, 8};
    code.fx_lines[2].flags = 2;
    code.fx_lines[2].subframes = 40;

    // Cutscene 0 plays piece 1 of the cutscenes' music. It writes its number to port 1 too. Cutscene 1 plays piece 5
    // (bit 5 set), cutscene 2 a stream, cutscene 3 no music, cutscene 4 piece 3 and cutscene 5 the diary's piece 2.
    // Piece 3 has no scene.
    code.cutscenes = {
        MakeCutscene(0, 0x01, 1, {{4, -6}, {10, 3}, {20, 5}, {30, 3}, {40, -1}}),
        MakeCutscene(1, 0x25, 0, {{0, 0}, {5, -2}}),
        MakeCutscene(2, 0x86, 1, {}),
        MakeCutscene(3, 0xc0, 0, {}),
        MakeCutscene(4, 0x03, 2, {{1, 0}, {2, -3}}),
        MakeCutscene(5, 0x02, 0, {}),
    };

    // Type 1: action 0 is limited (slot 0 on a new event), action 1 isn't (the last free slot, 15). Type 7: action 4
    // starts as the event is created and refuses a slot of its group. Action 0 then takes slot 14, action 2 (limited)
    // slot 0, and action 1 (of action 4's group, refusing) is refused.
    code.actions.first_actions = {0, 1, 3, 5, 7, 9, 11, 13};
    code.actions.actions.resize(18);
    code.actions.actions[1] = {0x10, 0, 1, 0x21};
    code.actions.actions[2] = {0x00, 0, 2, 0x22};
    code.actions.actions[13] = {0x00, 0, 5, 0x31};
    code.actions.actions[14] = {0x20, 0, 10, 0x32};
    code.actions.actions[15] = {0x10, 0, 6, 0x33};
    code.actions.actions[17] = {0x20, 0, 10, 0x35};

    code.effects = {
        MakeRow(0, 0x05, "SYSTEM_5", -2),
        MakeRow(1, 0x10, "SYSTEM_CONTAINER", -2),
        MakeRow(2, 0x12, "SYSTEM_SHIP_PART", -2),
        MakeRow(3, 0x26, "SYSTEM_PAUSE_ALL", -2),
        MakeRow(4, 0x05, "SYSTEM_5_AGAIN", -2),
        MakeRow(5, 0x20, "SYSTEM_STREAM", -2),
        MakeRow(6, 0x04, "CAPTAIN_PLUCK", -1),
        MakeRow(7, 0x8003, "PIKMIN_3", -1),
        MakeRow(8, 1, "OBJECT_1_1", 1),
        MakeRow(9, 0, "OBJECT_1_0", 1),
        MakeRow(10, 4, "SHIP_4", 7),
        MakeRow(11, 1, "SHIP_REFUSED", 7),
        MakeRow(12, 0, "SHIP_0", 7),
        MakeRow(13, 2, "SHIP_2", 7),
        MakeRow(14, 0, "ODD", 9),
        MakeRow(15, 5, "SHIP_PAST", 7),
    };

    return code;
}

JacEffect MakeEffect(uint8_t kind, uint8_t type, uint16_t value, uint16_t first = JacEffect::kNone)
{
    JacEffect effect;
    effect.kind = kind;
    effect.type = type;
    effect.value = value;
    effect.first = first;

    return effect;
}

std::string LabelsOf(const rip::JacSelection& selection)
{
    std::string labels;
    for (const rip::JaudioJob& job : selection.jobs)
    {
        labels += (labels.empty() ? "" : " ") + job.label;
    }

    return labels;
}

void TestSelection()
{
    const rip::JacSelection all = rip::SelectJacSounds(Code(), Sequences(), Systems(), std::regex(""), false);
    const JaudioLibrary3& library = all.library;

    XSFLIB_CHECK(library.effect_level == 20000 && library.cutscene_level == 8000 && library.music_volume == 0.75f);
    XSFLIB_CHECK(library.startup_files == (std::array<uint16_t, 4>{0, 1, 13, 2}));
    XSFLIB_CHECK(library.loads.size() == 8);
    XSFLIB_CHECK(library.loads.front() == (hsf::JacLoad{{0, 0}, 1}));
    XSFLIB_CHECK(library.loads[3] == (hsf::JacLoad{{22, 0}, 0}));
    XSFLIB_CHECK(library.loads.back() == (hsf::JacLoad{{2, 0}, 1}));

    // The pieces: file 6's wave system is missing, and file 22 has no settings.
    XSFLIB_CHECK(library.pieces.size() == 2);
    XSFLIB_CHECK(library.pieces[0] == (hsf::JacPiece{4, 0x1002, 0.02f, {4, 0}}));
    XSFLIB_CHECK(library.pieces[1] == (hsf::JacPiece{21, 0x1013, 0.19f, {21, 0}}));

    // The pieces of the cutscenes' music: the cutscenes' 1, 2, 3 and 5, and the ship's piece 8. Piece 3 has no scene.
    XSFLIB_CHECK(library.parts.size() == 4);
    XSFLIB_CHECK(library.parts[0] == (hsf::JacPart{1, {13, 7}}));
    XSFLIB_CHECK(library.parts[1] == (hsf::JacPart{2, {14, 0}}));
    XSFLIB_CHECK(library.parts[2] == (hsf::JacPart{5, {13, 3}}));
    XSFLIB_CHECK(library.parts[3] == (hsf::JacPart{8, {13, 6}}));

    XSFLIB_CHECK(all.warnings == (std::vector<std::string>{
                                     "piece6's wave system 6 isn't in the bank file; the piece is left out",
                                     "piece22 has no settings in the game's code; it's left out",
                                     "parts-3 has no scene in the game's code; it's left out",
                                     "ODD has an unknown type, 9; it's left out",
                                 }));

    // The effects of the table, then those of the code's numbers, then the cutscenes' sounds.
    const std::vector<JacEffect> table = {
        MakeEffect(JacEffect::kSystem, 0, 5),
        MakeEffect(JacEffect::kCaptain, 0, 0xe),
        MakeEffect(JacEffect::kSystem, 0, 0x1c),
        MakeEffect(JacEffect::kCaptain, 0, 4, 0xb),
        MakeEffect(JacEffect::kPikmin, 0, 3),
        MakeEffect(JacEffect::kEvent, 1, 0xf022),
        MakeEffect(JacEffect::kEvent, 1, 0x0021),
        MakeEffect(JacEffect::kEvent, 7, 0xf035),
        MakeEffect(JacEffect::kEvent, 7, 0xe031, 0xf035),
        MakeEffect(JacEffect::kEvent, 7, 0x0033, 0xf035),
    };
    XSFLIB_CHECK(library.effects.size() == table.size() + 3 + 1 + 13 + 1 + 29 + 1 + 4);
    for (std::size_t i = 0; i < table.size(); ++i)
    {
        Check(library.effects[i] == table[i], "the table's effect " + std::to_string(i));
    }

    std::size_t at = table.size();
    for (const uint16_t value : {0x19, 0x1e, 0x1f})
    {
        XSFLIB_CHECK(library.effects[at++] == MakeEffect(JacEffect::kSystem, 0, value));
    }
    XSFLIB_CHECK(library.effects[at++] == MakeEffect(JacEffect::kCaptain, 0, 0));
    for (uint16_t value = 0xb; value <= 0x17; ++value)
    {
        XSFLIB_CHECK(library.effects[at++] == MakeEffect(JacEffect::kPikmin, 0, value));
    }
    XSFLIB_CHECK(library.effects[at++] == MakeEffect(JacEffect::kFormation, 0, 127));
    for (uint16_t size = 1; size <= 29; ++size)
    {
        XSFLIB_CHECK(library.effects[at++] == MakeEffect(JacEffect::kCrowd, 0, size));
    }
    XSFLIB_CHECK(library.effects[at++] == MakeEffect(JacEffect::kWalk, 0, 1));

    // Cutscene 0 finishes at frame 40. Cutscenes 1 and 4 have no finish.
    for (const auto [value, finish] : {std::pair<uint16_t, uint16_t>{3, 30}, {5, 20}})
    {
        JacEffect effect = MakeEffect(JacEffect::kCutscene, JacEffect::kCutscenePart, value, 0);
        effect.finish = finish;
        XSFLIB_CHECK(library.effects[at++] == effect);
    }
    XSFLIB_CHECK(library.effects[at++] == MakeEffect(JacEffect::kCutscene, 0, 0, 1));
    XSFLIB_CHECK(library.effects[at++] == MakeEffect(JacEffect::kCutscene, 0, 0, 4));

    const std::string labels = LabelsOf(all);
    XSFLIB_CHECK(
        labels.starts_with("piece4 piece21 parts-1 parts-2 parts-5 parts-8 SYSTEM_5 SYSTEM_CONTAINER "
                           "SYSTEM_PAUSE_ALL CAPTAIN_PLUCK PIKMIN_3 OBJECT_1_1 OBJECT_1_0 SHIP_4 SHIP_0 "
                           "SHIP_2 system-0x19 system-0x1e system-0x1f captain-0x00 pikmin-0x0b"));
    XSFLIB_CHECK(
        labels.ends_with("pikmin-0x17 formation crowd-01 crowd-02 crowd-03 crowd-04 crowd-05 crowd-06 "
                         "crowd-07 crowd-08 crowd-09 crowd-10 crowd-11 crowd-12 crowd-13 crowd-14 crowd-15 "
                         "crowd-16 crowd-17 crowd-18 crowd-19 crowd-20 crowd-21 crowd-22 crowd-23 crowd-24 "
                         "crowd-25 crowd-26 crowd-27 crowd-28 crowd-29 footstep cutscene-000-03 "
                         "cutscene-000-05 cutscene-001-00 cutscene-004-00"));

    // Each sound's ID: its section and its place in its list.
    XSFLIB_CHECK(all.jobs[0].id == JaudioLibrary3::kPieceSection &&
                 all.jobs[1].id == (JaudioLibrary3::kPieceSection | 1));
    XSFLIB_CHECK(all.jobs[2].id == JaudioLibrary3::kPartSection &&
                 all.jobs[5].id == (JaudioLibrary3::kPartSection | 3));
    XSFLIB_CHECK(all.jobs[6].id == JaudioLibrary3::kEffectSection);
    XSFLIB_CHECK(all.jobs.back().id == (JaudioLibrary3::kEffectSection | (library.effects.size() - 1)));
    for (std::size_t i = 0; i < all.jobs.size(); ++i)
    {
        Check(all.jobs[i].index == i, "job " + std::to_string(i) + "'s index");
    }

    // Only the music, and only the sounds whose labels match.
    const rip::JacSelection music = rip::SelectJacSounds(Code(), Sequences(), Systems(), std::regex(""), true);
    XSFLIB_CHECK(LabelsOf(music) == "piece4 piece21 parts-1 parts-2 parts-5 parts-8");
    XSFLIB_CHECK(music.library.effects.empty());

    const rip::JacSelection some =
        rip::SelectJacSounds(Code(), Sequences(), Systems(), std::regex("^(parts-5|cutscene-000|SHIP_0)"), false);
    XSFLIB_CHECK(LabelsOf(some) == "parts-5 SHIP_0 cutscene-000-03 cutscene-000-05");
    XSFLIB_CHECK(some.library.parts == (std::vector<hsf::JacPart>{{5, {13, 3}}}));
    XSFLIB_CHECK(some.library.effects.size() == 3);
    XSFLIB_CHECK(some.jobs[1].id == JaudioLibrary3::kEffectSection &&
                 some.jobs[3].id == (JaudioLibrary3::kEffectSection | 2));
}

void TestSettings()
{
    const rip::JacGameCode code = Code();
    const hsf::JaudioSettings settings = rip::JacSettings(code);

    XSFLIB_CHECK(settings.library == hsf::JaudioSettings::kPikminLibrary);
    XSFLIB_CHECK(settings.dsp_level == 6144);
    XSFLIB_CHECK(settings.fx_lines == code.fx_lines);
    XSFLIB_CHECK(settings.music_volume == 1.0f && settings.effect_volume == 1.0f);
}

} // namespace

int main()
{
    const auto tests = []
    {
        TestFindFiles();
        TestBuildId();
        TestSelection();
        TestSettings();
    };

    return RunTest("jac_rip", tests);
}
