// SPDX-License-Identifier: MIT

// hsfrip's parts for Pikmin, whose Switch release plays its sound with the C JAudio, Jac (src/jac): the game's sound
// files and its code among its files, what the game's code sets up for its sounds, and the sounds to rip. A rip of the
// game has a library 3 descriptor (docs/hsf.md, "Library 3: Pikmin").

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/jaudio/rarc.h"
#include "ntdlib/jaudio/wave_system.h"
#include "ntdlib/nx/nso.h"

#include "hsf/format.h"
#include "rip/jac_tables.h"
#include "rip/jaudio_rip.h"

namespace hsf::jac
{
class SoundFiles;
} // namespace hsf::jac

namespace hsf::rip
{

// Pikmin's files that a rip reads, by their paths among the game's files. The RomFS's folder SndData holds the bank
// file (Banks/pikibank.bx), the wave archives (Banks) and the sequence archive (Seqs/pikiseq.arc). The ExeFS holds the
// game's code, main. `main` is empty when the game's files lack it.
struct JacPaths
{
    std::string bank;
    std::string sequences;
    std::string waves;
    std::string main;
};

// Pikmin's files among a game's files `files`, or nothing. The bank file is a file named pikibank.bx in a folder named
// Banks. The sequence archive is Seqs/pikiseq.arc beside that folder. main is a file named main in a folder named
// exefs. The case of the names doesn't count.
std::optional<JacPaths> FindJacFiles(const std::vector<std::string>& files);

// The build of main whose tables hsfrip reads: the release's version 1.0.0, with this build ID.
inline constexpr std::string_view kJacMainVersion = "1.0.0";
inline constexpr std::string_view kJacMainBuildId = "342a165cc42e997edf15023143252e9f7e5c1a8d";

// An NSO's build ID in hex: the first 20 bytes of the module's ID, at 0x40 in the header. Empty if `file` doesn't start
// as an NSO.
std::string NsoBuildId(std::span<const uint8_t> file);

// What a rip takes from main: the sequence index, the music's tables, the FX lines, the cutscenes' table, the events'
// actions and the game's table of effects (jac_tables.h).
struct JacGameCode
{
    std::vector<uint8_t> sequence_index;
    MusicTables music;
    std::array<JaudioFxLine, JaudioSettings::kFxLines> fx_lines{};
    std::vector<Cutscene> cutscenes;
    EventActions actions;
    std::vector<Effect> effects;
};

// The FILE chunk of a rip that holds the sequence index. No file of a RomFS can have its path.
inline constexpr std::string_view kJacSequenceIndexPath = "exefs/pikiseq.hed";

// The DSP's level that the game gives the DSP's mixer as it starts (Jac_Start, 0x266f5c): 1.5, in 4.12.
inline constexpr uint32_t kJacDspLevel = 6144;

// Reads what a rip takes from main, the build whose ID is kJacMainBuildId. Throws xsflib::FormatError if main is
// damaged or one of the tables runs out of main's segments.
JacGameCode ReadJacGameCode(const ntdlib::nx::Nso& main);

// The JAudio settings of a rip of Pikmin, from the game's code: library 3, the DSP's level and the FX lines. The
// managers' and the categories' volumes stay at 1.
JaudioSettings JacSettings(const JacGameCode& code);

// The sounds of a rip of Pikmin, and the descriptor's lists of what they play.
struct JacSelection
{
    std::vector<JaudioJob> jobs;
    JaudioLibrary3 library;
    std::vector<std::string> warnings;
};

// Pikmin's sounds whose labels match `only`, and what the game's code sets up for them. `sequences` are the sequence
// archive's files, as the sequence index divides it. `systems` are the bank file's wave systems.
//
// The pieces of music come first: each file of the sequence archive from file 2 on, but the cutscenes' music. A piece
// is named after its file, without the extension. Its scene is scene 0 of the wave system of its file's number, as the
// game sets it for the piece. A piece whose system the bank file lacks is left out. The pieces of the cutscenes' music
// follow: those that the cutscenes' table names, and the piece that plays as a part of the ship appears (piece 8). Such
// a piece is named after the cutscenes' music, a hyphen and its number, as demobgm-2. The game sets piece 2's waves
// with the diary's scenes as its title screen starts. The piece's scene is the diary's first scene.
//
// The effects follow, in the order of their kinds. Each row of the game's table of effects is an effect named as the
// row names it. The system's effects that the game plays as other sounds take those sounds: one of the captain's, or
// the system's effect of the pause. Those that play a piece of the cutscenes' music, a stream or nothing are left out.
// The effects that the game's code plays with numbers of its own follow, as system-0x19, captain-0x00 and pikmin-0x0b:
// the system's effect of a cutscene's start and two more of the system's, the captain's effects of a cutscene's start
// and of a filled container, and the Pikmin's cries as the game starts and as a Pikmin is plucked. Then come the
// formation's sound at the stick's full distance (formation), the crowd of Pikmin at each of its 29 sizes (crowd-01 to
// crowd-29) and the captain's footstep (footstep). Last come the sounds of each cutscene, as cutscene-004-01 for sound
// 1 of cutscene 4. An effect that another one plays the same way is left out. With `bgm`, only the music.
//
// A sound's ID is its section in the top 2 bits and its place in its list of the descriptor in the low 16.
JacSelection SelectJacSounds(const JacGameCode& code, const std::vector<ntdlib::jaudio::ArchiveFile>& sequences,
                             const std::vector<ntdlib::jaudio::WaveSystem>& systems, const std::regex& only, bool bgm);

// Works out how long the sounds of a library 3 descriptor play, on the model of the C JAudio set up as the player sets
// it up (hsf/jac.h). Analyze can be called from several threads at once.
class JacLengthAnalyzer
{
public:
    // `files` must outlive the analyzer.
    JacLengthAnalyzer(const jac::SoundFiles& files, ArchiveDescriptor settings);

    // As JaudioLengthAnalyzer::Analyze does, for the sound with ID `sound`. Pikmin plays no piece through a conductor.
    // Every loop therefore counts. The timing is empty when the sound can't start.
    JaudioTiming Analyze(uint32_t sound) const;

private:
    const jac::SoundFiles& files_;
    ArchiveDescriptor settings_;
};

} // namespace hsf::rip
