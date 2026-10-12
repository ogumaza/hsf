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
//                    music player
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
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rip/archive.h"
#include "ntdlib/rip/archive_mode.h"
#include "xsflib/ascii.h"
#include "xsflib/cli/rip.h"
#include "xsflib/rip/tool.h"

#include "game/game.h"
#include "hsf/format.h"
#include "hsf/sar.h"
#include "rip/rip.h"

namespace
{

namespace fs = std::filesystem;

using hsf::FileChunk;
using ntdlib::rip::Job;
using xsflib::rip::InputPath;

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
    hsf::atk::EngineOptions options;
    options.voice_count = static_cast<int>(settings.voice_count);
    const hsf::rip::LengthAnalyzer analyzer(std::move(archive), options, settings.effects);
    xsflib::cli::AnalyzeLengths(jobs, threads,
                                [&analyzer](Job& job)
                                {
                                    job.timing = analyzer.Analyze(job.index);
                                    return true;
                                });
}

// hsfrip's archive mode (ntdlib::rip::ArchiveRipTool).
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

        return t;
    }();

    return kTool;
}

// What a sound archive holds (such as "FSAR 2.2.0, 2911 sequences, 28 wave sounds, 220 streams"), or why it can't be
// read or ripped.
std::string DescribeArchive(std::vector<uint8_t> bytes)
{
    if (hsf::rip::IsWiiUArchive(bytes))
    {
        return "a Wii U sound archive (big-endian FSAR). HSF doesn't rip those";
    }

    return ntdlib::rip::DescribeArchive(std::move(bytes), hsf::kSarOptions);
}

// Whether the voice count comes from the game's boot.lua: --voices isn't given, and boot.lua gives a count from 1 to
// 96.
bool VoicesFromBoot(const hsf::game::GameFiles& game, const Options& o)
{
    return o.voices == 0 && game.voice_count && *game.voice_count >= 1 && *game.voice_count <= 96;
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
    else if (VoicesFromBoot(game, o))
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
    const std::string file = xsflib::AsciiLower(name + ".bfgrp");
    for (const std::string& path : game.files)
    {
        const std::size_t slash = path.rfind('/');
        const std::string file_name = slash == std::string::npos ? path : path.substr(slash + 1);
        if (xsflib::AsciiLower(file_name) != file)
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
// from `registries`. Returns the exit code.
int RipGame(const std::string& input, const fs::path& out_dir, const Options& o,
            xsflib::cli::FileNameRegistries& registries)
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
                    VoicesFromBoot(game, o) ? " (boot.lua)" : "", settings.effects.empty() ? "none" : kDefaultReverb);
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
                 archives.size() == 1 ? "" : "s", settings.voice_count, VoicesFromBoot(game, o) ? " (boot.lua)" : "",
                 settings.effects.empty() ? "no reverb" : kDefaultReverb);

    const auto find_group = [&game](const std::string& name)
    {
        return FindGameGroup(game, name);
    };

    const std::string game_name = o.game_name.empty() ? game.name : o.game_name;

    // A Wii U sound archive among the game's files isn't ripped.
    const auto switch_archive = [](const std::string& path, const std::vector<uint8_t>& bytes)
    {
        if (hsf::rip::IsWiiUArchive(bytes))
        {
            std::fprintf(stderr, "error: %s is a Wii U sound archive (big-endian FSAR); %s\n", path.c_str(),
                         kSwitchArchives);
            return false;
        }

        return true;
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
    if (o.voices > 0)
    {
        rip.settings.voice_count = static_cast<uint32_t>(o.voices);
    }

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
        if (v.empty() || *end || n < 1 || n > 96)
        {
            std::fprintf(stderr, "error: --voices takes a number from 1 to 96, not '%s'\n", v.c_str());
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
