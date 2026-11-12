// SPDX-License-Identifier: MIT

// hsfrip: builds HSF sets (.hsflib and .minihsf files) from Switch games and sound archives.
//
// usage: hsfrip [options] <input> [<input> ...]
//   <input>          a Switch game's files or a sound archive (.bfsar, or the Wii's .brsar that Skyward Sword HD
//                    plays). The game's files are a folder (its RomFS, once it's extracted) or a Bezel Engine archive
//                    (.bea). An NSP, XCI or NCA holds them encrypted, and is refused.
//   -o, --output DIR the folder the rips go in (default: a folder next to each input, with the input's name and "_hsf"
//                    added)
//   --list           list each game's sound archives, or what an archive holds, then stop
//   --archive PATH   rip only this sound archive of a game (its path among the game's files)
//   --only REGEX     rip only sounds whose label matches (ECMAScript regex)
//   --bgm            rip only the music (ntdlib::rip::IsMusic): sequences whose labels say BGM, or that play on a
//                    music player. For a JAudio game, the sounds that it plays as music.
//   --no-length      don't analyze lengths (no length/fade tags, and only the sequences of a .bfsar at a volume of 0
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
// or for a loose archive in its folder and the folders below it. While a group is still missing, the ripper also reads
// the group files in the SARC archives in the sound archive's folder (ntdlib::rip::SarcsBeside). Sequences whose data
// isn't all there are listed and skipped. So are those that make no sound: those whose volume in the archive is 0, and
// those that the length analysis finds never sound. The inputs are ripped one after another. An input that fails
// doesn't stop the rest. Dropping files or folders on the program gives it them as inputs. A game without a Switch
// sound archive or JAudio's files may hold Wii sound archives (.brsar). Skyward Sword HD does. Each one is ripped in
// the same way, with a library of the part of the archive that its sequences play.

#include "cli/hsfrip.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/rip/archive.h"
#include "ntdlib/rip/archive_mode.h"
#include "xsflib/binary.h"
#include "xsflib/cli/rip.h"
#include "xsflib/files.h"
#include "xsflib/rip/tool.h"

#include "cli/archive_rip.h"
#include "cli/game_settings.h"
#include "cli/jaudio_games.h"
#include "cli/rsar_rip.h"
#include "game/game.h"
#include "hsf/format.h"
#include "rip/game_paths.h"
#include "rip/jac_rip.h"
#include "rip/jaudio1_rip.h"
#include "rip/jaudio_rip.h"
#include "rip/nw4r_rip.h"
#include "rip/rip.h"

namespace hsf::cli
{

namespace
{

namespace fs = std::filesystem;

using xsflib::rip::InputPath;

constexpr char kUsage[] =
    "usage: hsfrip [options] <input> [<input> ...]\n"
    "\n"
    "Rips each input into an .hsflib and a .minihsf per sound. The rip goes in a folder next to the\n"
    "input, with the input's name and \"_hsf\" added. An input is a Switch game's files or a sound\n"
    "archive (.bfsar, or the .brsar of Skyward Sword HD). The game's files are a folder (its RomFS,\n"
    "once it's extracted) or a Bezel Engine archive (.bea). An NSP, XCI or NCA file holds a game's\n"
    "files encrypted, and hsfrip doesn't read it. Files and folders can also be dropped on the\n"
    "program. Sequences that make no sound, and those whose data isn't all there, are listed and\n"
    "skipped.\n"
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
    "                       finds the sequences that make no sound. Without it, only the\n"
    "                       sequences of a .bfsar at a volume of 0 are skipped.\n"
    "  --voices N           the sound library's voice count, 1 to 96 (default: what a Bezel\n"
    "                       Engine game's boot.lua gives, or else 96)\n"
    "  --no-effects         leave out the effects a Bezel Engine game puts on the aux buses\n"
    "  --jobs N             the number of length analyses that run at once (default: the number\n"
    "                       of CPU threads)\n"
    "  --game NAME, --artist NAME, --year YEAR, --copyright TEXT, --by NAME\n"
    "                       tags (the game defaults to the input's name)\n"
    "  --version            show the version\n"
    "  -h, --help           show this help\n";

// What every sound archive of `game` is ripped with, but its files: the settings, the game's levels, and the finders of
// its other archives and files. The finders keep a reference to `game`.
ArchiveToRip GameArchiveToRip(const hsf::game::GameFiles& game, const hsf::ArchiveDescriptor& settings,
                              const Options& o)
{
    ArchiveToRip common;
    common.settings = settings;
    common.levels = &game.sound_levels;
    common.game_code = o.effects;
    common.find_archive = [&game](std::string_view name) -> std::optional<FileChunk>
    {
        for (const std::string& path : game.sound_archives)
        {
            const fs::path file = xsflib::PathFromUtf8(path);
            FileChunk chunk;
            chunk.path = path;
            if (xsflib::Utf8(file.stem()) == name && game.read_file(path, chunk.data))
            {
                return chunk;
            }
        }

        return std::nullopt;
    };

    common.find_file = [&game](std::string_view path, std::vector<uint8_t>& data)
    {
        const auto named = [path](const std::string& file)
        {
            return hsf::rip::NamesFile(file, path);
        };
        const auto found = std::find_if(game.files.begin(), game.files.end(), named);

        return found != game.files.end() && game.read_file(*found, data);
    };

    return common;
}

// Rips a game without FSAR archives whose sound HSF plays another way: JAudio by RipJaudioGame, Pikmin 2's JAudio1
// by RipJaudio1Game, Pikmin's JAudio by RipJacGame, and Wii sound archives by RipRsarGame. Returns the exit code, or
// nothing for a game with none of these.
std::optional<int> RipOlderSoundSystem(const std::string& input, const hsf::game::GameFiles& game,
                                       const fs::path& out_dir, const Options& o,
                                       xsflib::cli::FileNameRegistries& registries)
{
    if (const std::optional<hsf::rip::JaudioPaths> jaudio = hsf::rip::FindJaudioFiles(game.files, game.read_file))
    {
        return RipJaudioGame(input, game, *jaudio, out_dir, o, registries);
    }

    if (const std::optional<hsf::rip::Jaudio1Paths> jaudio1 = hsf::rip::FindJaudio1Files(game.files, game.read_file))
    {
        return RipJaudio1Game(input, game, *jaudio1, out_dir, o, registries);
    }

    if (const std::optional<hsf::rip::JacPaths> jac = hsf::rip::FindJacFiles(game.files))
    {
        return RipJacGame(input, game, *jac, out_dir, o, registries);
    }

    if (const std::vector<std::string> rsars = hsf::rip::FindRsarFiles(game.files); !rsars.empty())
    {
        return RipRsarGame(input, game, rsars, out_dir, o, registries);
    }

    return std::nullopt;
}

// Rips a game's sound archives (or the one --archive names). Each archive's folder takes its registry of file names
// from `registries`. A game without FSAR archives goes to RipOlderSoundSystem. Returns the exit code.
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
        if (const std::optional<int> exit_code = RipOlderSoundSystem(input, game, out_dir, o, registries))
        {
            return *exit_code;
        }
    }

    const auto& archives = game.sound_archives;
    hsf::ArchiveDescriptor settings;
    try
    {
        settings = GameSettings(game, o);
    }
    catch (const xsflib::FormatError& e)
    {
        std::fprintf(stderr, "error: %s: %s\n", input.c_str(), e.what());
        return 1;
    }

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

    if (RefuseMissingArchive(archives, o))
    {
        return 1;
    }

    std::fprintf(stderr, "%zu sound archive%s; %u voices%s; %s%s\n", archives.size(), archives.size() == 1 ? "" : "s",
                 settings.voice_count, VoicesFromBoot(game, o) ? " (boot.lua)" : "",
                 EffectsText(GameEffects(game, o), "no effects on the aux buses").c_str(),
                 ProceduresText(settings).c_str());
    if (game.sound_levels.Count() > 0)
    {
        std::fprintf(stderr, "the game's sound sheets give levels for %zu sounds\n", game.sound_levels.Count());
    }

    // A group file anywhere among the game's files. A file holds the group when the file's name is the group's file
    // name in any case.
    const auto find_group = [&game](const std::string& name)
    {
        return ntdlib::rip::FindGroupFile<FileChunk>(game.files, game.read_file, name);
    };

    // A game can keep the group files of a sound archive in the SARC archives in the folder of the sound archive, under
    // other names. Super Mario 3D World + Bowser's Fury keeps those of `SoundData/SeData.bfsar` in
    // `SoundData/SeResource*.sarc`.
    const auto find_sarcs = [&game](const std::string& archive_path)
    {
        return ntdlib::rip::SarcsBeside(game.files, archive_path);
    };

    const std::string game_name = o.game_name.empty() ? game.name : o.game_name;

    // Another console's sound archive among the game's files isn't ripped. The game's sound archives are all FSAR
    // files. So only a Wii U archive can be one.
    const auto switch_archive = [](const std::string& path, const std::vector<uint8_t>& bytes)
    {
        return !RefuseForeignArchive(path, bytes);
    };

    return ntdlib::rip::RipArchives(ArchiveTool(), input, out_dir, archives, GameArchiveToRip(game, settings, o),
                                    game.read_file, find_group, game_name, o, registries, switch_archive, find_sarcs);
}

// Rips a loose sound archive into `out_dir`. `names` is the registry of file names for `out_dir`. Returns the exit
// code.
int RipLooseArchive(const std::string& input, const fs::path& out_dir, const Options& o, hsf::rip::FileNames& names)
{
    ArchiveToRip rip;
    rip.settings = OptionSettings(o);

    // A loose archive's add-ons lie beside it, with the same extension.
    const fs::path loose = InputPath(input);
    rip.find_archive = [loose](std::string_view name) -> std::optional<FileChunk>
    {
        const fs::path path =
            loose.parent_path() / xsflib::PathFromUtf8(std::string(name) + xsflib::Utf8(loose.extension()));
        FileChunk chunk;
        chunk.path = xsflib::Utf8(path.filename());
        if (!xsflib::ReadFile(path, chunk.data))
        {
            return std::nullopt;
        }

        return chunk;
    };

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
    const std::span<const uint8_t> head_bytes(reinterpret_cast<const uint8_t*>(head.data()), head.size());
    if (!hsf::rip::IsRsar(head_bytes) && RefuseForeignArchive(input, head_bytes))
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

    // A loose Wii sound archive plays as Skyward Sword HD's does. The library names it by its file name.
    if (hsf::rip::IsRsar(head_bytes))
    {
        const fs::path loose = InputPath(input);
        FileChunk rsar;
        rsar.path = xsflib::Utf8(loose.filename());
        if (!xsflib::ReadFile(loose, rsar.data))
        {
            std::fprintf(stderr, "error: can't read %s\n", input.c_str());
            return 1;
        }

        const std::string game_name = o.game_name.empty() ? xsflib::Utf8(loose.stem()) : o.game_name;
        return RipRsar(std::move(rsar), game_name, out_dir, o, registries);
    }

    return RipGame(input, out_dir, o, registries);
}

} // namespace

} // namespace hsf::cli

int main(int argc, char** argv)
{
    xsflib::cli::RipTool<hsf::cli::Options> tool;
    tool.name = "hsfrip";
    tool.version = HSF_VERSION;
    tool.usage = hsf::cli::kUsage;
    tool.modes = false;
    tool.parse_option = hsf::cli::ParseOption;
    tool.rip_input = hsf::cli::RipInput;

    return xsflib::cli::RunRipTool(tool, argc, argv);
}