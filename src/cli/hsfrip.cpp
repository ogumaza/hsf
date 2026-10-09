// SPDX-License-Identifier: MIT

// hsfrip: builds HSF sets (.hsflib and .minihsf files) from Switch games and sound archives.
//
// usage: hsfrip [options] <input> [<input> ...]
//   <input>          a Switch game's files or a sound archive (.bfsar). The game's files are a folder (its RomFS, once
//                    it's extracted) or a Bezel Engine archive (.bea). An NSP, XCI or NCA holds them encrypted, and is
//                    refused.
//   -o, --output DIR the folder the rips go in (default: a folder next to each input, with the input's name and "_hsf"
//                    added)
//   --list           list each game's sound archives, or what an archive holds, then stop
//   --archive PATH   rip only this sound archive of a game (its path among the game's files)
//   --only REGEX     rip only sounds whose label matches (ECMAScript regex)
//   --bgm            rip only the music (rip::IsMusic): sequences whose labels say BGM, or that play on a music player
//   --no-length      don't analyze lengths (no length/fade tags, and only the sequences at a volume of 0 are found to
//                    make no sound)
//   --voices N       nn::atk's voice count (default: a Bezel Engine game's voice count, from its boot.lua, or else 96)
//   --no-effects     leave out the reverb a Bezel Engine game puts on aux bus A
//   --jobs N         length analyses run in parallel (default: the number of CPU threads)
//   --game NAME      game tag (default: the input's name)
//   --artist NAME, --year YEAR, --copyright TEXT, --by NAME (hsfby)
//   --version        print the version
//
// Each of a game's sound archives gets a folder of its own in the output folder. The folder has the archive's name.
// Where two of the game's archives have the same name, the folders above each archive are added to it. A game may keep
// a sound archive's group files outside the archive. The ripper finds them by the groups' names among the game's files,
// or for a loose archive in its folder and the folders below it. Sequences whose data isn't all there are listed and
// skipped. So are those that make no sound: those whose volume in the archive is 0, and those that the length analysis
// finds never sound. The inputs are ripped one after another. An input that fails doesn't stop the rest. Dropping files
// or folders on the program gives it them as inputs.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <map>
#include <new>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "cli/console.h"
#include "common/ascii.h"
#include "fsar/formats.h"
#include "game/game.h"
#include "hsf/format.h"
#include "rip/rip.h"

namespace
{

namespace fs = std::filesystem;
using hsf::FileChunk;
using hsf::Tags;

// The effect a Bezel Engine game's scenes put on aux bus A unless one names another.
constexpr char kDefaultReverb[] = "I3DL2_Default";

// What follows the error for another console's sound archive: a sound archive of the 3DS, the Wii or the Wii U (see
// rip::IsWiiUArchive).
constexpr char kSwitchArchives[] = "HSF rips the Switch's sound archives (little-endian FSAR, .bfsar)";

constexpr char kUsage[] =
    "usage: hsfrip [options] <input> [<input> ...]\n"
    "\n"
    "Rips each input into an .hsflib and a .minihsf per sound. The rip goes in a folder next to the\n"
    "input, with the input's name and \"_hsf\" added. An input is a Switch game's files or a sound\n"
    "archive (.bfsar). The game's files are a folder (its RomFS, once it's extracted) or a Bezel\n"
    "Engine archive (.bea). An NSP, XCI or NCA file holds a game's files encrypted, and hsfrip\n"
    "doesn't read it. Files and folders can also be dropped on the program. Sequences that make no\n"
    "sound, and those whose data isn't all there, are listed and skipped.\n"
    "\n"
    "options:\n"
    "  -o, --output DIR     put the rips in DIR\n"
    "  --list               list a game's sound archives, or what a sound archive holds, then stop\n"
    "  --archive PATH       rip only this sound archive of a game (its path among the game's files)\n"
    "  --only REGEX         rip only the sounds whose label matches\n"
    "  --bgm                rip only the music: sequences whose labels say BGM, or that play on\n"
    "                       a player named for music (BGM, MUSIC, SONG or FANFARE)\n"
    "  --no-length          skip length analysis and omit length and fade tags. The analysis also\n"
    "                       finds the sequences that make no sound. Without it, only those at a\n"
    "                       volume of 0 are skipped.\n"
    "  --voices N           the sound library's voice count, 1 to 96 (default: what a Bezel\n"
    "                       Engine game's boot.lua gives, or else 96)\n"
    "  --no-effects         leave out the reverb a Bezel Engine game puts on aux bus A\n"
    "  --jobs N             the number of length analyses that run at once (default: the number\n"
    "                       of CPU threads)\n"
    "  --game NAME, --artist NAME, --year YEAR, --copyright TEXT, --by NAME\n"
    "                       tags (the game defaults to the input's name)\n"
    "  --version            show the version\n"
    "  -h, --help           show this help\n";

bool WriteFile(const fs::path& path, const std::vector<uint8_t>& data)
{
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    f.close();

    return !f.fail();
}

struct Options
{
    std::regex only{".*"};
    std::string only_text; // the pattern --only gave, or empty
    std::string out_dir, only_archive, game_name, artist, year, copyright, by = "hsfrip";
    bool list = false, lengths = true, bgm = false, effects = true;
    unsigned jobs = 0;
    int voices = 0; // 0: the game's voice count, or 96
};

Tags CommonTags(const Options& o, const std::string& game_name)
{
    Tags common;
    common["game"] = game_name;
    if (!o.artist.empty())
    {
        common["artist"] = o.artist;
    }
    if (!o.year.empty())
    {
        common["year"] = o.year;
    }
    if (!o.copyright.empty())
    {
        common["copyright"] = o.copyright;
    }
    common["hsfby"] = o.by;
    common["utf8"] = "1"; // tags are UTF-8 (PSF convention)

    return common;
}

// What a sound archive is ripped with: the archive's file and the group files found for it, by their paths among the
// game's files (or beside a loose archive), and the descriptor's settings. The length analysis plays with the same
// settings.
struct ArchiveToRip
{
    FileChunk archive;
    std::vector<FileChunk> groups;
    hsf::ArchiveDescriptor settings;
};

struct Job
{
    uint32_t index;
    std::string label;
    std::optional<hsf::rip::Timing> timing;
};

// Length analysis runs every selected sequence through the model. Because the sounds are independent, the analyses are
// spread over several threads.
void AnalyzeLengths(std::vector<Job>& jobs, hsf::fsar::SoundArchive archive, const hsf::ArchiveDescriptor& settings,
                    unsigned threads)
{
    hsf::atk::EngineOptions options;
    options.voice_count = static_cast<int>(settings.voice_count);
    const hsf::rip::LengthAnalyzer analyzer(std::move(archive), options, settings.effects);
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> done{0};
    std::atomic<bool> out_of_memory{false};
    const auto worker = [&]
    {
        for (std::size_t i; (i = next++) < jobs.size();)
        {
            try
            {
                jobs[i].timing = analyzer.Analyze(jobs[i].index);
            }
            catch (const std::bad_alloc&)
            {
                out_of_memory = true;
                next = jobs.size(); // the other threads take no more jobs
                return;
            }

            if (!jobs[i].timing)
            {
                std::fprintf(stderr, "warning: length analysis failed for %s; no length tag written\n",
                             jobs[i].label.c_str());
            }

            const std::size_t n = ++done;
            if (n % 100 == 0 || n == jobs.size())
            {
                std::fprintf(stderr, "  analyzed %zu of %zu\n", n, jobs.size());
            }
        }
    };

    threads = std::max(1u, std::min<unsigned>(threads, static_cast<unsigned>(jobs.size())));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; t++)
    {
        // If another thread can't start (the system is out of threads or memory), the ones already going do the work.
        try
        {
            pool.emplace_back(worker);
        }
        catch (const std::exception&)
        {
            break;
        }
    }

    worker();

    for (auto& t : pool)
    {
        t.join();
    }

    if (out_of_memory)
    {
        throw std::runtime_error("out of memory for the length analysis; try fewer --jobs");
    }
}

// Returns the input's absolute path, without the trailing separator of a folder given as "game/" or ".".
fs::path InputPath(const std::string& input)
{
    const fs::path path = fs::absolute(input).lexically_normal();
    return path.has_filename() ? path : path.parent_path();
}

// The name of `input`: the archive's file name without its extension, or the folder's name.
std::string NameOfInput(const std::string& input)
{
    const fs::path path = InputPath(input);
    return fs::is_directory(path) ? path.filename().string() : path.stem().string();
}

// Returns the folder that a rip of `input` goes in without -o: a folder next to the input, with the input's name and
// "_hsf" added.
fs::path DefaultOutputDir(const std::string& input)
{
    return InputPath(input).parent_path() / (NameOfInput(input) + "_hsf");
}

// The registry of file names for the rips in `folder`. Rips in one folder share it. Then no rip overwrites another's
// files. Folders whose names differ only in case count as one. Windows and macOS treat them as one folder: two inputs
// named "Sound.bfsar" and "sound.fsst" rip into one folder there.
hsf::rip::FileNames& NamesIn(std::map<std::string, hsf::rip::FileNames>& registries, const fs::path& folder)
{
    return registries[hsf::AsciiLower(fs::absolute(folder).lexically_normal().string())];
}

// The folders that the sets of a game's sound archives go in, given the archives' paths in the title. Each folder's
// name is the archive's name without its extension. Where two archives' names are the same, ignoring case, the folders
// above each archive are added until the folders' names differ (content/BGM/sound.bfsar and content/SE/sound.bfsar go
// in BGM_sound and SE_sound).
std::vector<std::string> ArchiveFolders(const std::vector<std::string>& paths)
{
    // Each path's parts from the archive up: its name without the extension, then the folders above it.
    std::vector<std::vector<std::string>> parts;
    for (const auto& path : paths)
    {
        std::vector<std::string> p{fs::path(path).stem().string()};
        for (fs::path folder = fs::path(path).parent_path(); folder.has_filename(); folder = folder.parent_path())
        {
            p.push_back(folder.filename().string());
        }
        parts.push_back(std::move(p));
    }

    // Each round, the archives whose folders clash take one more part.
    std::vector<std::size_t> used(paths.size(), 1);
    std::vector<std::string> folders(paths.size());
    for (bool clash = true; clash;)
    {
        std::map<std::string, std::vector<std::size_t>> by_name;
        for (std::size_t i = 0; i < paths.size(); i++)
        {
            folders[i] = parts[i][0];
            for (std::size_t j = 1; j < used[i]; j++)
            {
                folders[i] = parts[i][j] + "_" + folders[i];
            }
            by_name[hsf::AsciiLower(folders[i])].push_back(i);
        }

        clash = false;
        for (const auto& [name, archives] : by_name)
        {
            for (const std::size_t i : archives)
            {
                if (archives.size() > 1 && used[i] < parts[i].size())
                {
                    used[i]++;
                    clash = true;
                }
            }
        }
    }

    return folders;
}

// The sequences that --only and --bgm pick.
std::vector<Job> SelectSounds(const hsf::fsar::SoundArchive& archive, const Options& o)
{
    std::vector<Job> jobs;
    const auto& sounds = archive.Sounds();
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        // Streams are out of scope. Wave sounds aren't ripped yet.
        const auto index = static_cast<uint32_t>(i);
        if (sounds[i].type == hsf::fsar::SoundType::kSequence && std::regex_search(sounds[i].name, o.only) &&
            (!o.bgm || hsf::rip::IsMusic(archive, index)))
        {
            jobs.push_back({index, sounds[i].name, std::nullopt});
        }
    }

    return jobs;
}

// Writes one .minihsf per job.
int WriteMinis(const fs::path& out_dir, hsf::rip::FileNames& names, const std::string& lib_name,
               const std::vector<Job>& jobs, const Tags& common)
{
    int count = 0;
    for (const Job& job : jobs)
    {
        Tags tags = common;
        tags["title"] = job.label;
        tags["hsf_sound"] = job.label;
        if (job.timing)
        {
            tags["length"] = hsf::FormatTime(job.timing->length_ms);
            tags["fade"] = hsf::FormatTime(job.timing->fade_ms);
        }

        const std::string name = names.Mini(job.index, job.label);
        if (!WriteFile(out_dir / name, hsf::rip::BuildMini(job.index, lib_name, tags)))
        {
            std::fprintf(stderr, "error: can't write %s\n", name.c_str());
            return -1;
        }

        count++;
        std::fprintf(stderr, "%s%s%s\n", name.c_str(), tags.count("length") ? "  length " : "",
                     tags.count("length") ? tags["length"].c_str() : "");
    }

    return count;
}

// Reports that --only or --bgm picked no sound. That's an error: nothing gets written.
int NoSoundMatches(const Options& o)
{
    if (o.only_text.empty())
    {
        std::fprintf(stderr, "error: no sequence is music (--bgm)\n");
    }
    else
    {
        std::fprintf(stderr, "error: no %s matches '%s'\n", o.bgm ? "music" : "sound", o.only_text.c_str());
    }

    return 1;
}

// Prints the folder that a rip went in. That line is the last line about the input.
void ReportOutput(const fs::path& out_dir)
{
    std::fprintf(stderr, "output: %s\n", fs::absolute(out_dir).lexically_normal().string().c_str());
}

// "; 3 sequences skipped (2 data not there, 1 no sound)", or nothing when none were.
std::string SkippedText(std::size_t missing_data, std::size_t silent)
{
    const std::size_t total = missing_data + silent;
    if (total == 0)
    {
        return "";
    }

    std::string why = missing_data ? "data not there" : "no sound";
    if (missing_data && silent)
    {
        why = std::to_string(missing_data) + " data not there, " + std::to_string(silent) + " no sound";
    }

    return "; " + std::to_string(total) + (total == 1 ? " sequence" : " sequences") + " skipped (" + why + ")";
}

// Rips one sound archive into `out_dir` and adds the number of sounds --only picked to `matched`. `names` is the
// registry of file names for `out_dir`. Returns the number of .minihsf files written, or -1 on an error.
int RipArchive(const fs::path& out_dir, hsf::rip::FileNames& names, const std::string& lib_base,
               const ArchiveToRip& rip, const std::string& game_name, const Options& o, std::size_t& matched)
{
    const std::string& path = rip.archive.path;
    std::optional<hsf::fsar::SoundArchive> archive;
    try
    {
        archive = hsf::fsar::SoundArchive::Load(rip.archive.data);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s: %s\n", path.c_str(), e.what());
        return -1;
    }

    if (archive->Truncated())
    {
        std::fprintf(stderr, "warning: %s is truncated: sounds whose data lies past its end are skipped\n",
                     path.c_str());
    }

    for (const FileChunk& group : rip.groups)
    {
        archive->AddGroupFile(group.data);
    }

    std::vector<Job> jobs = SelectSounds(*archive, o);
    matched += jobs.size();
    if (jobs.empty())
    {
        return 0;
    }

    // Sequences whose data isn't all there would play with missing instruments. A sound whose volume in the archive is
    // 0 makes no sound. The game's sound library multiplies everything the sound plays by that volume.
    std::vector<Job> complete;
    std::size_t missing_data = 0;
    std::size_t silent = 0;
    for (Job& job : jobs)
    {
        const auto missing = hsf::rip::MissingData(*archive, job.index);
        if (!missing.empty())
        {
            missing_data++;
            std::string what;
            for (std::size_t i = 0; i < missing.size(); i++)
            {
                what += (i ? ", " : "") + missing[i];
            }
            std::fprintf(stderr, "skipped %s: missing %s\n", job.label.c_str(), what.c_str());
        }
        else if (archive->Sounds()[job.index].volume == 0)
        {
            silent++;
            std::fprintf(stderr, "skipped %s: it makes no sound (volume 0)\n", job.label.c_str());
        }
        else
        {
            complete.push_back(std::move(job));
        }
    }

    // The length analysis finds the other sequences that never sound. Some play no notes, and some play their notes at
    // a volume of 0.
    if (o.lengths && !complete.empty())
    {
        AnalyzeLengths(complete, std::move(*archive), rip.settings, o.jobs);
        std::vector<Job> sounding;
        for (Job& job : complete)
        {
            if (job.timing && job.timing->silent)
            {
                silent++;
                std::fprintf(stderr, "skipped %s: it makes no sound\n", job.label.c_str());
            }
            else
            {
                sounding.push_back(std::move(job));
            }
        }
        complete = std::move(sounding);
    }

    if (complete.empty())
    {
        std::fprintf(stderr, "%s: nothing to rip%s\n", path.c_str(), SkippedText(missing_data, silent).c_str());
        return 0;
    }

    const Tags common = CommonTags(o, game_name);
    const std::string lib_name = names.Library(lib_base);
    const std::vector<uint8_t> lib = hsf::rip::BuildArchiveLibrary(rip.archive, rip.groups, rip.settings, common);
    fs::create_directories(out_dir);
    if (!WriteFile(out_dir / lib_name, lib))
    {
        std::fprintf(stderr, "error: can't write %s\n", lib_name.c_str());
        return -1;
    }
    std::fprintf(stderr, "wrote %s (%zu bytes)\n", lib_name.c_str(), lib.size());

    const int count = WriteMinis(out_dir, names, lib_name, complete, common);
    if (count >= 0)
    {
        std::fprintf(stderr, "%s: %d minihsf files%s\n", path.c_str(), count,
                     SkippedText(missing_data, silent).c_str());
    }

    return count;
}

// What a sound archive holds (such as "FSAR 2.2.0, 2911 sequences, 28 wave sounds, 220 streams"), or why it can't be
// read or ripped.
std::string DescribeArchive(std::vector<uint8_t> bytes)
{
    if (hsf::rip::IsWiiUArchive(bytes))
    {
        return "a Wii U sound archive (big-endian FSAR). HSF doesn't rip those";
    }

    try
    {
        const hsf::fsar::SoundArchive archive = hsf::fsar::SoundArchive::Load(std::move(bytes));
        size_t sequences = 0, wave_sounds = 0, streams = 0;
        for (const hsf::fsar::SoundInfo& sound : archive.Sounds())
        {
            sequences += sound.type == hsf::fsar::SoundType::kSequence;
            wave_sounds += sound.type == hsf::fsar::SoundType::kWaveSound;
            streams += sound.type == hsf::fsar::SoundType::kStream;
        }
        const uint32_t v = archive.Version();
        char text[160];
        std::snprintf(text, sizeof(text), "FSAR %u.%u.%u, %zu sequences, %zu wave sounds, %zu streams%s",
                      v >> 16 & 0xFF, v >> 8 & 0xFF, v & 0xFF, sequences, wave_sounds, streams,
                      archive.Truncated() ? ", truncated" : "");

        return text;
    }
    catch (const std::exception& e)
    {
        return std::string("can't be read: ") + e.what();
    }
}

// Returns the group files that `archive` lists without data and that `find` supplies. `find` looks a group up by its
// name, and returns the group's file as its path and bytes, or nothing. A group that isn't found is reported.
std::vector<FileChunk> FindGroups(const std::vector<uint8_t>& archive_bytes,
                                  const std::function<std::optional<FileChunk>(const std::string& name)>& find)
{
    std::vector<FileChunk> groups;
    std::vector<std::string> missing;
    try
    {
        for (const std::string& name : hsf::rip::ExternalGroups(hsf::fsar::SoundArchive::Load(archive_bytes)))
        {
            if (std::optional<FileChunk> group = find(name))
            {
                groups.push_back(std::move(*group));
            }
            else
            {
                missing.push_back(name);
            }
        }
    }
    catch (const std::exception&)
    {
        return {}; // RipArchive reports an archive it can't read
    }

    for (const std::string& name : missing)
    {
        std::fprintf(stderr, "warning: group %s is kept outside the archive, and %s.bfgrp isn't there\n", name.c_str(),
                     name.c_str());
    }

    return groups;
}

// The settings every sound archive of a game is ripped with: the voice count, and the reverb a Bezel Engine game puts
// on aux bus A.
hsf::ArchiveDescriptor GameSettings(const hsf::game::GameFiles& game, const Options& o)
{
    hsf::ArchiveDescriptor settings;
    if (o.voices > 0)
    {
        settings.voice_count = static_cast<uint32_t>(o.voices);
    }
    else if (game.voice_count && *game.voice_count >= 1 && *game.voice_count <= 96)
    {
        settings.voice_count = static_cast<uint32_t>(*game.voice_count);
    }

    const auto reverb = game.i3dl2_presets.find(kDefaultReverb);
    if (o.effects && reverb != game.i3dl2_presets.end())
    {
        hsf::AuxEffect effect;
        effect.bus = 0;
        effect.type = hsf::AuxEffect::kI3dl2Reverb;
        effect.parameters.assign(reverb->second.begin(), reverb->second.end());
        settings.effects.push_back(std::move(effect));
    }

    return settings;
}

// The group file `name` anywhere among a game's files, found by its file name in any case, or nothing. MSVC 19.51
// failed with an internal compiler error (C1001) on a lambda in RipGame that did this with std::filesystem::path.
std::optional<FileChunk> FindGameGroup(const hsf::game::GameFiles& game, const std::string& name)
{
    const std::string file = hsf::AsciiLower(name + ".bfgrp");
    for (const std::string& path : game.files)
    {
        const std::size_t slash = path.rfind('/');
        const std::string file_name = slash == std::string::npos ? path : path.substr(slash + 1);
        if (hsf::AsciiLower(file_name) != file)
        {
            continue;
        }

        FileChunk group;
        group.path = path;
        if (game.read_file(path, group.data))
        {
            return group;
        }
    }

    return std::nullopt;
}

// Rips a game's sound archives (or the one --archive names). Each archive's folder takes its registry of file names
// from `registries` (see NamesIn). Returns the exit code.
int RipGame(const std::string& input, const fs::path& out_dir, const Options& o,
            std::map<std::string, hsf::rip::FileNames>& registries)
{
    hsf::game::GameFiles game;
    if (const auto error = hsf::game::LoadGame(InputPath(input), game))
    {
        std::fprintf(stderr, "error: %s: %s\n", input.c_str(), error->c_str());
        return 1;
    }

    const auto& archives = game.sound_archives;
    const hsf::ArchiveDescriptor settings = GameSettings(game, o);
    if (o.list)
    {
        std::printf("%s: %zu files, %zu sound archive%s\n", input.c_str(), game.files.size(), archives.size(),
                    archives.size() == 1 ? "" : "s");
        for (const std::string& path : archives)
        {
            std::vector<uint8_t> bytes;
            const bool read = game.read_file(path, bytes);
            const size_t size = bytes.size();
            const std::string held = read ? DescribeArchive(std::move(bytes)) : "can't be read";
            std::printf("  %s: %zu bytes, %s\n", path.c_str(), size, held.c_str());
        }
        std::printf("voices: %u%s; reverb on aux bus A: %s\n", settings.voice_count,
                    game.voice_count ? " (boot.lua)" : "", settings.effects.empty() ? "none" : kDefaultReverb);
        return 0;
    }

    if (archives.empty())
    {
        std::fprintf(stderr, "error: %s: no sound archive among its %zu files\n", input.c_str(), game.files.size());
        return 1;
    }
    if (!o.only_archive.empty() && std::find(archives.begin(), archives.end(), o.only_archive) == archives.end())
    {
        std::fprintf(stderr, "error: the game has no sound archive %s (--list shows them)\n", o.only_archive.c_str());
        return 1;
    }

    std::fprintf(stderr, "%zu sound archive%s; %u voices%s; %s on aux bus A\n", archives.size(),
                 archives.size() == 1 ? "" : "s", settings.voice_count, game.voice_count ? " (boot.lua)" : "",
                 settings.effects.empty() ? "no reverb" : kDefaultReverb);

    const auto find_group = [&game](const std::string& name)
    {
        return FindGameGroup(game, name);
    };

    const std::string game_name = o.game_name.empty() ? game.name : o.game_name;
    const std::vector<std::string> folders = ArchiveFolders(archives);
    int total = 0;
    std::size_t matched = 0;
    bool failed = false; // an archive that can't be ripped doesn't stop the others
    for (std::size_t a = 0; a < archives.size(); a++)
    {
        const std::string& path = archives[a];
        if (!o.only_archive.empty() && path != o.only_archive)
        {
            continue;
        }

        ArchiveToRip rip;
        rip.settings = settings;
        rip.archive.path = path;
        if (!game.read_file(path, rip.archive.data))
        {
            std::fprintf(stderr, "error: can't read %s\n", path.c_str());
            failed = true;
            continue;
        }
        if (hsf::rip::IsWiiUArchive(rip.archive.data))
        {
            std::fprintf(stderr, "error: %s is a Wii U sound archive (big-endian FSAR); %s\n", path.c_str(),
                         kSwitchArchives);
            failed = true;
            continue;
        }
        rip.groups = FindGroups(rip.archive.data, find_group);

        // Each archive's set goes in a folder of its own.
        const fs::path archive_dir = out_dir / folders[a];
        const int n = RipArchive(archive_dir, NamesIn(registries, archive_dir), game_name + " " + folders[a], rip,
                                 game_name, o, matched);
        if (n < 0)
        {
            failed = true;
            continue;
        }
        total += n;
    }

    if (matched == 0 && (!o.only_text.empty() || o.bgm) && !failed)
    {
        return NoSoundMatches(o);
    }

    std::fprintf(stderr, "%s: %d minihsf files\n", InputPath(input).filename().string().c_str(), total);
    if (total > 0)
    {
        ReportOutput(out_dir);
    }

    return failed ? 1 : 0;
}

// Rips a loose sound archive into `out_dir`. `names` is the registry of file names for `out_dir`. Returns the exit
// code.
int RipLooseArchive(const std::string& input, const fs::path& out_dir, const Options& o, hsf::rip::FileNames& names)
{
    const fs::path path = InputPath(input);
    const std::string file_name = path.filename().string();
    if (!o.only_archive.empty())
    {
        std::fprintf(stderr, "error: --archive picks one of a game's sound archives, and %s is one already\n",
                     input.c_str());
        return 2;
    }

    ArchiveToRip rip;
    rip.archive.path = file_name;
    if (o.voices > 0)
    {
        rip.settings.voice_count = static_cast<uint32_t>(o.voices);
    }
    if (!hsf::ReadWholeFile(path.string(), rip.archive.data))
    {
        std::fprintf(stderr, "error: can't read %s\n", input.c_str());
        return 1;
    }
    if (o.list)
    {
        std::printf("%s: a sound archive, %s\n", input.c_str(), DescribeArchive(rip.archive.data).c_str());
        return 0;
    }

    // A group file is found by its name in the archive's folder or a folder below it. The group file is kept at its
    // path from there.
    const fs::path folder = path.parent_path();
    const auto find_group = [&folder](const std::string& name) -> std::optional<FileChunk>
    {
        const std::string file = hsf::AsciiLower(name + ".bfgrp");
        std::error_code ec;
        for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        {
            FileChunk group{it->path().lexically_relative(folder).generic_string(), {}};
            if (it->is_regular_file(ec) && hsf::AsciiLower(it->path().filename().string()) == file &&
                hsf::ReadWholeFile(it->path().string(), group.data))
            {
                return group;
            }
        }

        return std::nullopt;
    };
    rip.groups = FindGroups(rip.archive.data, find_group);

    const std::string game_name = o.game_name.empty() ? NameOfInput(input) : o.game_name;
    std::size_t matched = 0;
    const int count = RipArchive(out_dir, names, game_name, rip, game_name, o, matched);
    if (count < 0)
    {
        return 1;
    }
    if (matched == 0 && (!o.only_text.empty() || o.bgm))
    {
        return NoSoundMatches(o);
    }
    if (count > 0)
    {
        ReportOutput(out_dir);
    }

    return 0;
}

// The first bytes of a file, or an empty string for a folder or a file that can't be read. The first bytes distinguish
// a sound archive from a game.
std::string ReadHead(const std::string& path)
{
    std::string head;
    std::error_code ec;
    if (fs::is_regular_file(path, ec))
    {
        std::ifstream f(path, std::ios::binary);
        head.resize(8);
        f.read(head.data(), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<std::size_t>(f.gcount()));
    }

    return head;
}

// Reads the options and inputs. Returns 0 to go on, -1 when there's nothing more to do (after --help or --version), or
// the exit code.
int ParseArgs(int argc, char** argv, Options& o, std::vector<std::string>& inputs)
{
    std::string only;
    for (int i = 1; i < argc; i++)
    {
        const std::string a = argv[i];
        bool missing_value = false;
        const auto next = [&]() -> std::string
        {
            if (i + 1 >= argc)
            {
                missing_value = true;
                return "";
            }

            return argv[++i];
        };

        if (a == "-h" || a == "--help")
        {
            std::fputs(kUsage, stdout);
            return -1;
        }
        else if (a == "--version")
        {
            std::printf("hsfrip %s\n", HSF_VERSION);
            return -1;
        }
        else if (a == "-o" || a == "--output")
        {
            o.out_dir = next();
        }
        else if (a == "--list")
        {
            o.list = true;
        }
        else if (a == "--archive")
        {
            o.only_archive = next();
        }
        else if (a == "--only")
        {
            only = next();
        }
        else if (a == "--bgm")
        {
            o.bgm = true;
        }
        else if (a == "--no-length")
        {
            o.lengths = false;
        }
        else if (a == "--no-effects")
        {
            o.effects = false;
        }
        else if (a == "--voices")
        {
            const std::string v = next();
            char* end = nullptr;
            const long n = std::strtol(v.c_str(), &end, 10);
            if (v.empty() || *end || n < 1 || n > 96)
            {
                std::fprintf(stderr, "error: --voices takes a number from 1 to 96, not '%s'\n", v.c_str());
                return 2;
            }
            o.voices = static_cast<int>(n);
        }
        else if (a == "--jobs")
        {
            const std::string v = next();
            char* end = nullptr;
            const long n = std::strtol(v.c_str(), &end, 10);
            if (v.empty() || *end || n < 1 || n > 1024)
            {
                std::fprintf(stderr, "error: --jobs takes a number of threads from 1 to 1024, not '%s'\n", v.c_str());
                return 2;
            }
            o.jobs = static_cast<unsigned>(n);
        }
        else if (a == "--game")
        {
            o.game_name = next();
        }
        else if (a == "--artist")
        {
            o.artist = next();
        }
        else if (a == "--year")
        {
            o.year = next();
        }
        else if (a == "--copyright")
        {
            o.copyright = next();
        }
        else if (a == "--by")
        {
            o.by = next();
        }
        else if (a.size() > 1 && a[0] == '-')
        {
            std::fprintf(stderr, "error: unknown option %s (see --help)\n", a.c_str());
            return 2;
        }
        else
        {
            inputs.push_back(a);
        }

        if (missing_value)
        {
            std::fprintf(stderr, "error: %s requires a value\n", a.c_str());
            return 2;
        }
    }

    if (inputs.empty())
    {
        std::fputs(kUsage, stderr);
        return 2;
    }

    // A pattern that doesn't compile is reported now. Nothing has been written yet.
    if (!only.empty())
    {
        try
        {
            o.only = std::regex(only);
            o.only_text = only;
        }
        catch (const std::regex_error& e)
        {
            std::fprintf(stderr, "error: --only: %s\n", e.what());
            return 2;
        }
    }

    if (o.jobs == 0)
    {
        o.jobs = std::max(1u, std::thread::hardware_concurrency());
    }

    return 0;
}

// Rips one input. The output folders take their registries of file names from `registries`. Returns the exit code.
int RipInput(const std::string& input, const Options& o, std::map<std::string, hsf::rip::FileNames>& registries)
{
    const std::string head = ReadHead(input);
    if (head.starts_with("CSAR") || head.starts_with("RSAR"))
    {
        std::fprintf(stderr, "error: %s is a %s sound archive; %s\n", input.c_str(),
                     head.starts_with("CSAR") ? "3DS" : "Wii", kSwitchArchives);
        return 1;
    }
    if (hsf::rip::IsWiiUArchive(std::span(reinterpret_cast<const uint8_t*>(head.data()), head.size())))
    {
        std::fprintf(stderr, "error: %s is a Wii U sound archive (big-endian FSAR); %s\n", input.c_str(),
                     kSwitchArchives);
        return 1;
    }
    if (const auto kind = hsf::game::EncryptedKind(InputPath(input)))
    {
        std::fprintf(stderr,
                     "error: %s is %s. Such a file holds a game's files encrypted.\n"
                     "       hsfrip reads a game's files once they're extracted: the game's RomFS folder,\n"
                     "       or the archives and sound archives in it.\n",
                     input.c_str(), kind->c_str());
        return 1;
    }

    const fs::path out_dir = o.out_dir.empty() ? DefaultOutputDir(input) : fs::path(o.out_dir);
    if (head.starts_with("FSAR"))
    {
        return RipLooseArchive(input, out_dir, o, NamesIn(registries, out_dir));
    }

    return RipGame(input, out_dir, o, registries);
}

int Run(int argc, char** argv)
{
    Options o;
    std::vector<std::string> inputs;
    if (const int parsed = ParseArgs(argc, argv, o, inputs))
    {
        return parsed < 0 ? 0 : parsed;
    }

    // Check all paths before writing anything. Then a typo, or an output folder given without -o, stops the command at
    // once.
    for (std::size_t i = 0; i < inputs.size(); i++)
    {
        std::error_code ec;
        if (!fs::exists(inputs[i], ec))
        {
            std::fprintf(stderr, "error: %s: no such file or folder%s\n", inputs[i].c_str(),
                         i ? " (-o names the output folder)" : "");
            return 1;
        }
    }

    // Rip each input once. Errors don't stop the loop. Return the highest exit code.
    std::map<std::string, hsf::rip::FileNames> registries;
    std::set<fs::path> done;
    int result = 0;
    for (std::size_t i = 0; i < inputs.size(); i++)
    {
        if (inputs.size() > 1)
        {
            std::fprintf(stderr, "%s%s:\n", i ? "\n" : "", InputPath(inputs[i]).filename().string().c_str());
        }

        std::error_code ec;
        const fs::path canonical = fs::weakly_canonical(inputs[i], ec);
        if (!done.insert(ec ? InputPath(inputs[i]) : canonical).second)
        {
            std::fprintf(stderr, "skipped duplicate input\n");
            continue;
        }

        try
        {
            result = std::max(result, RipInput(inputs[i], o, registries));
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "error: %s: %s\n", inputs[i].c_str(), e.what());
            result = std::max(result, 1);
        }
    }

    return result;
}

} // namespace

int main(int argc, char** argv)
{
    hsf::SetUpConsole();

    // Report unhandled errors, including allocation failures.
    int result = 1;
    try
    {
        result = Run(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
    }

    hsf::KeepConsoleOpen();

    return result;
}
