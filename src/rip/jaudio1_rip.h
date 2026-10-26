// SPDX-License-Identifier: MIT

// hsfrip's parts for Pikmin 2, whose Switch release plays its sound with JAudio1 (src/jaudio1): the game's sound files
// among its files, what the game's code sets up for each piece of music, the sounds to rip, and their lengths. A rip
// of the game has a library 2 descriptor (docs/hsf.md, "Library 2: Pikmin 2").

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <vector>

#include "hsf/format.h"
#include "hsf/jaudio1.h"
#include "rip/jaudio_rip.h"
#include "rip/rip.h"

namespace hsf::rip
{

// Pikmin 2's files that a rip reads, by their paths among the game's files. The folder AudioRes holds the start-up
// archive (an AAF), the sequence archive (Seqs/Seq.arc), the wave archives (Banks), the conductor archive
// (Conductor.arc) and the sounds' names (an .asn file). The folder user beside it holds the game's list of its music
// (Totaka/BgmList.txt) and the track maps.
struct Jaudio1Paths
{
    std::string startup;
    std::string sequences;
    std::string waves;
    std::string conductors;
    std::string names;
    std::string user;
};

// The JAudio settings of a rip of Pikmin 2, from the game's code: library 2, the DSP's level that the game's start-up
// sets (JASDriver::setLevel), and the options' volumes of the music and the effects, 1 at the start.
JaudioSettings Jaudio1Settings();

// Pikmin 2's files among a game's files `files`, or nothing. The start-up archive is a file directly in a folder named
// AudioRes, with a name that ends in .aaf, whose SHA-1 is that of the Switch release's. `read` reads the game's files.
std::optional<Jaudio1Paths> FindJaudio1Files(const std::vector<std::string>& files, const ReadGameFile& read);

// The game's list of its music (PSSystem::SeqDataList): each file's volume by the file's name. The list is text: a
// name and a number in turn, up to the word endoffile. The game reads each number's leading digits. The first entry
// of a name counts. A token that starts with # starts a comment, up to the end of its line. Braces separate tokens.
std::map<std::string, uint8_t> ReadMusicVolumes(std::span<const uint8_t> text);

// A track map of the game's directors (PSM::BgmTrackMap): the counts of the piece's tracks of each kind, in their
// order. The basic tracks come first. The directors' layers follow: events, treasure, enemies nearby, battles and the
// ground.
using TrackCounts = std::array<uint8_t, 6>;

// The track maps of a track map file (PSM::BgmTrackMapFile), by the name of the piece or the conductor that each one
// is for. Each map is a name, the 6 counts, then a flag for each of 16 tracks and 8 more for the Pikmin director. The
// game reads at most 32 maps. The text is laid out as for ReadMusicVolumes. Throws xsflib::FormatError for a count
// past 15 or a flag past 1, as the game stops at one.
std::map<std::string, TrackCounts> ReadTrackMaps(std::span<const uint8_t> text);

// The tracks that the directors mute as a piece starts: every layer of the map, a bit for each.
uint16_t MutedTracks(const TrackCounts& counts);

// What a rip of Pikmin 2 takes from the game's files besides its sound files: the music's volumes, the four track map
// files' maps (trackMap_Cond_T.txt, trackMap_Seq_T.txt, trackMap_Cond_W.txt and trackMap_Seq_W.txt, in that order)
// and the sounds' names by their IDs.
struct Jaudio1GameText
{
    std::map<std::string, uint8_t> volumes;
    std::array<std::map<std::string, TrackCounts>, 4> track_maps;
    std::map<uint32_t, std::string> names;
};

// Reads what a rip takes from the game's files besides its sound files. The name file may be missing. Returns an error
// for another file that the game lacks or that can't be read.
std::optional<std::string> ReadJaudio1GameText(const Jaudio1Paths& paths, const ReadGameFile& read,
                                               Jaudio1GameText& text);

// The sounds of a rip of Pikmin 2, and the descriptor's list of the pieces that they play.
struct Jaudio1Selection
{
    std::vector<JaudioJob> jobs;
    JaudioLibrary2 library;
    std::vector<std::string> warnings;
};

// Pikmin 2's sounds whose labels match `only`: each piece of music that a file of the sequence archive holds, with
// what the game's code sets up for it, then the effects of the sound table. A piece of the cave music plays once with
// each of its conductors. With `bgm`, only the music. A piece's ID is section 2 and its place in the descriptor's list.
// An effect's ID is the sound table's, with bit 11 set (the game keeps the effect until it ends). A piece is named
// after its file, a piece of the cave music after its conductor, and an effect as the name file names it, or after
// its category and index.
Jaudio1Selection SelectJaudio1Sounds(const Jaudio1Files& files, const Jaudio1GameText& text, const std::regex& only,
                                     bool bgm);

// Works out how long the sounds of a library 2 descriptor play, on the model of JAudio1 set up as the player sets it
// up (hsf/jaudio1.h). Analyze can be called from several threads at once.
class Jaudio1LengthAnalyzer
{
public:
    // `files` must outlive the analyzer.
    Jaudio1LengthAnalyzer(const Jaudio1Files& files, ArchiveDescriptor settings);

    // As JaudioLengthAnalyzer::Analyze does, for the sound with ID `sound`. A piece that a conductor plays never
    // repeats: its loops don't count. The timing is empty when the sound can't start. With `draws`, the sound starts
    // after that many draws from the sequences' generator, in place of the descriptor's draws for it.
    JaudioTiming Analyze(uint32_t sound, std::optional<uint32_t> draws = std::nullopt) const;

private:
    const Jaudio1Files& files_;
    ArchiveDescriptor settings_;
};

} // namespace hsf::rip
