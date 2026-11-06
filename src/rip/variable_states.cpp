// SPDX-License-Identifier: MIT

#include "rip/variable_states.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

#include "xsflib/sha1.h"

namespace hsf::rip
{

namespace
{

// A sound archive that a table of waiting pieces belongs to: its size, its SHA-1 and its pieces.
struct WaitingArchive
{
    std::size_t size = 0;
    std::string_view sha1;
    std::vector<WaitingPiece> pieces;
};

// A piece of a rhythm mode (Super Mario Party Jamboree's rhythm mode, Super Mario Party's Sound Stage): its label, the
// number that its sequence writes to the global variable 29 as it starts, and the tempo that the game gives it.
struct RhythmPiece
{
    std::string_view label;
    int16_t number = 0;
    int16_t bpm = 120;
};

// A sub-archive of a rhythm mode: its size, its SHA-1 and its pieces.
struct RhythmArchive
{
    std::size_t size = 0;
    std::string_view sha1;
    std::vector<RhythmPiece> pieces;
};

// The waiting pieces of the rhythm archives `archives`. Each piece plays beside the controller `controller`
// (RhythmScript). The sub-archive `controller_archive` holds the controller.
std::vector<WaitingArchive> BesideController(const std::vector<RhythmArchive>& archives, std::string_view controller,
                                             std::string_view controller_archive)
{
    std::vector<WaitingArchive> waiting;
    for (const RhythmArchive& archive : archives)
    {
        WaitingArchive a;
        a.size = archive.size;
        a.sha1 = archive.sha1;
        for (const RhythmPiece& piece : archive.pieces)
        {
            a.pieces.push_back(
                {piece.label, {{"", {{27, piece.bpm}, {28, piece.number}}}}, controller, controller_archive});
        }
        waiting.push_back(std::move(a));
    }

    return waiting;
}

// Super Mario Party Jamboree's rhythm mode (the first release's main, build 0451231f26f181157cec81b4385b5ed1b1b259ff).
// Each piece's sequence writes its number to the global variable 29 and waits until the global variable 28 holds the
// same number (STARTTRIGGER_CHECK, offset 86 of each piece's sequence). The mode's controller, SQ_BGM_RC_MAIN_RHYTHM in
// subarc_rc_cmn, copies v28 = v29 at the end of a bar once the piece before has ended (countTrack, offset 491 of its
// sequence). The game starts the controller at the mode's start (RmSoundMan::StartModeMainBgm, 0x4264c4). Each piece's
// track 0 sets the tempo from the global variable 27 at every tick, or 120 when v27 is outside 0 to 1023 (TEMPO_CHECK,
// offset 318). The game writes the mode's tempo to v27 (0x425d70, from RmGameWork::SetBpm at 0x42a584 and
// RmMgSceneBase::SyncedSetupGame at 0x4437a4). A piece starts in the state the controller hands it over in: v28 at the
// piece's number, and v27 at the tempo of the piece. The game picks a minigame's B piece when the tempo is 121 or more
// (0x4429c0). The rhythm stage plays at 120 until its half-way minigame, and then at its speed-up tempo: 180 unless the
// game sets another (Scene::SettingBpm at 0x2c564 and Params::createInstance at 0x2ba34 of rc_stage01's module). The
// table gives the B pieces 180 and the others 120. An Eden run of the stage's start shows the game writing 120 to v27,
// then starting the controller and the first piece in the same frame.
std::vector<WaitingArchive> Jamboree()
{
    const std::vector<RhythmArchive> archives = {
        {1'729'416,
         "4fe37e040f806edf68d25366cab4df6bc6d5b0a6",
         {
             {"SQ_BGM_MG1801_A", 18011, 120},
             {"SQ_BGM_MG1801_A_INTER_END", 18014, 120},
             {"SQ_BGM_MG1801_A_MG_ENDING", 50, 120},
             {"SQ_BGM_MG1801_B", 18011, 180},
             {"SQ_BGM_MG1801_B_INTER_END", 18014, 180},
             {"SQ_BGM_MG1801_B_MG_ENDING", 50, 180},
             {"SQ_BGM_MG1801_C", 18013, 120},
             {"SQ_BGM_MG1801_C_INTER_END", 18014, 120},
             {"SQ_BGM_MG1801_C_MG_ENDING", 50, 120},
             {"SQ_BGM_MG1801_ENDING", 6, 120},
             {"SQ_BGM_MG1801_MG_ENDING", 50, 120},
         }},
        {2'698'016,
         "40d2a94a617503b330dced6d7622d0b26a602230",
         {
             {"SQ_BGM_MG1802_A", 18021, 120},
             {"SQ_BGM_MG1802_A_INTER_END", 18014, 120},
             {"SQ_BGM_MG1802_A_MG_ENDING", 50, 120},
             {"SQ_BGM_MG1802_B", 18021, 180},
             {"SQ_BGM_MG1802_B_INTER_END", 18014, 180},
             {"SQ_BGM_MG1802_B_MG_ENDING", 50, 180},
             {"SQ_BGM_MG1802_C", 18023, 120},
             {"SQ_BGM_MG1802_C_INTER_END", 18014, 120},
             {"SQ_BGM_MG1802_C_MG_ENDING", 50, 120},
             {"SQ_BGM_MG1802_ENDING", 6, 120},
             {"SQ_BGM_MG1802_MG_ENDING", 50, 120},
         }},
        {3'668'568,
         "3b63801b329e8ed903c3b1942e2a33a9a2a701e5",
         {
             {"SQ_BGM_MG1803_A", 18031, 120},
             {"SQ_BGM_MG1803_A_INTER_END", 18034, 120},
             {"SQ_BGM_MG1803_A_MG_ENDING", 6, 120},
             {"SQ_BGM_MG1803_B", 18031, 180},
             {"SQ_BGM_MG1803_B_INTER_END", 18034, 180},
             {"SQ_BGM_MG1803_B_MG_ENDING", 6, 180},
             {"SQ_BGM_MG1803_C", 18033, 120},
             {"SQ_BGM_MG1803_C_INTER_END", 18034, 120},
             {"SQ_BGM_MG1803_C_MG_ENDING", 6, 120},
             {"SQ_BGM_MG1803_ENDING", 6, 120},
             {"SQ_BGM_MG1803_MG_ENDING", 6, 120},
         }},
        {2'456'128,
         "75e42552a04ee46ffa669d41c347853b928f8d9a",
         {
             {"SQ_BGM_MG1804_A", 18041, 120},
             {"SQ_BGM_MG1804_A_INTER_END", 18014, 120},
             {"SQ_BGM_MG1804_A_MG_ENDING", 6, 120},
             {"SQ_BGM_MG1804_B", 18041, 180},
             {"SQ_BGM_MG1804_B_INTER_END", 18014, 180},
             {"SQ_BGM_MG1804_B_MG_ENDING", 6, 180},
             {"SQ_BGM_MG1804_C", 18043, 120},
             {"SQ_BGM_MG1804_C_INTER_END", 18014, 120},
             {"SQ_BGM_MG1804_C_MG_ENDING", 6, 120},
             {"SQ_BGM_MG1804_ENDING", 6, 120},
             {"SQ_BGM_MG1804_MG_ENDING", 6, 120},
         }},
        {1'591'496,
         "174f28343be60f0cc2e8d69ba01d50f60a648b72",
         {
             {"SQ_BGM_MG1805_A", 18051, 120},
             {"SQ_BGM_MG1805_A_INTER_END", 18014, 120},
             {"SQ_BGM_MG1805_A_MG_ENDING", 6, 120},
             {"SQ_BGM_MG1805_B", 18051, 180},
             {"SQ_BGM_MG1805_B_INTER_END", 18014, 180},
             {"SQ_BGM_MG1805_B_MG_ENDING", 6, 180},
             {"SQ_BGM_MG1805_C", 18053, 120},
             {"SQ_BGM_MG1805_C_INTER_END", 18014, 120},
             {"SQ_BGM_MG1805_C_MG_ENDING", 6, 120},
             {"SQ_BGM_MG1805_ENDING", 6, 120},
             {"SQ_BGM_MG1805_MG_ENDING", 6, 120},
         }},
        {2'091'480,
         "6984d7d099e4f3f9ef69dbd50ee2d6191ebf9ca9",
         {
             {"SQ_BGM_MG1806_A", 18061, 120},
             {"SQ_BGM_MG1806_A_INTER_END", 18014, 120},
             {"SQ_BGM_MG1806_A_MG_ENDING", 6, 120},
             {"SQ_BGM_MG1806_B", 18061, 180},
             {"SQ_BGM_MG1806_B_INTER_END", 18014, 180},
             {"SQ_BGM_MG1806_B_MG_ENDING", 6, 180},
             {"SQ_BGM_MG1806_C", 18063, 120},
             {"SQ_BGM_MG1806_C_INTER_END", 18014, 120},
             {"SQ_BGM_MG1806_C_MG_ENDING", 6, 120},
             {"SQ_BGM_MG1806_ENDING", 6, 120},
             {"SQ_BGM_MG1806_MG_ENDING", 6, 120},
         }},
        {1'720'736,
         "d0677b7ab75ad5ac1c2b60333f471d6e6e805b53",
         {
             {"SQ_BGM_MG1807_A", 18071, 120},
             {"SQ_BGM_MG1807_A_INTER_END", 18014, 120},
             {"SQ_BGM_MG1807_A_MG_ENDING", 6, 120},
             {"SQ_BGM_MG1807_B", 18071, 180},
             {"SQ_BGM_MG1807_B_INTER_END", 18014, 180},
             {"SQ_BGM_MG1807_B_MG_ENDING", 6, 180},
             {"SQ_BGM_MG1807_C", 18073, 120},
             {"SQ_BGM_MG1807_C_INTER_END", 18014, 120},
             {"SQ_BGM_MG1807_C_MG_ENDING", 6, 120},
             {"SQ_BGM_MG1807_ENDING", 6, 120},
             {"SQ_BGM_MG1807_MG_ENDING", 6, 120},
         }},
        {1'976'824,
         "cd9d29c129535ce4c5ae6d4bf2fc05618b0bfab5",
         {
             {"SQ_BGM_MG1808_A", 18081, 120},
             {"SQ_BGM_MG1808_A_INTER_END", 18014, 120},
             {"SQ_BGM_MG1808_A_MG_ENDING", 6, 120},
             {"SQ_BGM_MG1808_B", 18081, 180},
             {"SQ_BGM_MG1808_B_INTER_END", 18014, 180},
             {"SQ_BGM_MG1808_B_MG_ENDING", 6, 180},
             {"SQ_BGM_MG1808_C", 18083, 120},
             {"SQ_BGM_MG1808_C_INTER_END", 18014, 120},
             {"SQ_BGM_MG1808_C_MG_ENDING", 6, 120},
             {"SQ_BGM_MG1808_ENDING", 6, 120},
             {"SQ_BGM_MG1808_MG_ENDING", 6, 120},
         }},
        {1'933'752,
         "2ae02ec280db68336f835b03d5e26911dc0bd985",
         {
             {"SQ_BGM_MG1809_A", 18091, 120},
             {"SQ_BGM_MG1809_A_INTER_END", 18014, 120},
             {"SQ_BGM_MG1809_A_MG_ENDING", 6, 120},
             {"SQ_BGM_MG1809_B", 18091, 180},
             {"SQ_BGM_MG1809_B_INTER_END", 18014, 180},
             {"SQ_BGM_MG1809_B_MG_ENDING", 6, 180},
             {"SQ_BGM_MG1809_C", 18083, 120},
             {"SQ_BGM_MG1809_C_INTER_END", 18014, 120},
             {"SQ_BGM_MG1809_C_MG_ENDING", 6, 120},
             {"SQ_BGM_MG1809_ENDING", 6, 120},
             {"SQ_BGM_MG1809_MG_ENDING", 6, 120},
         }},
        {2'756'696,
         "45ce9f4c5dcefb24c33613659b8f91f2e03414c0",
         {
             {"SQ_BGM_MG1810_A", 18101, 120},
             {"SQ_BGM_MG1810_A_INTER_END", 18104, 120},
             {"SQ_BGM_MG1810_A_MG_ENDING", 6, 120},
             {"SQ_BGM_MG1810_B", 18101, 180},
             {"SQ_BGM_MG1810_B_INTER_END", 18104, 180},
             {"SQ_BGM_MG1810_B_MG_ENDING", 6, 180},
             {"SQ_BGM_MG1810_C", 18103, 120},
             {"SQ_BGM_MG1810_C_INTER_END", 18104, 120},
             {"SQ_BGM_MG1810_C_MG_ENDING", 6, 120},
             {"SQ_BGM_MG1810_ENDING", 6, 120},
             {"SQ_BGM_MG1810_MG_ENDING", 6, 120},
         }},
        {5'653'424,
         "bad0406e0b3687fdac636029abcad25a2747e4fe",
         {
             {"SQ_BGM_RC_BPMUP", 5, 120},           {"SQ_BGM_RC_CALIBRATION", 3, 120},
             {"SQ_BGM_RC_DATALOAD", 2, 120},        {"SQ_BGM_RC_GENERIC", 100, 120},
             {"SQ_BGM_RC_MG1801_DATALOAD", 2, 120}, {"SQ_BGM_RC_MG1802_DATALOAD", 2, 120},
             {"SQ_BGM_RC_MG1803_DATALOAD", 2, 120}, {"SQ_BGM_RC_MG1804_DATALOAD", 2, 120},
             {"SQ_BGM_RC_MG1805_DATALOAD", 2, 120}, {"SQ_BGM_RC_MG1806_DATALOAD", 2, 120},
             {"SQ_BGM_RC_MG1807_DATALOAD", 2, 120}, {"SQ_BGM_RC_MG1808_DATALOAD", 2, 120},
             {"SQ_BGM_RC_MG1809_DATALOAD", 2, 120}, {"SQ_BGM_RC_MG1810_DATALOAD", 2, 120},
             {"SQ_BGM_RC_MGCMN_OP", 1, 120},        {"SQ_BGM_RC_MG_PRE", 3, 120},
             {"SQ_BGM_RC_OP_DATALOAD", 4, 120},     {"SQ_BGM_RC_OP_ENCORE", 400, 120},
             {"SQ_BGM_RC_REMIX", 18000, 120},
         }},
        {4'273'296,
         "190b410c250e2684c412d99b5a7ca3a81434f5e6",
         {
             {"SQ_BGM_RC_STAGE01_OP_A", 200, 120},
             {"SQ_BGM_RC_STAGE01_OP_B", 201, 120},
             {"SQ_BGM_RC_STAGE01_OP_C", 202, 120},
             {"SQ_BGM_RC_STAGE01_OP_C_SKIP", 300, 120},
             {"SQ_BGM_RC_STAGE01_OP_D", 203, 120},
         }},
    };

    return BesideController(archives, "SQ_BGM_RC_MAIN_RHYTHM", "subarc_rc_cmn");
}

// Super Mario Party's Sound Stage (main build 9f13b697576664123716fbd655d9f47b): the arena bdcn001 and its ten
// minigames, mgcn401 to mgcn410. The stage's music has the design of Jamboree's rhythm mode. Each piece's sequence
// writes its number to the global variable 29 and waits until the global variable 28 holds the same number
// (STARTTRIGGER_CHECK, offset 72 of each piece's sequence). The controller, SQ_BGM_BDCN001_MAIN_RHYTHM in
// subarc_common_a, copies v28 = v29 at the start of each bar while bit 0 of the global variable 26 is set. It then
// clears the bit (countTrack, offset 468 of its sequence). The piece before sets bit 0 once the game has started the
// next piece (ENDPLAY_CHECK, offset 147). Bit 1 asks the controller for a fill. Each piece clears bit 1 as it starts
// (TEMPO_SETTING, offset 15). The controller starts with the first piece once v29 holds 1 or 200
// (STARTTRIGGER_CHECK_RHYTHM, offset 51). Each piece's track 0 sets the tempo from the global variable 27 at every
// tick, or 120 when v27 is outside 0 to 1023 (TEMPO_CHECK, offset 330). The game's scripts write the tempo to v27
// (system_boot/kernel/libca/script/beat_savework.lua, line 366, through the binding of WriteGlobalVariable at 0x494560
// in main). The stage's script gives the round's first three minigames their A piece at 120, and the later ones their B
// piece at 180 (mainmode/bdcn001/script/bdcn001.lua, lines 298 to 312). The table gives the B pieces 180 and the others
// 120.
std::vector<WaitingArchive> SuperMarioParty()
{
    const std::vector<RhythmArchive> archives = {
        {3'101'624,
         "d67d1c2a25b963f77d6b9bb9a537d7ed0c8e3405",
         {
             {"SQ_BGM_BDCN001_OP", 1, 120},
             {"SQ_BGM_BDCN001_OP_STAGE", 99, 120},
             {"SQ_BGM_BDCN001_OP_ENCORE", 200, 120},
         }},
        {28'266'968,
         "376717748d5ae339467d6e071adc95c211bda2fc",
         {
             {"SQ_BGM_BDCN001_MGCMN_OP", 1, 120},
             {"SQ_BGM_BDCN001_OP_DATALOAD", 4, 120},
             {"SQ_BGM_BDCN001_DATALOAD", 2, 120},
             {"SQ_BGM_BDCN001_CALIBRATION", 3, 120},
             {"SQ_BGM_BDCN001_BPMUP", 5, 120},
             {"SQ_BGM_BDCN001_MGCN_GENERIC", 100, 120},
             {"SQ_BGM_BDCN001_MGCN401_DATALOAD", 2, 120},
             {"SQ_BGM_BDCN001_MGCN401_PRE", 3, 120},
             {"SQ_BGM_BDCN001_MGCN402_DATALOAD", 2, 120},
             {"SQ_BGM_BDCN001_MGCN402_PRE", 3, 120},
             {"SQ_BGM_BDCN001_MGCN403_DATALOAD", 2, 120},
             {"SQ_BGM_BDCN001_MGCN403_PRE", 3, 120},
             {"SQ_BGM_BDCN001_MGCN404_DATALOAD", 2, 120},
             {"SQ_BGM_BDCN001_MGCN404_PRE", 3, 120},
             {"SQ_BGM_BDCN001_MGCN405_DATALOAD", 2, 120},
             {"SQ_BGM_BDCN001_MGCN405_PRE", 3, 120},
             {"SQ_BGM_BDCN001_MGCN406_DATALOAD", 2, 120},
             {"SQ_BGM_BDCN001_MGCN406_PRE", 3, 120},
             {"SQ_BGM_BDCN001_MGCN407_DATALOAD", 2, 120},
             {"SQ_BGM_BDCN001_MGCN407_PRE", 3, 120},
             {"SQ_BGM_BDCN001_MGCN408_DATALOAD", 2, 120},
             {"SQ_BGM_BDCN001_MGCN408_PRE", 3, 120},
             {"SQ_BGM_BDCN001_MGCN409_DATALOAD", 2, 120},
             {"SQ_BGM_BDCN001_MGCN409_PRE", 3, 120},
             {"SQ_BGM_BDCN001_MGCN410_DATALOAD", 2, 120},
             {"SQ_BGM_BDCN001_MGCN410_PRE", 3, 120},
         }},
        {2'281'088,
         "8a0a4266a15b64ad993779b00790c234026b9f2f",
         {
             {"SQ_BGM_BDCN001_MGCN401_A", 7, 120},
             {"SQ_BGM_BDCN001_MGCN401_B", 8, 180},
             {"SQ_BGM_BDCN001_MGCN401_A_END", 7, 120},
             {"SQ_BGM_BDCN001_MGCN401_B_END", 8, 180},
             {"SQ_BGM_BDCN001_MGCN401_C", 7, 120},
             {"SQ_BGM_BDCN001_MGCN401_C_END", 7, 120},
             {"SQ_BGM_BDCN001_MGCN401_C_END_FIN", 6, 120},
             {"SQ_BGM_BDCN001_MGCN401_A_END_FIN", 6, 120},
             {"SQ_BGM_BDCN001_MGCN401_B_END_FIN", 6, 180},
             {"SQ_BGM_BDCN001_MGCN401_MG_FIN", 50, 120},
         }},
        {2'330'952,
         "e942fe3bffd0ba4cd021f9a40e19ec372f6f50bb",
         {
             {"SQ_BGM_BDCN001_MGCN402_A", 10, 120},
             {"SQ_BGM_BDCN001_MGCN402_B", 11, 180},
             {"SQ_BGM_BDCN001_MGCN402_A_END", 10, 120},
             {"SQ_BGM_BDCN001_MGCN402_B_END", 11, 180},
             {"SQ_BGM_BDCN001_MGCN402_C", 10, 120},
             {"SQ_BGM_BDCN001_MGCN402_C_END", 10, 120},
             {"SQ_BGM_BDCN001_MGCN402_C_END_FIN", 6, 120},
             {"SQ_BGM_BDCN001_MGCN402_A_END_FIN", 6, 120},
             {"SQ_BGM_BDCN001_MGCN402_B_END_FIN", 6, 180},
             {"SQ_BGM_BDCN001_MGCN402_MG_FIN", 50, 120},
         }},
        {2'760'896,
         "13a7fb2982aeff80d658f47b26c32dfba98d746d",
         {
             {"SQ_BGM_BDCN001_MGCN403_A", 13, 120},
             {"SQ_BGM_BDCN001_MGCN403_B", 14, 180},
             {"SQ_BGM_BDCN001_MGCN403_A_END", 13, 120},
             {"SQ_BGM_BDCN001_MGCN403_B_END", 14, 180},
             {"SQ_BGM_BDCN001_MGCN403_C", 13, 120},
             {"SQ_BGM_BDCN001_MGCN403_C_END", 13, 120},
             {"SQ_BGM_BDCN001_MGCN403_C_END_FIN", 6, 120},
             {"SQ_BGM_BDCN001_MGCN403_A_END_FIN", 6, 120},
             {"SQ_BGM_BDCN001_MGCN403_B_END_FIN", 6, 180},
             {"SQ_BGM_BDCN001_MGCN403_MG_FIN", 50, 120},
         }},
        {1'364'408,
         "40bba41be9fc010ce4579eb5daecb8b8c75e4535",
         {
             {"SQ_BGM_BDCN001_MGCN404_A", 16, 120},
             {"SQ_BGM_BDCN001_MGCN404_B", 17, 180},
             {"SQ_BGM_BDCN001_MGCN404_A_END", 16, 120},
             {"SQ_BGM_BDCN001_MGCN404_B_END", 17, 180},
             {"SQ_BGM_BDCN001_MGCN404_C", 16, 120},
             {"SQ_BGM_BDCN001_MGCN404_C_END", 16, 120},
             {"SQ_BGM_BDCN001_MGCN404_C_END_FIN", 6, 120},
             {"SQ_BGM_BDCN001_MGCN404_A_END_FIN", 6, 120},
             {"SQ_BGM_BDCN001_MGCN404_B_END_FIN", 6, 180},
             {"SQ_BGM_BDCN001_MGCN404_MG_FIN", 50, 120},
         }},
        {3'221'056,
         "5c6b0312b7029c47340551223450b94c9a75896e",
         {
             {"SQ_BGM_BDCN001_MGCN405_A", 19, 120},
             {"SQ_BGM_BDCN001_MGCN405_B", 20, 180},
             {"SQ_BGM_BDCN001_MGCN405_A_END", 19, 120},
             {"SQ_BGM_BDCN001_MGCN405_B_END", 20, 180},
             {"SQ_BGM_BDCN001_MGCN405_C", 21, 120},
             {"SQ_BGM_BDCN001_MGCN405_C_END", 21, 120},
             {"SQ_BGM_BDCN001_MGCN405_C_END_FIN", 6, 120},
             {"SQ_BGM_BDCN001_MGCN405_A_END_FIN", 6, 120},
             {"SQ_BGM_BDCN001_MGCN405_B_END_FIN", 6, 180},
             {"SQ_BGM_BDCN001_MGCN405_MG_FIN", 50, 120},
         }},
        {1'994'984,
         "54478361610e3dbbc19dcce247eff5135420d936",
         {
             {"SQ_BGM_BDCN001_MGCN406_A", 22, 120},
             {"SQ_BGM_BDCN001_MGCN406_B", 23, 180},
             {"SQ_BGM_BDCN001_MGCN406_A_END", 22, 120},
             {"SQ_BGM_BDCN001_MGCN406_B_END", 23, 180},
             {"SQ_BGM_BDCN001_MGCN406_C", 22, 120},
             {"SQ_BGM_BDCN001_MGCN406_C_END", 22, 120},
             {"SQ_BGM_BDCN001_MGCN406_C_END_FIN", 6, 120},
             {"SQ_BGM_BDCN001_MGCN406_A_END_FIN", 6, 120},
             {"SQ_BGM_BDCN001_MGCN406_B_END_FIN", 6, 180},
             {"SQ_BGM_BDCN001_MGCN406_MG_FIN", 50, 120},
         }},
        {3'065'344,
         "b1e79b2b6c290d0c4fb3ce96b59a1463e6075d28",
         {
             {"SQ_BGM_BDCN001_MGCN407_A", 25, 120},
             {"SQ_BGM_BDCN001_MGCN407_B", 26, 180},
             {"SQ_BGM_BDCN001_MGCN407_A_END", 25, 120},
             {"SQ_BGM_BDCN001_MGCN407_B_END", 26, 180},
             {"SQ_BGM_BDCN001_MGCN407_C", 25, 120},
             {"SQ_BGM_BDCN001_MGCN407_C_END", 25, 120},
             {"SQ_BGM_BDCN001_MGCN407_C_END_FIN", 6, 120},
             {"SQ_BGM_BDCN001_MGCN407_A_END_FIN", 6, 120},
             {"SQ_BGM_BDCN001_MGCN407_B_END_FIN", 6, 180},
             {"SQ_BGM_BDCN001_MGCN407_MG_FIN", 50, 120},
         }},
        {4'211'520,
         "6367b6fb0442cfd35b2e9f213789ae6abd3a62fd",
         {
             {"SQ_BGM_BDCN001_MGCN408_A", 28, 120},
             {"SQ_BGM_BDCN001_MGCN408_B", 29, 180},
             {"SQ_BGM_BDCN001_MGCN408_A_END", 28, 120},
             {"SQ_BGM_BDCN001_MGCN408_B_END", 29, 180},
             {"SQ_BGM_BDCN001_MGCN408_C", 28, 120},
             {"SQ_BGM_BDCN001_MGCN408_C_END", 28, 120},
             {"SQ_BGM_BDCN001_MGCN408_C_END_FIN", 6, 120},
             {"SQ_BGM_BDCN001_MGCN408_A_END_FIN", 6, 120},
             {"SQ_BGM_BDCN001_MGCN408_B_END_FIN", 6, 180},
             {"SQ_BGM_BDCN001_MGCN408_MG_FIN", 50, 120},
         }},
        {2'966'776,
         "2401ab1d4864865ac0f56e9841349618ae2e23f4",
         {
             {"SQ_BGM_BDCN001_MGCN409_A", 31, 120},
             {"SQ_BGM_BDCN001_MGCN409_B", 32, 180},
             {"SQ_BGM_BDCN001_MGCN409_A_END", 31, 120},
             {"SQ_BGM_BDCN001_MGCN409_B_END", 32, 180},
             {"SQ_BGM_BDCN001_MGCN409_C", 33, 120},
             {"SQ_BGM_BDCN001_MGCN409_C_END", 33, 120},
             {"SQ_BGM_BDCN001_MGCN409_C_END_FIN", 6, 120},
             {"SQ_BGM_BDCN001_MGCN409_A_END_FIN", 6, 120},
             {"SQ_BGM_BDCN001_MGCN409_B_END_FIN", 6, 180},
             {"SQ_BGM_BDCN001_MGCN409_MG_FIN", 50, 120},
         }},
        {2'422'912,
         "832c888bd55e4a183239ee80f29cf48b54634a63",
         {
             {"SQ_BGM_BDCN001_MGCN410_A", 34, 120},
             {"SQ_BGM_BDCN001_MGCN410_B", 35, 180},
             {"SQ_BGM_BDCN001_MGCN410_A_END", 34, 120},
             {"SQ_BGM_BDCN001_MGCN410_B_END", 35, 180},
             {"SQ_BGM_BDCN001_MGCN410_C", 34, 120},
             {"SQ_BGM_BDCN001_MGCN410_C_END", 34, 120},
             {"SQ_BGM_BDCN001_MGCN410_C_END_FIN", 6, 120},
             {"SQ_BGM_BDCN001_MGCN410_A_END_FIN", 6, 120},
             {"SQ_BGM_BDCN001_MGCN410_B_END_FIN", 6, 180},
             {"SQ_BGM_BDCN001_MGCN410_MG_FIN", 50, 120},
         }},
    };

    return BesideController(archives, "SQ_BGM_BDCN001_MAIN_RHYTHM", "subarc_common_a");
}

const std::vector<WaitingArchive>& Archives()
{
    static const std::vector<WaitingArchive> kArchives = []
    {
        std::vector<WaitingArchive> archives = Jamboree();
        for (WaitingArchive& a : SuperMarioParty())
        {
            archives.push_back(std::move(a));
        }

        return archives;
    }();

    return kArchives;
}

} // namespace

std::span<const WaitingPiece> WaitingPieces(std::span<const uint8_t> archive)
{
    std::string sha1;
    for (const WaitingArchive& a : Archives())
    {
        if (archive.size() != a.size)
        {
            continue;
        }

        if (sha1.empty())
        {
            sha1 = xsflib::Sha1::Hex(xsflib::Sha1::Of(archive));
        }
        if (sha1 == a.sha1)
        {
            return a.pieces;
        }
    }

    return {};
}

const WaitingPiece* WaitingPieceOf(std::span<const WaitingPiece> pieces, std::string_view label)
{
    for (const WaitingPiece& piece : pieces)
    {
        if (label == piece.label ||
            (label.size() > piece.label.size() && label.starts_with(piece.label) && label[piece.label.size()] == ' '))
        {
            return &piece;
        }
    }

    return nullptr;
}

ScriptedStart RhythmScript(uint32_t piece, uint32_t controller, const xsflib::psf::Variables& variables,
                           uint16_t controller_archive)
{
    constexpr uint16_t kTempo = 27;
    constexpr uint16_t kHandedOver = 28;
    constexpr uint16_t kCurrent = 29;
    constexpr uint16_t kHandOver = 26;

    ScriptedStart script = {piece, 0, {}};
    if (const auto tempo = variables.find(kTempo); tempo != variables.end())
    {
        script.steps.push_back(ScriptStep::Write(0, kTempo, tempo->second));
    }

    script.steps.push_back(ScriptStep::Start(0, controller, controller_archive));

    const auto number = variables.find(kHandedOver);
    const int16_t n = number == variables.end() ? -1 : number->second;
    if (n == 1 || n == 200 || n == 300 || n == 400)
    {
        script.steps.push_back(ScriptStep::Start(0, piece));
        return script;
    }

    script.steps.push_back(ScriptStep::Write(2, kCurrent, 1));
    script.steps.push_back(ScriptStep::Start(4, piece));
    script.steps.push_back(ScriptStep::Write(8, kHandOver, 1));

    return script;
}

std::vector<ntdlib::rip::Job> WithVariableStates(std::span<const WaitingPiece> pieces,
                                                 const std::vector<ntdlib::rip::Job>& selected)
{
    std::vector<ntdlib::rip::Job> jobs;
    for (const ntdlib::rip::Job& job : selected)
    {
        const auto piece =
            std::find_if(pieces.begin(), pieces.end(), [&job](const WaitingPiece& p) { return p.label == job.label; });
        if (piece == pieces.end())
        {
            jobs.push_back(job);
            continue;
        }

        for (const VariableState& state : piece->states)
        {
            ntdlib::rip::Job in_state = job;
            if (!state.name.empty())
            {
                in_state.label += " ";
                in_state.label += state.name;
            }

            for (const StateVariable& v : state.variables)
            {
                in_state.variables[v.variable] = v.value;
            }

            jobs.push_back(std::move(in_state));
        }
    }

    return jobs;
}

} // namespace hsf::rip
