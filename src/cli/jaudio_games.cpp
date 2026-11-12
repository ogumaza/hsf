// SPDX-License-Identifier: MIT

// hsfrip's rips of the games that play their sound with JAudio (see jaudio_games.h).

#include "cli/jaudio_games.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/jaudio/barc.h"
#include "ntdlib/jaudio/bx.h"
#include "ntdlib/nx/nso.h"
#include "xsflib/cli/rip.h"
#include "xsflib/psf/psf.h"

#include "game/game.h"
#include "hsf/format.h"
#include "hsf/jac.h"
#include "hsf/jaudio.h"
#include "hsf/jaudio1.h"
#include "hsf/jaudio_models.h"
#include "rip/game_paths.h"
#include "rip/jac_rip.h"
#include "rip/jaudio1_rip.h"
#include "rip/jaudio_length.h"
#include "rip/jaudio_rip.h"
#include "rip/rip.h"

namespace hsf::cli
{

namespace
{

namespace fs = std::filesystem;

// The most draws from the sequences' generator that the Pikmin 2 ripper tries before a sound that never sounds.
constexpr uint32_t kSearchDraws = 16;

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

// Warns that the wave archive `name` is missing from the game's files.
void WarnMissingWaveArchive(const std::string& name)
{
    std::fprintf(stderr, "warning: the wave archive %s isn't among the game's files; its waves stay silent\n",
                 name.c_str());
}

// Reads each of `chunks` from the game's file at its path. Reports the first file that can't be read, and returns
// false for it.
bool ReadChunks(const hsf::game::GameFiles& game, std::initializer_list<FileChunk*> chunks)
{
    for (FileChunk* chunk : chunks)
    {
        if (!game.read_file(chunk->path, chunk->data))
        {
            std::fprintf(stderr, "error: can't read %s\n", chunk->path.c_str());
            return false;
        }
    }

    return true;
}

// The loader of a game's files by their paths, for hsf::LoadJaudio1Files and hsf::LoadJacFiles: the data of one of
// `chunks`, or of the wave archive in `waves_folder` that `wave(name)` gives, or null.
template <class Wave>
auto ChunkOrWave(std::vector<const FileChunk*> chunks, const std::string& waves_folder, Wave wave)
{
    return [chunks = std::move(chunks), prefix = waves_folder + "/", wave](const std::string& path)
    {
        for (const FileChunk* chunk : chunks)
        {
            if (path == chunk->path)
            {
                return &chunk->data;
            }
        }

        return path.starts_with(prefix) ? wave(path.substr(prefix.size())) : nullptr;
    };
}

// What a JAudio2 game's sound table holds, such as "120 music sequences, 900 other sequences, 30 streams".
std::string JaudioSummary(const std::vector<ntdlib::jaudio::SoundEntry>& sounds)
{
    const auto count = [&sounds](auto test)
    {
        return std::count_if(sounds.begin(), sounds.end(), test);
    };

    return std::to_string(count([](const auto& s) { return !s.stream && s.section == 1; })) + " music sequences, " +
           std::to_string(count([](const auto& s) { return !s.stream && s.section != 1; })) + " other sequences, " +
           std::to_string(count([](const auto& s) { return s.stream; })) + " streams";
}

// Sets the volumes that the game's code writes into the sound table from `known`'s file of the music's parameters,
// beside the start-up archive at `startup_path`. Without the file, the music plays at the sound table's volumes, with
// a warning.
void ReadMusicVolumes(const hsf::game::GameFiles& game, const std::string& startup_path,
                      const hsf::rip::JaudioGame& known, const hsf::jaudio::SoundFiles& files,
                      hsf::JaudioSettings& settings)
{
    const std::string path = hsf::rip::InFolder(hsf::rip::FolderOf(startup_path), known.bgm_params);
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
            settings.sound_volumes = hsf::rip::ReadBgmVolumes(table, files.Archive());
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

// The build of Pikmin's main `main` that hsfrip reads the settings from, or null with an error that lists the builds
// it reads.
const hsf::rip::JacBuild* JacBuildOf(const FileChunk& main)
{
    const std::string build_id = hsf::rip::NsoBuildId(main.data);
    const hsf::rip::JacBuild* build = hsf::rip::FindJacBuild(build_id);
    if (build == nullptr)
    {
        std::string known;
        for (const hsf::rip::JacBuild& known_build : hsf::rip::kJacBuilds)
        {
            known += "\n       version " + std::string(known_build.version) + ", build ID " +
                     std::string(known_build.build_id);
        }

        const std::string other = build_id.empty() ? std::string("no NSO's header") : "build ID " + build_id;
        std::fprintf(stderr,
                     "error: %s isn't a main that hsfrip reads Pikmin's settings from. It reads these:%s\n"
                     "       It has %s.\n",
                     main.path.c_str(), known.c_str(), other.c_str());
    }

    return build;
}

// Reads into `waves` every wave archive that `systems` name. Warns once for each archive that the game lacks.
void ReadWaveSystems(const std::vector<ntdlib::jaudio::WaveSystem>& systems, JaudioWaves& waves)
{
    std::vector<std::string> missing;
    for (const ntdlib::jaudio::WaveSystem& system : systems)
    {
        for (const ntdlib::jaudio::WaveGroup& group : system.groups)
        {
            if (!group.archive.empty() && !waves(group.archive) &&
                std::find(missing.begin(), missing.end(), group.archive) == missing.end())
            {
                missing.push_back(group.archive);
                WarnMissingWaveArchive(group.archive);
            }
        }
    }
}

// Gives draws to the sounds of Pikmin 2 that never sound. Such a sound may take another branch where its sequence draws
// a number at random. The game's generator can be in another state as the sound starts. The rip plays the first state
// after 1 to kSearchDraws draws in which the sound sounds, and `draws` lists that sound's draws. A variant shares its
// piece's draws.
void SearchDraws(std::vector<hsf::rip::JaudioJob>& jobs, const hsf::rip::Jaudio1LengthAnalyzer& analyzer,
                 std::vector<hsf::JaudioDraws>& draws)
{
    for (hsf::rip::JaudioJob& job : jobs)
    {
        if (hsf::rip::IsVariant(job) || job.missing_bank_notes != 0 || !job.timing || !job.timing->silent ||
            job.sequence_draws == 0)
        {
            continue;
        }

        for (uint32_t count = 1; count <= kSearchDraws; count++)
        {
            const hsf::rip::JaudioTiming result = analyzer.Analyze(job.id, count);
            if (result.missing_bank_notes == 0 && result.timing && !result.timing->silent)
            {
                job.timing = result.timing;
                draws.push_back({job.id, count});
                break;
            }
        }
    }
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

// Measures Pikmin 2's sounds on `threads` threads, with the draws that SearchDraws finds added to `descriptor`. Drops
// the sounds that can't play (DropUnplayable). A variant plays with its descriptor.
Skipped AnalyzeJaudio1Jobs(std::vector<hsf::rip::JaudioJob>& jobs, const hsf::Jaudio1Files& files,
                           hsf::ArchiveDescriptor& descriptor, unsigned threads)
{
    const hsf::rip::Jaudio1LengthAnalyzer analyzer(files, descriptor);
    const auto analyze = [&files, &descriptor, &analyzer](hsf::rip::JaudioJob& job)
    {
        const hsf::rip::JaudioTiming result =
            hsf::rip::IsVariant(job)
                ? hsf::rip::Jaudio1LengthAnalyzer(files, hsf::rip::JobDescriptor(descriptor, job)).Analyze(job.id)
                : analyzer.Analyze(job.id);
        job.timing = result.timing;
        job.missing_bank_notes = result.missing_bank_notes;
        job.sequence_draws = result.sequence_draws;
        return true;
    };
    xsflib::cli::AnalyzeLengths(jobs, threads, analyze);

    SearchDraws(jobs, analyzer, descriptor.jaudio.library2.draws);

    // The analysis measures a variant of a piece that the search gave draws to again, with the draws.
    const std::vector<hsf::JaudioDraws>& draws = descriptor.jaudio.library2.draws;
    for (hsf::rip::JaudioJob& job : jobs)
    {
        const auto same_sound = [&job](const hsf::JaudioDraws& entry)
        {
            return entry.sound == job.id;
        };
        if (hsf::rip::IsVariant(job) && std::any_of(draws.begin(), draws.end(), same_sound))
        {
            analyze(job);
        }
    }

    return DropUnplayable(jobs);
}

// Measures Pikmin's sounds on `threads` threads, with the game's files that `file` gives (hsf::LoadJacFiles), and drops
// those that make no sound. A sound that plays notes without a wave stays, with a warning. Returns how many sounds it
// dropped, or nothing after an error. The error names the bank file at `bank_path`.
template <class File>
std::optional<std::size_t> AnalyzeJacJobs(std::vector<hsf::rip::JaudioJob>& jobs,
                                          const hsf::ArchiveDescriptor& descriptor, const File& file,
                                          std::vector<std::vector<hsf::rip::JacLaterEvent>> later_events,
                                          const std::string& bank_path, unsigned threads)
{
    std::optional<hsf::jac::SoundFiles> sound;
    try
    {
        if (const std::optional<std::string> error = hsf::LoadJacFiles(descriptor, file, sound))
        {
            std::fprintf(stderr, "error: %s\n", error->c_str());
            return std::nullopt;
        }
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s: %s\n", bank_path.c_str(), e.what());
        return std::nullopt;
    }

    const hsf::rip::JacLengthAnalyzer analyzer(*sound, descriptor, std::move(later_events));
    const auto analyze = [&analyzer](hsf::rip::JaudioJob& job)
    {
        const hsf::rip::JaudioTiming result = analyzer.Analyze(job.id);
        job.timing = result.timing;
        job.missing_bank_notes = result.missing_bank_notes;
        return true;
    };
    xsflib::cli::AnalyzeLengths(jobs, threads, analyze);

    const auto warn_missing = [](const hsf::rip::JaudioJob& job)
    {
        if (job.missing_bank_notes != 0)
        {
            std::fprintf(stderr,
                         "warning: %s plays notes that get no wave from the bank file; they stay silent, as in the "
                         "game\n",
                         job.label.c_str());
        }
    };

    return DropSilent(jobs, warn_missing);
}

// Measures a JAudio2 game's sounds on `threads` threads, and drops the sounds that can't play (DropUnplayable). A
// variant plays with its descriptor.
Skipped AnalyzeJaudioJobs(std::vector<hsf::rip::JaudioJob>& jobs, const hsf::jaudio::SoundFiles& files,
                          const hsf::ArchiveDescriptor& descriptor, unsigned threads)
{
    const auto analyze = [&files, &descriptor](hsf::rip::JaudioJob& job)
    {
        const hsf::rip::JaudioLengthAnalyzer analyzer(files, hsf::rip::JobDescriptor(descriptor, job));
        const hsf::rip::JaudioTiming result = analyzer.Analyze(job.id);
        job.timing = result.timing;
        job.missing_bank_notes = result.missing_bank_notes;
        return true;
    };
    xsflib::cli::AnalyzeLengths(jobs, threads, analyze);

    return DropUnplayable(jobs);
}

// Prints each of `warnings` as a warning.
void PrintWarnings(const std::vector<std::string>& warnings)
{
    for (const std::string& warning : warnings)
    {
        std::fprintf(stderr, "warning: %s\n", warning.c_str());
    }
}

// What a rip of Pikmin 2 holds, such as "20 pieces of music, 5 variants of them and 900 sound effects".
std::string Jaudio1Summary(const hsf::rip::Jaudio1Selection& selection)
{
    const std::size_t pieces = selection.library.pieces.size();
    const auto variants =
        static_cast<std::size_t>(std::count_if(selection.jobs.begin(), selection.jobs.end(), hsf::rip::IsVariant));
    return std::to_string(pieces) + " pieces of music, " + std::to_string(variants) + " variants of them and " +
           std::to_string(selection.jobs.size() - pieces - variants) + " sound effects";
}

// What a rip of Pikmin holds, such as "20 pieces of music, 8 pieces of the cutscenes' music and 400 sound effects".
std::string JacSummary(const hsf::rip::JacSelection& selection)
{
    const std::size_t pieces = selection.library.pieces.size();
    const std::size_t parts = selection.library.parts.size();
    return std::to_string(pieces) + " pieces of music, " + std::to_string(parts) +
           " pieces of the cutscenes' music and " + std::to_string(selection.jobs.size() - pieces - parts) +
           " sound effects";
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
    if (jobs.empty())
    {
        std::fprintf(stderr, "%s: nothing to rip%s\n", startup.path.c_str(), skipped_text.c_str());
        return 0;
    }

    const std::string game_name = o.game_name.empty() ? game.name : o.game_name;
    const auto build_library = [&](const xsflib::psf::Tags& common)
    {
        return hsf::rip::BuildJaudioLibrary(startup, sequences, files, descriptor, common);
    };
    // The hsf_sound tag of a variant gives the label of the sound that it plays.
    const auto build_mini =
        [&descriptor](const hsf::rip::JaudioJob& job, const std::string& lib_name, xsflib::psf::Tags tags)
    {
        if (!job.sound_label.empty())
        {
            tags["hsf_sound"] = job.sound_label;
        }

        return hsf::rip::BuildJaudioMini(job, descriptor, lib_name, tags);
    };

    return WriteRip(startup.path, game_name, jobs, skipped_text, out_dir, o, registries, build_library, build_mini);
}

// What a rip of Pikmin reads from the game: the bank file, the sequence archive and the ExeFS's main, and the settings
// in main's code.
struct JacGame
{
    FileChunk bank;
    FileChunk sequences;
    FileChunk main;
    const hsf::rip::JacBuild* build = nullptr;
    hsf::rip::JacGameCode code;
    std::vector<ntdlib::jaudio::ArchiveFile> files;
    std::vector<ntdlib::jaudio::WaveSystem> systems;
};

// Reads Pikmin's files at `paths` from `game`. Nothing, with an error, if one can't be read. The error about a missing
// main names `input`.
std::optional<JacGame> ReadJacGame(const std::string& input, const hsf::game::GameFiles& game,
                                   const hsf::rip::JacPaths& paths)
{
    if (paths.main.empty())
    {
        std::fprintf(stderr,
                     "error: %s: Pikmin's sound settings are in the game's code, the ExeFS's main, and the input\n"
                     "       lacks it. Give hsfrip a folder that holds both the game's RomFS and its ExeFS, as\n"
                     "       romfs/ and exefs/.\n",
                     input.c_str());
        return std::nullopt;
    }

    JacGame jac;
    jac.bank.path = paths.bank;
    jac.sequences.path = paths.sequences;
    jac.main.path = paths.main;
    if (!ReadChunks(game, {&jac.bank, &jac.sequences, &jac.main}))
    {
        return std::nullopt;
    }

    jac.build = JacBuildOf(jac.main);
    if (jac.build == nullptr)
    {
        return std::nullopt;
    }

    // The file that each step reads, for the error message.
    const std::string* reading = &paths.main;
    try
    {
        jac.code = hsf::rip::ReadJacGameCode(ntdlib::nx::ReadNso(jac.main.data), jac.build->at);
        reading = &paths.sequences;
        jac.files = ntdlib::jaudio::ReadBarcSequenceFiles(jac.code.sequence_index, jac.sequences.data, false);
        reading = &paths.bank;
        jac.systems = ntdlib::jaudio::ReadBxWaveSystems(jac.bank.data);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s: %s\n", reading->c_str(), e.what());
        return std::nullopt;
    }

    return jac;
}

// Loads JAudio's sound files into `files`: the start-up archive `startup`, the sequence archive `sequences` if the game
// has one, and the wave archives that `waves` reads. False after an error message.
bool LoadJaudioFiles(const FileChunk& startup, const std::optional<FileChunk>& sequences, JaudioWaves& waves,
                     std::optional<hsf::jaudio::SoundFiles>& files)
{
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
        std::fprintf(stderr, "error: %s: %s\n", startup.path.c_str(), e.what());
        return false;
    }

    return true;
}

// Loads JAudio1's sound files into `files` for `descriptor`: the files of `chunks`, and the wave archives in the folder
// `waves_folder` that `waves` reads. False after an error message.
bool LoadJaudio1(const hsf::ArchiveDescriptor& descriptor, const std::vector<const FileChunk*>& chunks,
                 const std::string& waves_folder, JaudioWaves& waves, std::optional<hsf::Jaudio1Files>& files)
{
    try
    {
        const auto wave = [&waves](const std::string& name)
        {
            return waves(name);
        };
        const auto file = ChunkOrWave(chunks, waves_folder, wave);
        if (const std::optional<std::string> error = hsf::LoadJaudio1Files(descriptor, file, files))
        {
            std::fprintf(stderr, "error: %s\n", error->c_str());
            return false;
        }
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s: %s\n", chunks.front()->path.c_str(), e.what());
        return false;
    }

    return true;
}

} // namespace

int RipJaudioGame(const std::string& input, const hsf::game::GameFiles& game, const hsf::rip::JaudioPaths& paths,
                  const fs::path& out_dir, const Options& o, xsflib::cli::FileNameRegistries& registries)
{
    FileChunk startup;
    startup.path = paths.startup;
    std::optional<FileChunk> sequences;
    if (!paths.sequences.empty())
    {
        sequences.emplace();
        sequences->path = paths.sequences;
    }

    if (!ReadChunks(game, {&startup}) || (sequences && !ReadChunks(game, {&*sequences})))
    {
        return 1;
    }

    const hsf::rip::JaudioGame* known = hsf::rip::FindJaudioGame(startup.data);
    hsf::JaudioSettings settings = known ? known->settings : hsf::JaudioSettings{};
    JaudioWaves waves(game, paths.waves);
    std::optional<hsf::jaudio::SoundFiles> files;
    if (!LoadJaudioFiles(startup, sequences, waves, files))
    {
        return 1;
    }

    const std::string summary = JaudioSummary(files->Archive().Sounds());
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
        WarnMissingWaveArchive(name);
    }

    if (known && !known->bgm_params.empty())
    {
        ReadMusicVolumes(game, paths.startup, *known, *files, settings);
    }

    std::vector<hsf::rip::JaudioJob> jobs = hsf::rip::SelectJaudioSounds(*files, known, o.only, o.bgm);
    if (jobs.empty())
    {
        return NoSoundsSelected(paths.startup, o);
    }

    hsf::ArchiveDescriptor descriptor = hsf::ArchiveDescriptor::Jaudio(settings);
    descriptor.archive_path = startup.path;
    descriptor.group_paths = {sequences ? sequences->path : std::string(), paths.waves};

    // The sounds that play notes on banks that the files don't load stop the game. Their data isn't all there. The
    // length analysis finds them and the sounds that never sound.
    Skipped skipped;
    if (o.lengths)
    {
        skipped = AnalyzeJaudioJobs(jobs, *files, descriptor, o.jobs);
    }

    const std::string skipped_text =
        xsflib::cli::SkippedText(skipped.missing, "banks not loaded", skipped.silent, "sequence");
    return WriteJaudioRip(startup, sequences, waves.Chunks(), descriptor, jobs, skipped_text, game, out_dir, o,
                          registries);
}

int RipJaudio1Game(const std::string& input, const hsf::game::GameFiles& game, const hsf::rip::Jaudio1Paths& paths,
                   const fs::path& out_dir, const Options& o, xsflib::cli::FileNameRegistries& registries)
{
    FileChunk startup;
    startup.path = paths.startup;
    FileChunk sequences;
    sequences.path = paths.sequences;
    FileChunk conductors;
    conductors.path = paths.conductors;
    if (!ReadChunks(game, {&startup, &sequences, &conductors}))
    {
        return 1;
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
    if (!LoadJaudio1(descriptor, {&startup, &sequences, &conductors}, paths.waves, waves, files))
    {
        return 1;
    }

    hsf::rip::Jaudio1Selection selection = hsf::rip::SelectJaudio1Sounds(*files, text, o.only, o.bgm);
    selection.library.conductor_archive = conductors.path;
    descriptor.jaudio.library2 = selection.library;
    const std::string summary = Jaudio1Summary(selection);
    if (o.list)
    {
        std::printf("%s: JAudio1: start-up archive %s (%zu bytes), %s; Pikmin 2's settings\n", input.c_str(),
                    paths.startup.c_str(), startup.data.size(), summary.c_str());
        return 0;
    }

    std::fprintf(stderr, "JAudio1: %s, %s; Pikmin 2's settings\n", paths.startup.c_str(), summary.c_str());
    PrintWarnings(selection.warnings);
    for (const std::string& name : files->sound.MissingWaves())
    {
        WarnMissingWaveArchive(name);
    }

    std::vector<hsf::rip::JaudioJob> jobs = std::move(selection.jobs);
    if (jobs.empty())
    {
        return NoSoundsSelected(paths.startup, o);
    }

    // The sounds that play notes on banks that the files don't load, and those that never sound, are left out.
    Skipped skipped;
    if (o.lengths)
    {
        skipped = AnalyzeJaudio1Jobs(jobs, *files, descriptor, o.jobs);
    }

    const std::string skipped_text =
        xsflib::cli::SkippedText(skipped.missing, "banks not loaded", skipped.silent, "sound");
    std::vector<FileChunk> files_in_library = waves.Chunks();
    files_in_library.push_back(conductors);
    return WriteJaudioRip(startup, sequences, files_in_library, descriptor, jobs, skipped_text, game, out_dir, o,
                          registries);
}

int RipJacGame(const std::string& input, const hsf::game::GameFiles& game, const hsf::rip::JacPaths& paths,
               const fs::path& out_dir, const Options& o, xsflib::cli::FileNameRegistries& registries)
{
    const std::optional<JacGame> jac = ReadJacGame(input, game, paths);
    if (!jac)
    {
        return 1;
    }

    const FileChunk& bank = jac->bank;
    const FileChunk& sequences = jac->sequences;
    const hsf::rip::JacBuild* build = jac->build;
    const hsf::rip::JacGameCode& code = jac->code;

    // The sequence index goes in a FILE chunk whose path no file of the game can have.
    FileChunk index;
    index.path = hsf::rip::kJacSequenceIndexPath;
    index.data = code.sequence_index;

    hsf::rip::JacSelection selection = hsf::rip::SelectJacSounds(code, jac->files, jac->systems, o.only, o.bgm);
    selection.library.sequence_index = index.path;
    hsf::ArchiveDescriptor descriptor = hsf::ArchiveDescriptor::Jaudio(hsf::rip::JacSettings(code));
    descriptor.archive_path = bank.path;
    descriptor.group_paths = {sequences.path, paths.waves};
    descriptor.jaudio.library3 = selection.library;
    const std::string summary = JacSummary(selection);
    if (o.list)
    {
        std::printf("%s: Jac: bank file %s (%zu bytes), %s; Pikmin's settings from main version %s\n", input.c_str(),
                    paths.bank.c_str(), bank.data.size(), summary.c_str(), std::string(build->version).c_str());
        return 0;
    }

    std::fprintf(stderr, "Jac: %s, %s; Pikmin's settings from main version %s\n", paths.bank.c_str(), summary.c_str(),
                 std::string(build->version).c_str());
    PrintWarnings(selection.warnings);

    // The library holds every wave archive that a wave system names.
    JaudioWaves waves(game, paths.waves);
    ReadWaveSystems(jac->systems, waves);

    std::vector<hsf::rip::JaudioJob> jobs = std::move(selection.jobs);
    if (jobs.empty())
    {
        return NoSoundsSelected(paths.bank, o);
    }

    // The sounds that never sound are left out. The analyses run on several threads, and each reads the wave archives
    // as the game's code loads its scenes. They find the archives that the loop above read.
    std::size_t silent = 0;
    if (o.lengths)
    {
        const auto wave = [&waves](const std::string& name)
        {
            return waves.Find(name);
        };
        const auto file = ChunkOrWave({&bank, &sequences, &index}, paths.waves, wave);
        const std::optional<std::size_t> dropped =
            AnalyzeJacJobs(jobs, descriptor, file, std::move(selection.later_events), paths.bank, o.jobs);
        if (!dropped)
        {
            return 1;
        }

        silent = *dropped;
    }

    const std::string skipped_text = xsflib::cli::SkippedText(0, "", silent, "sound");
    std::vector<FileChunk> files_in_library = waves.Chunks();
    files_in_library.push_back(index);
    return WriteJaudioRip(bank, sequences, files_in_library, descriptor, jobs, skipped_text, game, out_dir, o,
                          registries);
}

} // namespace hsf::cli
