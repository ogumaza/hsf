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
//   --bgm            rip only the music (ntdlib::rip::IsMusic): sequences whose labels say BGM, or that play on a
//                    music player. For a JAudio game, the sounds that it plays as music.
//   --no-length      don't analyze lengths (no length/fade tags, and only a sound archive's sequences at a volume of 0
//                    are found to make no sound)
//   --voices N       nn::atk's voice count (default: a Bezel Engine game's voice count, from its boot.lua, or else 96)
//   --no-effects     leave out the effects a Bezel Engine game puts on the aux buses
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
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/jaudio/barc.h"
#include "ntdlib/jaudio/bx.h"
#include "ntdlib/nx/nso.h"
#include "ntdlib/rip/archive.h"
#include "ntdlib/rip/archive_mode.h"
#include "xsflib/cli/rip.h"
#include "xsflib/rip/tool.h"

#include "game/game.h"
#include "hsf/format.h"
#include "hsf/jac.h"
#include "hsf/jaudio.h"
#include "hsf/jaudio1.h"
#include "hsf/sar.h"
#include "jaudio/engine.h"
#include "rip/jac_rip.h"
#include "rip/jaudio1_rip.h"
#include "rip/jaudio_rip.h"
#include "rip/rip.h"

namespace
{

namespace fs = std::filesystem;

using hsf::FileChunk;
using ntdlib::rip::Job;
using xsflib::rip::InputPath;

// The most voices a set can have. It's also nn::atk's voice count when the game doesn't change it.
constexpr int kMaxVoices = static_cast<int>(hsf::ArchiveDescriptor::kDefaultVoices);

// The most draws from the sequences' generator that the Pikmin 2 ripper tries before a sound that never sounds.
constexpr uint32_t kSearchDraws = 16;

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
    "                       a player named for music (BGM, MUSIC, SONG or FANFARE). For a game\n"
    "                       that plays its sound with JAudio, the sounds that it plays as music.\n"
    "  --no-length          skip length analysis and omit length and fade tags. The analysis also\n"
    "                       finds the sequences that make no sound. Without it, only a sound\n"
    "                       archive's sequences at a volume of 0 are skipped.\n"
    "  --voices N           the sound library's voice count, 1 to 96 (default: what a Bezel\n"
    "                       Engine game's boot.lua gives, or else 96)\n"
    "  --no-effects         leave out the effects a Bezel Engine game puts on the aux buses\n"
    "  --jobs N             the number of length analyses that run at once (default: the number\n"
    "                       of CPU threads)\n"
    "  --game NAME, --artist NAME, --year YEAR, --copyright TEXT, --by NAME\n"
    "                       tags (the game defaults to the input's name)\n"
    "  --version            show the version\n"
    "  -h, --help           show this help\n";

// The rip frame's options, and HSF's.
struct Options : xsflib::cli::RipOptions
{
    bool effects = true;
    int voices = 0; // 0: the game's voice count, or 96
};

// What a sound archive is ripped with: the archive's file and the group files found for it, by their paths among the
// game's files (or beside a loose archive), and the descriptor's settings. The length analysis plays with the same
// settings.
struct ArchiveToRip
{
    FileChunk archive;
    std::vector<FileChunk> groups;
    hsf::ArchiveDescriptor settings;
};

// Length analysis runs every selected sequence through the model. Because the sounds are independent, the rip frame
// spreads the analyses over several threads.
void AnalyzeLengths(std::vector<Job>& jobs, hsf::fsar::SoundArchive archive, const hsf::ArchiveDescriptor& settings,
                    unsigned threads)
{
    const hsf::rip::LengthAnalyzer analyzer(std::move(archive), settings);
    const auto analyze = [&analyzer](Job& job)
    {
        job.timing = analyzer.Analyze(job.index);
        return true;
    };
    xsflib::cli::AnalyzeLengths(jobs, threads, analyze);
}

// hsfrip's archive mode (ntdlib::rip::ArchiveRipTool). A library holds only the files that its sequences play.
const ntdlib::rip::ArchiveRipTool<hsf::rip::RipFormat, ArchiveToRip>& ArchiveTool()
{
    static const auto kTool = []
    {
        ntdlib::rip::ArchiveRipTool<hsf::rip::RipFormat, ArchiveToRip> t;
        t.format = "hsf";
        t.mini_word = "minihsf";
        t.library_extension = ".hsflib";
        t.mini_extension = ".minihsf";
        t.sar_options = hsf::kSarOptions;
        t.analyze_lengths = AnalyzeLengths;
        t.keeps_sequence_data = [](const ArchiveToRip&)
        {
            return true;
        };

        return t;
    }();

    return kTool;
}

// What another console's sound archive is, from the file's first bytes `head`: "a 3DS sound archive", "a Wii sound
// archive" or "a Wii U sound archive (big-endian FSAR)" (see rip::IsWiiUArchive). Nothing for a Switch sound archive or
// any other file.
std::optional<std::string> ForeignArchive(std::span<const uint8_t> head)
{
    const std::string_view text(reinterpret_cast<const char*>(head.data()), head.size());
    if (text.starts_with("CSAR"))
    {
        return "a 3DS sound archive";
    }
    if (text.starts_with("RSAR"))
    {
        return "a Wii sound archive";
    }
    if (hsf::rip::IsWiiUArchive(head))
    {
        return "a Wii U sound archive (big-endian FSAR)";
    }

    return std::nullopt;
}

// Reports the error when the file at `path` is another console's sound archive. `head` holds the file's first bytes.
// Returns whether the file is one.
bool RefuseForeignArchive(const std::string& path, std::span<const uint8_t> head)
{
    const std::optional<std::string> foreign = ForeignArchive(head);
    if (foreign)
    {
        std::fprintf(stderr, "error: %s is %s; HSF rips the Switch's sound archives (little-endian FSAR, .bfsar)\n",
                     path.c_str(), foreign->c_str());
    }

    return foreign.has_value();
}

// What a sound archive holds (such as "FSAR 2.2.0, 2911 sequences, 28 wave sounds, 220 streams"), or why it can't be
// read or ripped.
std::string DescribeArchive(std::vector<uint8_t> bytes)
{
    if (const std::optional<std::string> foreign = ForeignArchive(bytes))
    {
        return *foreign + ". HSF doesn't rip those";
    }

    return ntdlib::rip::DescribeArchive(std::move(bytes), hsf::kSarOptions);
}

// The settings that the options give every sound archive: the voice count of --voices, or else nn::atk's default.
hsf::ArchiveDescriptor OptionSettings(const Options& o)
{
    hsf::ArchiveDescriptor settings;
    if (o.voices > 0)
    {
        settings.voice_count = static_cast<uint32_t>(o.voices);
    }

    return settings;
}

// Whether the voice count comes from the game's boot.lua: --voices isn't given, and boot.lua gives a count from 1 to
// 96.
bool VoicesFromBoot(const hsf::game::GameFiles& game, const Options& o)
{
    return o.voices == 0 && game.voice_count && *game.voice_count >= 1 && *game.voice_count <= kMaxVoices;
}

// The effects that a Bezel Engine game's code puts on the aux buses (hsf::game::StartupEffects), or none with
// --no-effects.
std::vector<hsf::game::BusEffect> GameEffects(const hsf::game::GameFiles& game, const Options& o)
{
    return o.effects ? hsf::game::StartupEffects(game) : std::vector<hsf::game::BusEffect>{};
}

// The effects as hsfrip reports them, such as "I3DL2_Default on aux bus A", or `none` without any.
std::string EffectsText(const std::vector<hsf::game::BusEffect>& effects, const std::string& none)
{
    std::string text;
    for (const hsf::game::BusEffect& e : effects)
    {
        text += (text.empty() ? "" : ", ") + e.name + " on aux bus " + static_cast<char>('A' + e.bus);
    }

    return text.empty() ? none : text;
}

// The settings every sound archive of a game is ripped with: the options' settings, the voice count of the game's
// boot.lua when --voices isn't given, and the effects a Bezel Engine game puts on the aux buses.
hsf::ArchiveDescriptor GameSettings(const hsf::game::GameFiles& game, const Options& o)
{
    hsf::ArchiveDescriptor settings = OptionSettings(o);
    if (VoicesFromBoot(game, o))
    {
        settings.voice_count = static_cast<uint32_t>(*game.voice_count);
    }

    for (hsf::game::BusEffect& e : GameEffects(game, o))
    {
        hsf::AuxEffect effect;
        effect.bus = e.bus;
        effect.type = e.type;
        effect.parameters = std::move(e.parameters);
        settings.effects.push_back(std::move(effect));
    }

    return settings;
}

// The group file `name` anywhere among a game's files, or nothing. A file holds the group when the file's name is the
// group's file name in any case (ntdlib::rip::IsGroupFileName).
std::optional<FileChunk> FindGameGroup(const hsf::game::GameFiles& game, const std::string& name)
{
    for (const std::string& path : game.files)
    {
        const std::size_t slash = path.rfind('/');
        const std::string file_name = slash == std::string::npos ? path : path.substr(slash + 1);
        if (!ntdlib::rip::IsGroupFileName(file_name, name))
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

// Prints --list for a game: its sound archives, what each one holds, and the settings that a rip gives them.
void ListGame(const std::string& input, const hsf::game::GameFiles& game, const hsf::ArchiveDescriptor& settings,
              const Options& o)
{
    const std::vector<std::string>& archives = game.sound_archives;
    std::printf("%s: %zu files, %zu sound archive%s\n", input.c_str(), game.files.size(), archives.size(),
                archives.size() == 1 ? "" : "s");
    ntdlib::rip::ListArchives(archives, game.read_file, DescribeArchive);
    std::printf("voices: %u%s; effects: %s\n", settings.voice_count, VoicesFromBoot(game, o) ? " (boot.lua)" : "",
                EffectsText(GameEffects(game, o), "none").c_str());
}

// The wave archives of a JAudio game, read from the game's files as the start-up archive's wave systems name them. Each
// is read once. An archive that the game lacks stays missing.
class JaudioWaves
{
public:
    JaudioWaves(const hsf::game::GameFiles& game, std::string folder) : game_(game), folder_(std::move(folder))
    {
    }

    const std::vector<uint8_t>* operator()(const std::string& name)
    {
        auto [chunk, added] = chunks_.try_emplace(name);
        if (added)
        {
            chunk->second.path = hsf::JaudioWavePath(folder_, name);
            if (!game_.read_file(chunk->second.path, chunk->second.data))
            {
                chunks_.erase(chunk);
                return nullptr;
            }
        }

        return &chunk->second.data;
    }

    // The archive `name` if a call has read it, or null. Find reads no file. Several threads can call it at once.
    const std::vector<uint8_t>* Find(const std::string& name) const
    {
        const auto chunk = chunks_.find(name);
        return chunk == chunks_.end() ? nullptr : &chunk->second.data;
    }

    // The archives read, in the order of their names.
    std::vector<FileChunk> Chunks() const
    {
        std::vector<FileChunk> chunks;
        for (const auto& [name, chunk] : chunks_)
        {
            chunks.push_back(chunk);
        }

        return chunks;
    }

private:
    const hsf::game::GameFiles& game_;
    std::string folder_;
    std::map<std::string, FileChunk> chunks_;
};

// The exit code when a JAudio game's selection holds no sound. An --only or --bgm that matches nothing is an error.
// Otherwise the game has nothing to rip. `source` is the start-up archive or the bank file.
int NoJaudioSounds(const std::string& source, const Options& o)
{
    if (!o.only_text.empty() || o.bgm)
    {
        return ntdlib::rip::NoSoundMatches(o.only_text, o.bgm);
    }

    std::fprintf(stderr, "%s: nothing to rip\n", source.c_str());
    return 0;
}

// The sounds that a JAudio game's length analysis skips: those that play notes on banks that the files don't load, and
// those that make no sound.
struct Skipped
{
    std::size_t missing = 0;
    std::size_t silent = 0;
};

// Drops the sounds that play notes on banks that the start-up archive doesn't load, and those that make no sound, with
// a line for each. Returns how many of each it dropped.
Skipped DropUnplayable(std::vector<hsf::rip::JaudioJob>& jobs)
{
    Skipped skipped;
    std::vector<hsf::rip::JaudioJob> kept;
    for (hsf::rip::JaudioJob& job : jobs)
    {
        if (job.missing_bank_notes != 0)
        {
            skipped.missing++;
            std::fprintf(stderr, "skipped %s: it plays notes on banks that the start-up archive doesn't load\n",
                         job.label.c_str());
        }
        else if (job.timing && job.timing->silent)
        {
            skipped.silent++;
            std::fprintf(stderr, "skipped %s: it makes no sound\n", job.label.c_str());
        }
        else
        {
            kept.push_back(std::move(job));
        }
    }
    jobs = std::move(kept);

    return skipped;
}

// Writes a JAudio game's rip into the folder of `out_dir` named after `startup`, the start-up archive or the bank file:
// a library of `startup`, `sequences` and `files` with `descriptor`, and a mini for each of `jobs`. The messages name
// `startup`'s path, and the last ends with `skipped_text`. Returns the exit code.
int WriteJaudioRip(const FileChunk& startup, const std::optional<FileChunk>& sequences,
                   const std::vector<FileChunk>& files, const hsf::ArchiveDescriptor& descriptor,
                   const std::vector<hsf::rip::JaudioJob>& jobs, const std::string& skipped_text,
                   const hsf::game::GameFiles& game, const fs::path& out_dir, const Options& o,
                   xsflib::cli::FileNameRegistries& registries)
{
    const char* source = startup.path.c_str();
    if (jobs.empty())
    {
        std::fprintf(stderr, "%s: nothing to rip%s\n", source, skipped_text.c_str());
        return 0;
    }

    const std::string game_name = o.game_name.empty() ? game.name : o.game_name;
    const std::string folder = xsflib::rip::ArchiveFolders({startup.path}).front();
    const fs::path rip_dir = out_dir / folder;
    hsf::rip::FileNames& names = xsflib::cli::NamesIn(registries, rip_dir, ".hsflib", ".minihsf");
    const xsflib::psf::Tags common = xsflib::cli::RipTags(o, game_name, "hsf", nullptr);
    const std::vector<uint8_t> lib = hsf::rip::BuildJaudioLibrary(startup, sequences, files, descriptor, common);
    const std::string lib_name = names.LibraryIn(rip_dir, game_name + " " + folder, lib);
    fs::create_directories(rip_dir);
    if (!xsflib::rip::WriteFile(rip_dir / xsflib::PathFromUtf8(lib_name), lib))
    {
        std::fprintf(stderr, "error: can't write %s\n", lib_name.c_str());
        return 1;
    }
    std::fprintf(stderr, "wrote %s (%zu bytes)\n", lib_name.c_str(), lib.size());

    const auto build_mini = [&lib_name](const hsf::rip::JaudioJob& job, const xsflib::psf::Tags& tags)
    {
        return hsf::rip::BuildJaudioMini(job.id, lib_name, tags);
    };
    const int written = xsflib::cli::WriteMinis(rip_dir, names, jobs, common, "hsf_sound", build_mini);
    if (written < 0)
    {
        return 1;
    }

    std::fprintf(stderr, "%s: %d minihsf files%s\n", source, written, skipped_text.c_str());
    xsflib::rip::ReportOutput(out_dir);

    return 0;
}

// Rips a game that plays its sound with JAudio: one library of its sound files, and a mini for each sound that plays
// a sequence. The settings come from the game's code when HSF holds them for the game. The folder of the rip has the
// start-up archive's name. Returns the exit code.
int RipJaudioGame(const std::string& input, const hsf::game::GameFiles& game, const hsf::rip::JaudioPaths& paths,
                  const fs::path& out_dir, const Options& o, xsflib::cli::FileNameRegistries& registries)
{
    FileChunk startup;
    startup.path = paths.startup;
    std::optional<FileChunk> sequences;
    if (!game.read_file(paths.startup, startup.data))
    {
        std::fprintf(stderr, "error: can't read %s\n", paths.startup.c_str());
        return 1;
    }
    if (!paths.sequences.empty())
    {
        sequences.emplace();
        sequences->path = paths.sequences;
        if (!game.read_file(paths.sequences, sequences->data))
        {
            std::fprintf(stderr, "error: can't read %s\n", paths.sequences.c_str());
            return 1;
        }
    }

    const hsf::rip::JaudioGame* known = hsf::rip::FindJaudioGame(startup.data);
    hsf::JaudioSettings settings = known ? known->settings : hsf::JaudioSettings{};
    JaudioWaves waves(game, paths.waves);
    std::optional<hsf::jaudio::SoundFiles> files;
    try
    {
        const auto wave = [&waves](const std::string& name)
        {
            return waves(name);
        };
        files.emplace(startup.data, sequences ? std::span<const uint8_t>(sequences->data) : std::span<const uint8_t>(),
                      wave);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s: %s\n", paths.startup.c_str(), e.what());
        return 1;
    }

    const std::vector<ntdlib::jaudio::SoundEntry>& sounds = files->Archive().Sounds();
    const auto count = [&sounds](auto test)
    {
        return std::count_if(sounds.begin(), sounds.end(), test);
    };
    const std::string summary =
        std::to_string(count([](const auto& s) { return !s.stream && s.section == 1; })) + " music sequences, " +
        std::to_string(count([](const auto& s) { return !s.stream && s.section != 1; })) + " other sequences, " +
        std::to_string(count([](const auto& s) { return s.stream; })) + " streams";
    const std::string profile =
        known ? known->name + "'s settings" : "JAudio2's defaults (HSF has no settings for the game)";
    if (o.list)
    {
        std::printf("%s: JAudio: start-up archive %s (%zu bytes), %s; %s\n", input.c_str(), paths.startup.c_str(),
                    startup.data.size(), summary.c_str(), profile.c_str());
        return 0;
    }

    std::fprintf(stderr, "JAudio: %s, %s; %s\n", paths.startup.c_str(), summary.c_str(), profile.c_str());
    if (!known)
    {
        std::fprintf(
            stderr,
            "warning: HSF has no settings for this game's code. The rips take JAudio2's defaults for the levels "
            "and effects that a game's code sets.\n");
    }
    for (const std::string& name : files->MissingWaves())
    {
        std::fprintf(stderr, "warning: the wave archive %s isn't among the game's files; its waves stay silent\n",
                     name.c_str());
    }

    // The volumes that the game's code writes into the sound table from a file of the game.
    if (known && !known->bgm_params.empty())
    {
        const std::size_t slash = paths.startup.rfind('/');
        const std::string path =
            (slash == std::string::npos ? std::string() : paths.startup.substr(0, slash + 1)) + known->bgm_params;
        std::vector<uint8_t> table;
        std::string error;
        if (!game.read_file(path, table))
        {
            error = "it isn't among the game's files";
        }
        else
        {
            try
            {
                settings.sound_volumes = hsf::rip::ReadBgmVolumes(table, files->Archive());
            }
            catch (const std::exception& e)
            {
                error = e.what();
            }
        }
        if (!error.empty())
        {
            std::fprintf(stderr,
                         "warning: can't read the music's volumes from %s: %s. The music plays at the sound table's "
                         "volumes.\n",
                         path.c_str(), error.c_str());
        }
    }

    std::vector<hsf::rip::JaudioJob> jobs = hsf::rip::SelectJaudioSounds(*files, known, o.only, o.bgm);
    if (jobs.empty())
    {
        return NoJaudioSounds(paths.startup, o);
    }

    hsf::ArchiveDescriptor descriptor = hsf::ArchiveDescriptor::Jaudio(settings);
    descriptor.archive_path = startup.path;
    descriptor.group_paths = {sequences ? sequences->path : std::string(), paths.waves};

    // The sounds that play notes on banks that the files don't load stop the game. Their data isn't all there. The
    // length analysis finds them and the sounds that never sound.
    Skipped skipped;
    if (o.lengths)
    {
        const hsf::rip::JaudioLengthAnalyzer analyzer(*files, descriptor);
        const auto analyze = [&analyzer](hsf::rip::JaudioJob& job)
        {
            const hsf::rip::JaudioTiming result = analyzer.Analyze(job.id);
            job.timing = result.timing;
            job.missing_bank_notes = result.missing_bank_notes;
            return true;
        };
        xsflib::cli::AnalyzeLengths(jobs, o.jobs, analyze);
        skipped = DropUnplayable(jobs);
    }

    const std::string skipped_text =
        xsflib::cli::SkippedText(skipped.missing, "banks not loaded", skipped.silent, "sequence");
    return WriteJaudioRip(startup, sequences, waves.Chunks(), descriptor, jobs, skipped_text, game, out_dir, o,
                          registries);
}

// Rips Pikmin 2, whose Switch release plays its sound with JAudio1: one library of its sound files with the conductor
// archive, and a mini for each piece of music, each conductor of the cave music and each effect. The settings come
// from the game's code and from its list of its music and its track maps. The folder of the rip has the start-up
// archive's name. Returns the exit code.
int RipJaudio1Game(const std::string& input, const hsf::game::GameFiles& game, const hsf::rip::Jaudio1Paths& paths,
                   const fs::path& out_dir, const Options& o, xsflib::cli::FileNameRegistries& registries)
{
    FileChunk startup;
    startup.path = paths.startup;
    FileChunk sequences;
    sequences.path = paths.sequences;
    FileChunk conductors;
    conductors.path = paths.conductors;
    for (FileChunk* chunk : {&startup, &sequences, &conductors})
    {
        if (!game.read_file(chunk->path, chunk->data))
        {
            std::fprintf(stderr, "error: can't read %s\n", chunk->path.c_str());
            return 1;
        }
    }

    hsf::rip::Jaudio1GameText text;
    if (const std::optional<std::string> error = hsf::rip::ReadJaudio1GameText(paths, game.read_file, text))
    {
        std::fprintf(stderr, "error: %s\n", error->c_str());
        return 1;
    }

    hsf::ArchiveDescriptor descriptor = hsf::ArchiveDescriptor::Jaudio(hsf::rip::Jaudio1Settings());
    descriptor.archive_path = startup.path;
    descriptor.group_paths = {sequences.path, paths.waves};
    descriptor.jaudio.library2.conductor_archive = conductors.path;

    JaudioWaves waves(game, paths.waves);
    std::optional<hsf::Jaudio1Files> files;
    try
    {
        const std::string wave_prefix = paths.waves + "/";
        const auto file = [&](const std::string& path) -> const std::vector<uint8_t>*
        {
            for (const FileChunk* chunk : {&startup, &sequences, &conductors})
            {
                if (path == chunk->path)
                {
                    return &chunk->data;
                }
            }

            return path.starts_with(wave_prefix) ? waves(path.substr(wave_prefix.size())) : nullptr;
        };
        if (const std::optional<std::string> error = hsf::LoadJaudio1Files(descriptor, file, files))
        {
            std::fprintf(stderr, "error: %s\n", error->c_str());
            return 1;
        }
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s: %s\n", paths.startup.c_str(), e.what());
        return 1;
    }

    hsf::rip::Jaudio1Selection selection = hsf::rip::SelectJaudio1Sounds(*files, text, o.only, o.bgm);
    selection.library.conductor_archive = conductors.path;
    descriptor.jaudio.library2 = selection.library;
    const std::size_t pieces = selection.library.pieces.size();
    const std::string summary = std::to_string(pieces) + " pieces of music and " +
                                std::to_string(selection.jobs.size() - pieces) + " sound effects";
    if (o.list)
    {
        std::printf("%s: JAudio1: start-up archive %s (%zu bytes), %s; Pikmin 2's settings\n", input.c_str(),
                    paths.startup.c_str(), startup.data.size(), summary.c_str());
        return 0;
    }

    std::fprintf(stderr, "JAudio1: %s, %s; Pikmin 2's settings\n", paths.startup.c_str(), summary.c_str());
    for (const std::string& warning : selection.warnings)
    {
        std::fprintf(stderr, "warning: %s\n", warning.c_str());
    }
    for (const std::string& name : files->sound.MissingWaves())
    {
        std::fprintf(stderr, "warning: the wave archive %s isn't among the game's files; its waves stay silent\n",
                     name.c_str());
    }

    std::vector<hsf::rip::JaudioJob> jobs = std::move(selection.jobs);
    if (jobs.empty())
    {
        return NoJaudioSounds(paths.startup, o);
    }

    // The sounds that play notes on banks that the files don't load, and those that never sound, are left out.
    Skipped skipped;
    if (o.lengths)
    {
        const hsf::rip::Jaudio1LengthAnalyzer analyzer(*files, descriptor);
        const auto analyze = [&analyzer](hsf::rip::JaudioJob& job)
        {
            const hsf::rip::JaudioTiming result = analyzer.Analyze(job.id);
            job.timing = result.timing;
            job.missing_bank_notes = result.missing_bank_notes;
            job.sequence_draws = result.sequence_draws;
            return true;
        };
        xsflib::cli::AnalyzeLengths(jobs, o.jobs, analyze);

        // A sound that never sounds may take another branch where its sequence draws a number at random. The game's
        // generator can be in another state as the sound starts. The rip plays the first state after 1 to
        // kSearchDraws draws in which the sound sounds, and the descriptor lists that sound's draws.
        for (hsf::rip::JaudioJob& job : jobs)
        {
            if (job.missing_bank_notes != 0 || !job.timing || !job.timing->silent || job.sequence_draws == 0)
            {
                continue;
            }

            for (uint32_t draws = 1; draws <= kSearchDraws; draws++)
            {
                const hsf::rip::JaudioTiming result = analyzer.Analyze(job.id, draws);
                if (result.missing_bank_notes == 0 && result.timing && !result.timing->silent)
                {
                    job.timing = result.timing;
                    descriptor.jaudio.library2.draws.push_back({job.id, draws});
                    break;
                }
            }
        }

        skipped = DropUnplayable(jobs);
    }

    const std::string skipped_text =
        xsflib::cli::SkippedText(skipped.missing, "banks not loaded", skipped.silent, "sound");
    std::vector<FileChunk> files_in_library = waves.Chunks();
    files_in_library.push_back(conductors);
    return WriteJaudioRip(startup, sequences, files_in_library, descriptor, jobs, skipped_text, game, out_dir, o,
                          registries);
}

// Rips Pikmin, whose sound plays with the C JAudio: one library of the game's sound files and of the sequence index
// from its main, and a mini for each piece of music, each piece of the cutscenes' music and each effect. The settings
// come from the game's main, the build that hsfrip reads. The folder of the rip has the bank file's name. Returns the
// exit code.
int RipJacGame(const std::string& input, const hsf::game::GameFiles& game, const hsf::rip::JacPaths& paths,
               const fs::path& out_dir, const Options& o, xsflib::cli::FileNameRegistries& registries)
{
    if (paths.main.empty())
    {
        std::fprintf(stderr,
                     "error: %s: Pikmin's sound settings are in the game's code, the ExeFS's main, and the input\n"
                     "       lacks it. Give hsfrip a folder that holds both the game's RomFS and its ExeFS, as\n"
                     "       romfs/ and exefs/.\n",
                     input.c_str());
        return 1;
    }

    FileChunk bank;
    bank.path = paths.bank;
    FileChunk sequences;
    sequences.path = paths.sequences;
    std::vector<uint8_t> main;
    for (FileChunk* chunk : {&bank, &sequences})
    {
        if (!game.read_file(chunk->path, chunk->data))
        {
            std::fprintf(stderr, "error: can't read %s\n", chunk->path.c_str());
            return 1;
        }
    }
    if (!game.read_file(paths.main, main))
    {
        std::fprintf(stderr, "error: can't read %s\n", paths.main.c_str());
        return 1;
    }

    const std::string build = hsf::rip::NsoBuildId(main);
    if (build != hsf::rip::kJacMainBuildId)
    {
        const std::string other = build.empty() ? std::string("no NSO's header") : "build ID " + build;
        std::fprintf(stderr,
                     "error: %s isn't the main that hsfrip reads Pikmin's settings from: version %s's, build ID %s.\n"
                     "       It has %s.\n",
                     paths.main.c_str(), std::string(hsf::rip::kJacMainVersion).c_str(),
                     std::string(hsf::rip::kJacMainBuildId).c_str(), other.c_str());
        return 1;
    }

    hsf::rip::JacGameCode code;
    std::vector<ntdlib::jaudio::ArchiveFile> files;
    std::vector<ntdlib::jaudio::WaveSystem> systems;
    try
    {
        code = hsf::rip::ReadJacGameCode(ntdlib::nx::ReadNso(main));
        files = ntdlib::jaudio::ReadBarcSequenceFiles(code.sequence_index, sequences.data, false);
        systems = ntdlib::jaudio::ReadBxWaveSystems(bank.data);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s: %s\n", paths.bank.c_str(), e.what());
        return 1;
    }

    // The sequence index goes in a FILE chunk whose path no file of the game can have.
    FileChunk index;
    index.path = hsf::rip::kJacSequenceIndexPath;
    index.data = code.sequence_index;

    hsf::rip::JacSelection selection = hsf::rip::SelectJacSounds(code, files, systems, o.only, o.bgm);
    selection.library.sequence_index = index.path;
    hsf::ArchiveDescriptor descriptor = hsf::ArchiveDescriptor::Jaudio(hsf::rip::JacSettings(code));
    descriptor.archive_path = bank.path;
    descriptor.group_paths = {sequences.path, paths.waves};
    descriptor.jaudio.library3 = selection.library;
    const std::size_t pieces = selection.library.pieces.size();
    const std::size_t parts = selection.library.parts.size();
    const std::string summary = std::to_string(pieces) + " pieces of music, " + std::to_string(parts) +
                                " pieces of the cutscenes' music and " +
                                std::to_string(selection.jobs.size() - pieces - parts) + " sound effects";
    if (o.list)
    {
        std::printf("%s: Jac: bank file %s (%zu bytes), %s; Pikmin's settings from main version %s\n", input.c_str(),
                    paths.bank.c_str(), bank.data.size(), summary.c_str(),
                    std::string(hsf::rip::kJacMainVersion).c_str());
        return 0;
    }

    std::fprintf(stderr, "Jac: %s, %s; Pikmin's settings from main version %s\n", paths.bank.c_str(), summary.c_str(),
                 std::string(hsf::rip::kJacMainVersion).c_str());
    for (const std::string& warning : selection.warnings)
    {
        std::fprintf(stderr, "warning: %s\n", warning.c_str());
    }

    // The library holds every wave archive that a wave system names.
    JaudioWaves waves(game, paths.waves);
    std::vector<std::string> missing_waves;
    for (const ntdlib::jaudio::WaveSystem& system : systems)
    {
        for (const ntdlib::jaudio::WaveGroup& group : system.groups)
        {
            if (!group.archive.empty() && !waves(group.archive) &&
                std::find(missing_waves.begin(), missing_waves.end(), group.archive) == missing_waves.end())
            {
                missing_waves.push_back(group.archive);
                std::fprintf(stderr,
                             "warning: the wave archive %s isn't among the game's files; its waves stay silent\n",
                             group.archive.c_str());
            }
        }
    }

    std::vector<hsf::rip::JaudioJob> jobs = std::move(selection.jobs);
    if (jobs.empty())
    {
        return NoJaudioSounds(paths.bank, o);
    }

    // The sounds that never sound are left out. The analyses run on several threads, and each reads the wave archives
    // as the game's code loads its scenes. They find the archives that the loop above read.
    std::size_t silent = 0;
    if (o.lengths)
    {
        std::optional<hsf::jac::SoundFiles> sound;
        const std::string wave_prefix = paths.waves + "/";
        const auto file = [&](const std::string& path) -> const std::vector<uint8_t>*
        {
            for (const FileChunk* chunk : {&bank, &sequences, &index})
            {
                if (path == chunk->path)
                {
                    return &chunk->data;
                }
            }

            return path.starts_with(wave_prefix) ? waves.Find(path.substr(wave_prefix.size())) : nullptr;
        };
        try
        {
            if (const std::optional<std::string> error = hsf::LoadJacFiles(descriptor, file, sound))
            {
                std::fprintf(stderr, "error: %s\n", error->c_str());
                return 1;
            }
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "error: %s: %s\n", paths.bank.c_str(), e.what());
            return 1;
        }

        const hsf::rip::JacLengthAnalyzer analyzer(*sound, descriptor);
        const auto analyze = [&analyzer](hsf::rip::JaudioJob& job)
        {
            const hsf::rip::JaudioTiming result = analyzer.Analyze(job.id);
            job.timing = result.timing;
            job.missing_bank_notes = result.missing_bank_notes;
            return true;
        };
        xsflib::cli::AnalyzeLengths(jobs, o.jobs, analyze);

        std::vector<hsf::rip::JaudioJob> kept;
        for (hsf::rip::JaudioJob& job : jobs)
        {
            if (job.timing && job.timing->silent)
            {
                silent++;
                std::fprintf(stderr, "skipped %s: it makes no sound\n", job.label.c_str());
                continue;
            }

            if (job.missing_bank_notes != 0)
            {
                std::fprintf(stderr,
                             "warning: %s plays notes that get no wave from the bank file; they stay silent, as in "
                             "the game\n",
                             job.label.c_str());
            }
            kept.push_back(std::move(job));
        }
        jobs = std::move(kept);
    }

    const std::string skipped_text = xsflib::cli::SkippedText(0, "", silent, "sound");
    std::vector<FileChunk> files_in_library = waves.Chunks();
    files_in_library.push_back(index);
    return WriteJaudioRip(bank, sequences, files_in_library, descriptor, jobs, skipped_text, game, out_dir, o,
                          registries);
}

// Rips a game's sound archives (or the one --archive names). Each archive's folder takes its registry of file names
// from `registries`. A game that plays its sound with JAudio is ripped by RipJaudioGame, Pikmin 2 by RipJaudio1Game
// and Pikmin by RipJacGame. Returns the exit code.
int RipGame(const std::string& input, const fs::path& out_dir, const Options& o,
            xsflib::cli::FileNameRegistries& registries)
{
    hsf::game::GameFiles game;
    if (const auto error = hsf::game::LoadGame(InputPath(input), game))
    {
        std::fprintf(stderr, "error: %s: %s\n", input.c_str(), error->c_str());
        return 1;
    }

    if (game.sound_archives.empty())
    {
        if (const std::optional<hsf::rip::JaudioPaths> jaudio = hsf::rip::FindJaudioFiles(game.files, game.read_file))
        {
            return RipJaudioGame(input, game, *jaudio, out_dir, o, registries);
        }

        if (const std::optional<hsf::rip::Jaudio1Paths> jaudio1 =
                hsf::rip::FindJaudio1Files(game.files, game.read_file))
        {
            return RipJaudio1Game(input, game, *jaudio1, out_dir, o, registries);
        }

        if (const std::optional<hsf::rip::JacPaths> jac = hsf::rip::FindJacFiles(game.files))
        {
            return RipJacGame(input, game, *jac, out_dir, o, registries);
        }
    }

    const auto& archives = game.sound_archives;
    const hsf::ArchiveDescriptor settings = GameSettings(game, o);
    if (o.list)
    {
        ListGame(input, game, settings, o);
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

    std::fprintf(stderr, "%zu sound archive%s; %u voices%s; %s\n", archives.size(), archives.size() == 1 ? "" : "s",
                 settings.voice_count, VoicesFromBoot(game, o) ? " (boot.lua)" : "",
                 EffectsText(GameEffects(game, o), "no effects on the aux buses").c_str());

    const auto find_group = [&game](const std::string& name)
    {
        return FindGameGroup(game, name);
    };

    const std::string game_name = o.game_name.empty() ? game.name : o.game_name;

    // Another console's sound archive among the game's files isn't ripped. The game's sound archives are all FSAR
    // files. So only a Wii U archive can be one.
    const auto switch_archive = [](const std::string& path, const std::vector<uint8_t>& bytes)
    {
        return !RefuseForeignArchive(path, bytes);
    };

    ArchiveToRip common;
    common.settings = settings;

    return ntdlib::rip::RipArchives(ArchiveTool(), input, out_dir, archives, common, game.read_file, find_group,
                                    game_name, o, registries, switch_archive);
}

// Rips a loose sound archive into `out_dir`. `names` is the registry of file names for `out_dir`. Returns the exit
// code.
int RipLooseArchive(const std::string& input, const fs::path& out_dir, const Options& o, hsf::rip::FileNames& names)
{
    ArchiveToRip rip;
    rip.settings = OptionSettings(o);

    return ntdlib::rip::RipLooseArchive(ArchiveTool(), input, out_dir, o, names, std::move(rip), DescribeArchive);
}

// Takes the options that the rip frame leaves to HSF: --voices and --no-effects. Returns -1 for another option, 0 when
// it took the option, or 2 after an error.
int ParseOption(const std::string& a, const std::function<std::string()>& next, Options& o)
{
    if (a == "--no-effects")
    {
        o.effects = false;
        return 0;
    }

    if (a == "--voices")
    {
        const std::string v = next();
        char* end = nullptr;
        const long n = std::strtol(v.c_str(), &end, 10);
        if (v.empty() || *end || n < 1 || n > kMaxVoices)
        {
            std::fprintf(stderr, "error: --voices takes a number from 1 to %d, not '%s'\n", kMaxVoices, v.c_str());
            return 2;
        }

        o.voices = static_cast<int>(n);
        return 0;
    }

    return -1;
}

// Rips one input. The output folders take their registries of file names from `registries`. Returns the exit code.
int RipInput(const std::string& input, const Options& o, xsflib::cli::FileNameRegistries& registries)
{
    const std::string head = xsflib::rip::ReadHead(input);
    if (RefuseForeignArchive(input, std::span(reinterpret_cast<const uint8_t*>(head.data()), head.size())))
    {
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

    const fs::path out_dir = o.out_dir.empty() ? xsflib::rip::DefaultOutputDir(input, "_hsf") : fs::path(o.out_dir);
    if (head.starts_with("FSAR"))
    {
        return RipLooseArchive(input, out_dir, o, xsflib::cli::NamesIn(registries, out_dir, ".hsflib", ".minihsf"));
    }

    return RipGame(input, out_dir, o, registries);
}

} // namespace

int main(int argc, char** argv)
{
    xsflib::cli::RipTool<Options> tool;
    tool.name = "hsfrip";
    tool.version = HSF_VERSION;
    tool.usage = kUsage;
    tool.modes = false;
    tool.parse_option = ParseOption;
    tool.rip_input = RipInput;

    return xsflib::cli::RunRipTool(tool, argc, argv);
}
