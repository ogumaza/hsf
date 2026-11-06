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
// or for a loose archive in its folder and the folders below it. While a group is still missing, the ripper also reads
// the group files in the SARC archives in the sound archive's folder (hsf::game::SarcsBeside). Sequences whose data
// isn't all there are listed and skipped. So are those that make no sound: those whose volume in the archive is 0, and
// those that the length analysis finds never sound. The inputs are ripped one after another. An input that fails
// doesn't stop the rest. Dropping files or folders on the program gives it them as inputs.

#include <algorithm>
#include <array>
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
#include "ntdlib/nx/bex/music_sync.h"
#include "ntdlib/nx/nso.h"
#include "ntdlib/rip/archive.h"
#include "ntdlib/rip/archive_mode.h"
#include "xsflib/binary.h"
#include "xsflib/cli/rip.h"
#include "xsflib/files.h"
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
#include "rip/procedures.h"
#include "rip/rip.h"
#include "rip/sound_states.h"
#include "rip/variable_states.h"

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

// The latest start, in frames of 5 ms, that the nn::atk ripper tries for a sound that never sounds and whose sequence
// draws a number, and the frames after each start in which the sound has to sound (AnalyzeLengths). A branch that one
// draw in ten takes would have to be missed 400 times in a row.
constexpr uint32_t kStartDelays = 400;
constexpr uint32_t kStartWindow = 2000;

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
// settings. A Bezel Engine game's sound sheets give its sounds' levels. A loose archive has none.
struct ArchiveToRip
{
    FileChunk archive;
    std::vector<FileChunk> groups;
    hsf::ArchiveDescriptor settings;
    const ntdlib::bezel::SoundLevels* levels = nullptr;
    bool game_code = false; // true for a game's archive ripped with the game's effects and rules

    // Finds another of the game's sound archives by its file name without the extension, for a piece whose companion
    // it holds (rip/variable_states.h). Nothing when the game has no such archive.
    std::function<std::optional<FileChunk>(std::string_view name)> find_archive;

    // Reads one of the game's files by its path, for a piece that follows a stream's music sync (WithSyncedPieces). A
    // file in a Bezel Engine archive has its path in the archive. Returns false when the game has no such file, and
    // for a loose archive.
    std::function<bool(std::string_view path, std::vector<uint8_t>& data)> find_file;
};

// The waiting pieces of the archives that the rip has begun, by the archive's path. The rip cuts an archive down to the
// files that its sequences play before it builds the library and the minis. The table recognizes the whole archive.
// sound_states therefore keeps its pieces here.
std::map<std::string, std::span<const hsf::rip::WaitingPiece>>& KeptPieces()
{
    static std::map<std::string, std::span<const hsf::rip::WaitingPiece>> pieces;

    return pieces;
}

std::span<const hsf::rip::WaitingPiece> PiecesOf(const ArchiveToRip& to_rip)
{
    const auto found = KeptPieces().find(to_rip.archive.path);

    return found == KeptPieces().end() ? std::span<const hsf::rip::WaitingPiece>{} : found->second;
}

// A sound's index in `archive`'s sound table by its label, or nothing.
std::optional<uint32_t> SoundNamed(const hsf::fsar::SoundArchive& archive, std::string_view label)
{
    const auto& sounds = archive.Sounds();
    const auto found =
        std::find_if(sounds.begin(), sounds.end(), [label](const auto& sound) { return sound.name == label; });
    if (found == sounds.end())
    {
        return std::nullopt;
    }

    return static_cast<uint32_t>(found - sounds.begin());
}

// The add-on archive that holds a waiting piece's companion when the piece's archive doesn't: the game's archive that
// the table names, cut down to the files that the companion plays, and the companion's index in it.
struct CompanionAddon
{
    FileChunk archive;
    uint32_t companion = 0;
};

std::optional<CompanionAddon> CompanionAddonOf(const ArchiveToRip& to_rip, const hsf::rip::WaitingPiece& piece)
{
    if (piece.companion_archive.empty() || !to_rip.find_archive)
    {
        return std::nullopt;
    }

    std::optional<FileChunk> found = to_rip.find_archive(piece.companion_archive);
    if (!found)
    {
        return std::nullopt;
    }

    const hsf::fsar::SoundArchive archive = hsf::fsar::SoundArchive::Load(found->data, hsf::kSarOptions);
    const std::optional<uint32_t> companion = SoundNamed(archive, piece.companion);
    if (!companion)
    {
        return std::nullopt;
    }

    const std::array<uint32_t, 1> sounds = {*companion};
    std::optional<ntdlib::rip::LibraryArchive> kept = ntdlib::rip::KeepSequenceData(archive, sounds);
    if (kept && kept->group_files.empty())
    {
        found->data = std::move(kept->archive);
    }

    return CompanionAddon{std::move(*found), *companion};
}

// The add-on archives that the companions of an archive's waiting pieces need, each once.
std::vector<FileChunk> CompanionAddonsOf(const ArchiveToRip& to_rip)
{
    std::vector<FileChunk> addons;
    const hsf::fsar::SoundArchive archive = hsf::fsar::SoundArchive::Load(to_rip.archive.data, hsf::kSarOptions);
    for (const hsf::rip::WaitingPiece& piece : PiecesOf(to_rip))
    {
        if (piece.companion.empty() || SoundNamed(archive, piece.companion))
        {
            continue;
        }

        std::optional<CompanionAddon> addon = CompanionAddonOf(to_rip, piece);
        const auto same = [&addon](const FileChunk& chunk)
        {
            return addon && chunk.path == addon->archive.path;
        };
        if (addon && std::none_of(addons.begin(), addons.end(), same))
        {
            addons.push_back(std::move(addon->archive));
        }
    }

    return addons;
}

// A rip of a piece that follows a stream's music sync (ntdlib::nx::bex::SyncedPieces): its scripted start, and its
// length.
struct SyncedRip
{
    hsf::ScriptedStart script;
    hsf::rip::Timing timing;
};

// The synced rips of the archive that the rip has begun, by their jobs' labels. sound_states makes them for each
// archive. The length analysis and the minis then take them.
std::map<std::string, SyncedRip>& SyncedRips()
{
    static std::map<std::string, SyncedRip> rips;

    return rips;
}

// The fade of a synced rip: 10 s, in frames of 5 ms.
constexpr uint32_t kSyncedFadeFrames = 2000;

// The jobs that rip each synced piece of `to_rip`'s archive over the streams that it follows, in place of the piece's
// job. The game starts the piece while a board's stream plays, and the piece reads that stream's beat and chord
// through user procedures 2 to 4 (docs/hsf.md, "Ripping"). A stream's job has the piece's label, a space and the
// stream's label. Its script starts the piece at frame 0 with the stream, and gives procedures 2 to 4 their writes as
// writes of kind 2 (ntdlib::nx::bex::PlaySynced). It plays one pass over the stream's regions in the order of the jump
// setting, then the loop once more, and fades over 10 s while the loop plays again. The stream itself isn't part of
// the rip. A piece keeps its job when the game's files hold none of its streams. It then plays nothing, and the length
// analysis finds it silent.
std::vector<Job> WithSyncedPieces(const ArchiveToRip& to_rip, std::vector<Job> jobs)
{
    SyncedRips().clear();
    const std::span<const ntdlib::nx::bex::SyncedPiece> pieces = ntdlib::nx::bex::SyncedPieces(to_rip.archive.data);
    if (pieces.empty())
    {
        return jobs;
    }

    std::vector<Job> ripped;
    for (Job& job : jobs)
    {
        const auto piece = std::find_if(pieces.begin(), pieces.end(),
                                        [&job](const ntdlib::nx::bex::SyncedPiece& p) { return p.label == job.label; });
        if (piece == pieces.end())
        {
            ripped.push_back(std::move(job));
            continue;
        }

        const uint32_t sound = ntdlib::rip::kSoundItem << 24 | job.index;
        bool made = false;
        for (const ntdlib::nx::bex::SyncedStream& stream : piece->streams)
        {
            std::vector<uint8_t> stream_file;
            std::vector<uint8_t> jump_setting;
            if (!to_rip.find_file || !to_rip.find_file(stream.stream_path, stream_file) ||
                !to_rip.find_file(stream.jump_setting_path, jump_setting))
            {
                continue;
            }

            ntdlib::nx::bex::SyncedPlay play;
            try
            {
                play = ntdlib::nx::bex::PlaySynced(*piece, stream_file, jump_setting, kSyncedFadeFrames);
            }
            catch (const std::exception& e)
            {
                std::fprintf(stderr, "warning: %s: %s can't be read: %s\n", job.label.c_str(),
                             std::string(stream.label).c_str(), e.what());
                continue;
            }

            SyncedRip rip;
            rip.script = {sound, 0, {hsf::ScriptStep::Start(0, sound)}};
            for (const ntdlib::nx::bex::ProcedureWrite& write : play.writes)
            {
                rip.script.steps.push_back(hsf::ScriptStep::WriteInProcedure(
                    write.frame, static_cast<uint8_t>(write.procedure), write.variable, write.value, sound));
            }

            rip.timing = xsflib::rip::LoopAt(play.frames, 5.0, kSyncedFadeFrames * 5.0);

            Job synced = job;
            synced.label = job.label + " " + std::string(stream.label);
            SyncedRips()[synced.label] = std::move(rip);
            ripped.push_back(std::move(synced));
            made = true;
        }

        if (!made)
        {
            std::fprintf(stderr, "warning: %s: the game's files hold none of the streams that it follows\n",
                         job.label.c_str());
            ripped.push_back(std::move(job));
        }
    }

    return ripped;
}

// The scripted start of a job's sound, with the job's variables (Job::variables): at the job's start frame, the global
// variables, then the sound's start, then the sound's variables. The game's code sets the global variables before it
// starts a sound, and the sound's variables through the sound's handle. A job gives only those, 0 to 31. The lead-in
// runs to the start. The sound therefore starts as the output does.
hsf::ScriptedStart ScriptOf(const Job& job)
{
    const uint32_t sound = ntdlib::rip::kSoundItem << 24 | job.index;
    const uint32_t frame = job.start_frame;
    hsf::ScriptedStart script = {sound, frame, {}};
    for (const auto& [number, value] : job.variables)
    {
        if (number >= hsf::ScriptStep::kLocalEnd)
        {
            script.steps.push_back(hsf::ScriptStep::Write(frame, static_cast<uint16_t>(number), value));
        }
    }

    script.steps.push_back(hsf::ScriptStep::Start(frame, sound));
    for (const auto& [number, value] : job.variables)
    {
        if (number < hsf::ScriptStep::kLocalEnd)
        {
            script.steps.push_back(hsf::ScriptStep::Write(frame, static_cast<uint16_t>(number), value, sound));
        }
    }

    return script;
}

// Length analysis runs every selected sequence through the model. Because the sounds are independent, the rip frame
// spreads the analyses over several threads. A sound that never sounds may wait for a value that the game's code gives
// its variables. The analysis then tries the values that the archive's rules give (Job::start_states), and keeps the
// first with which the sound sounds. A sound may also take a branch with no sound on a number that its sequence draws
// at random. nn::atk's random numbers step once a frame, so the draw depends on when the game starts the sound. The
// analysis then looks for the first start, up to kStartDelays frames later, in which the sound sounds within
// kStartWindow frames. The job starts there (Job::start_frame). Its mini's descriptor gives the values or the start.
// A synced rip takes its length from its stream (WithSyncedPieces).
void AnalyzeLengths(std::vector<Job>& jobs, hsf::fsar::SoundArchive archive, const hsf::ArchiveDescriptor& settings,
                    unsigned threads)
{
    // A job with a reverb on aux bus B plays with it (rip/sound_states.h). An archive's rules hold at most a few
    // reverbs.
    std::map<const ntdlib::sar::ReverbPreset*, hsf::ArchiveDescriptor> with_reverb;
    for (const Job& job : jobs)
    {
        if (job.bus_b_reverb && !with_reverb.contains(job.bus_b_reverb))
        {
            with_reverb.emplace(job.bus_b_reverb, hsf::rip::WithBusBReverb(settings, job.bus_b_reverb));
        }
    }

    const hsf::rip::LengthAnalyzer analyzer(std::move(archive), settings);
    const auto analyze = [&analyzer, &with_reverb](Job& job)
    {
        if (const auto synced = SyncedRips().find(job.label); synced != SyncedRips().end())
        {
            job.timing = synced->second.timing;
            return true;
        }

        const auto reverb = with_reverb.find(job.bus_b_reverb);
        const hsf::ArchiveDescriptor* job_settings = reverb == with_reverb.end() ? nullptr : &reverb->second;
        hsf::ScriptedStart script = ScriptOf(job);
        job.timing = analyzer.Analyze(job.index, job.track_volumes, &script, job_settings, job.volume);
        if (!job.timing || !job.timing->silent)
        {
            return true;
        }

        // The states that the game's code can give the sound's variables (Job::start_states), the first in which the
        // sound sounds.
        for (const ntdlib::sar::VariableValues& state : job.start_states)
        {
            Job in_state = job;
            for (const auto& [number, value] : state)
            {
                in_state.variables[number] = value;
            }

            const hsf::ScriptedStart start = ScriptOf(in_state);
            const std::optional<hsf::rip::Timing> timing =
                analyzer.Analyze(job.index, job.track_volumes, &start, job_settings, job.volume);
            if (timing && !timing->silent)
            {
                job.variables = std::move(in_state.variables);
                job.timing = timing;
                return true;
            }
        }

        const std::optional<uint32_t> delay =
            analyzer.SoundingDelay(script, job.track_volumes, kStartDelays, kStartWindow, job_settings);
        if (delay)
        {
            job.start_frame += *delay;
            script = ScriptOf(job);
            job.timing = analyzer.Analyze(job.index, job.track_volumes, &script, job_settings, job.volume);
        }

        return true;
    };
    xsflib::cli::AnalyzeLengths(jobs, threads, analyze);
}

// The scripted start of a rhythm piece's mini: the piece beside its companion (rip/variable_states.h, RhythmScript),
// with the lead-in that ends as the companion hands over to the piece, and the add-on archive that holds the companion
// when the piece's archive doesn't. The piece then plays from the output's first frames, just like a piece that starts
// alone. Nothing for a job whose piece has no companion that the rip can find, or when the hand-over doesn't come
// within 30 s.
struct CompanionStart
{
    hsf::ScriptedStart script;
    std::vector<hsf::AddonArchive> addons;
};

std::optional<CompanionStart> CompanionScriptOf(const ArchiveToRip& to_rip, const Job& job)
{
    const hsf::rip::WaitingPiece* piece = hsf::rip::WaitingPieceOf(PiecesOf(to_rip), job.label);
    const auto number = job.variables.find(28);
    if (!piece || piece->companion.empty() || number == job.variables.end())
    {
        return std::nullopt;
    }

    hsf::fsar::SoundArchive archive = hsf::fsar::SoundArchive::Load(to_rip.archive.data, hsf::kSarOptions);
    for (const FileChunk& group : to_rip.groups)
    {
        archive.AddGroupFile(group.data);
    }

    const auto item = [](uint32_t index)
    {
        return ntdlib::rip::kSoundItem << 24 | index;
    };

    CompanionStart start;
    std::optional<hsf::fsar::SoundArchive> addon_archive;
    std::vector<const hsf::fsar::SoundArchive*> addons;
    if (const std::optional<uint32_t> companion = SoundNamed(archive, piece->companion))
    {
        start.script = hsf::rip::RhythmScript(item(job.index), item(*companion), job.variables);
    }
    else if (std::optional<CompanionAddon> addon = CompanionAddonOf(to_rip, *piece))
    {
        start.script = hsf::rip::RhythmScript(item(job.index), item(addon->companion), job.variables, 0);
        start.addons = {{addon->archive.path, {}}};
        addon_archive.emplace(hsf::fsar::SoundArchive::Load(std::move(addon->archive.data), hsf::kSarOptions));
        addons.push_back(&*addon_archive);
    }
    else
    {
        std::fprintf(stderr, "warning: %s: no sound archive holds its companion %s; the piece plays alone\n",
                     job.label.c_str(), std::string(piece->companion).c_str());
        return std::nullopt;
    }

    hsf::ScriptedStart& script = start.script;

    // The piece's sequence starts in the second frame after its start (ntdlib's atk::Engine::StartSequence), and that
    // of a piece alone in frame 1. The hand-over comes in the frame before the one that FramesUntil counts to. The
    // lead-in therefore ends two frames before it.
    constexpr uint32_t kLimit = 6000;
    const hsf::rip::LengthAnalyzer analyzer(std::move(archive), to_rip.settings);
    const std::optional<uint32_t> frames = analyzer.FramesUntil(script, 28, number->second, kLimit, addons);
    if (!frames || *frames < 2)
    {
        std::fprintf(stderr, "warning: %s: the companion %s doesn't hand over within 30 s; the piece plays alone\n",
                     job.label.c_str(), std::string(piece->companion).c_str());
        return std::nullopt;
    }

    script.lead_in = *frames - 2;

    return start;
}

// The reserved area of a job's mini. A job whose tracks aren't all at 1, that sets variables, or that plays with a
// reverb on aux bus B that the library's descriptor doesn't have, gets the library's descriptor with the job's track
// volumes, scripted start (docs/hsf.md, "Track volumes" and "Scripted starts") and effects (rip/sound_states.h). A
// rhythm piece starts beside its companion (CompanionScriptOf). Its length comes from the piece alone (ScriptOf). A
// synced rip has its script (WithSyncedPieces). The set loads that descriptor last. It therefore takes the place of the
// library's. Every other job gets no reserved area.
std::vector<uint8_t> MiniReserved(const ArchiveToRip& to_rip, const Job& job)
{
    const hsf::ArchiveDescriptor with_reverb = hsf::rip::WithBusBReverb(to_rip.settings, job.bus_b_reverb);
    const bool volumes = job.track_volumes != ntdlib::rip::kFullTrackVolumes;
    const auto same = [](const hsf::AuxEffect& a, const hsf::AuxEffect& b)
    {
        return a.bus == b.bus && a.type == b.type && a.parameters == b.parameters;
    };

    const bool scripted = !job.variables.empty() || job.start_frame != 0;
    const auto synced = SyncedRips().find(job.label);
    if (!volumes && !scripted && synced == SyncedRips().end() &&
        std::ranges::equal(with_reverb.effects, to_rip.settings.effects, same))
    {
        return {};
    }

    hsf::ArchiveDescriptor descriptor = with_reverb;
    descriptor.archive_path = to_rip.archive.path;
    descriptor.group_paths.clear();
    for (const FileChunk& group : to_rip.groups)
    {
        descriptor.group_paths.push_back(group.path);
    }

    const uint32_t sound = ntdlib::rip::kSoundItem << 24 | job.index;
    if (volumes)
    {
        descriptor.flags |= hsf::ArchiveDescriptor::kFlagTrackVolumes;
        descriptor.track_volumes = {{sound, job.track_volumes}};
    }

    if (synced != SyncedRips().end())
    {
        descriptor.flags |= hsf::ArchiveDescriptor::kFlagScripts;
        descriptor.scripts = {synced->second.script};
    }
    else if (scripted)
    {
        descriptor.flags |= hsf::ArchiveDescriptor::kFlagScripts;
        if (std::optional<CompanionStart> companion = CompanionScriptOf(to_rip, job))
        {
            descriptor.scripts = {std::move(companion->script)};
            descriptor.addons = std::move(companion->addons);
        }
        else
        {
            descriptor.scripts = {ScriptOf(job)};
        }
    }

    return descriptor.Serialize();
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

        t.sound_volume = [](const ArchiveToRip& rip, const Job& job)
        {
            return rip.levels ? rip.levels->LevelOf(job.label) : 1.0f;
        };

        // Some games' code turns the tracks of some pieces up and down, or puts a reverb on aux bus B while some sounds
        // play. Each such sound gets a rip of each state (rip/sound_states.h). Rips without the game's effects (with
        // --no-effects, or of a loose archive) leave bus B empty too, and only the tracks' states remain. A game rip
        // with effects takes the reverbs of the game's rules even when start-up puts no effect that HSF reads on the
        // buses. Clubhouse Games is such a game. Some games give some pieces the values that they wait for
        // (rip/variable_states.h). Each such piece gets a rip of each state too. Each job also takes the values that
        // the rules give its sound's variables (Job::start_states). The length analysis tries them on a sound that
        // never sounds. A piece that follows a stream's music sync gets a rip over each stream (WithSyncedPieces).
        t.sound_states = [](const ArchiveToRip& to_rip, const std::vector<Job>& selected)
        {
            const std::span<const hsf::rip::WaitingPiece> pieces = hsf::rip::WaitingPieces(to_rip.archive.data);
            KeptPieces()[to_rip.archive.path] = pieces;
            const ntdlib::sar::ArchiveRules& rules = ntdlib::sar::RulesOf(to_rip.archive.data);
            std::vector<Job> with_starts = selected;
            for (Job& job : with_starts)
            {
                job.start_states = ntdlib::sar::StartsOf(rules, job.label);
            }

            if (!to_rip.game_code)
            {
                ntdlib::sar::ArchiveRules tracks_only;
                tracks_only.layered = rules.layered;
                return WithSyncedPieces(
                    to_rip, hsf::rip::WithVariableStates(pieces, hsf::rip::WithStates(tracks_only, with_starts)));
            }

            return WithSyncedPieces(to_rip,
                                    hsf::rip::WithVariableStates(pieces, hsf::rip::WithStates(rules, with_starts)));
        };

        t.mini_reserved = MiniReserved;

        // A library also holds the add-on archives that its waiting pieces' companions need (CompanionAddonsOf).
        // The minis' descriptors list them. The library's descriptor doesn't.
        t.build_library = [](const ArchiveToRip& to_rip, std::span<const uint32_t>, const xsflib::psf::Tags& tags)
        {
            hsf::ArchiveDescriptor settings = to_rip.settings;
            settings.archive_path = to_rip.archive.path;
            settings.group_paths.clear();
            hsf::ProgramBuilder program;
            program.AddFileRef(to_rip.archive.path, to_rip.archive.data);
            for (const FileChunk& group : to_rip.groups)
            {
                settings.group_paths.push_back(group.path);
                program.AddFileRef(group.path, group.data);
            }

            const std::vector<FileChunk> addons = CompanionAddonsOf(to_rip);
            for (const FileChunk& addon : addons)
            {
                program.AddFileRef(addon.path, addon.data);
            }

            return program.WritePsf(hsf::rip::RipFormat::kPsfVersion, settings.Serialize(), tags);
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
// boot.lua when --voices isn't given, the effects a Bezel Engine game puts on the aux buses, and the game's handler of
// user procedures (hsf::rip::GameProcedures). Throws xsflib::FormatError for a table of the handler that the game's
// files hold in another form.
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

    if (std::optional<hsf::UserProcedureHandler> procedures = hsf::rip::GameProcedures(game.files, game.read_file))
    {
        settings.flags |= hsf::ArchiveDescriptor::kFlagProcedures;
        settings.procedures = std::move(*procedures);
    }

    return settings;
}

// What the settings say about the game's user procedures, for the summary: whose handler runs them, if any.
std::string ProceduresText(const hsf::ArchiveDescriptor& settings)
{
    if ((settings.flags & hsf::ArchiveDescriptor::kFlagProcedures) == 0)
    {
        return std::string();
    }

    return "; user procedures: " + hsf::rip::HandlerName(settings.procedures.handler);
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
    std::printf("voices: %u%s; effects: %s%s\n", settings.voice_count, VoicesFromBoot(game, o) ? " (boot.lua)" : "",
                EffectsText(GameEffects(game, o), "none").c_str(), ProceduresText(settings).c_str());
    if (game.sound_levels.Count() > 0)
    {
        std::printf("the game's sound sheets give levels for %zu sounds\n", game.sound_levels.Count());
    }
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

    const auto build_mini = [&lib_name, &descriptor](const hsf::rip::JaudioJob& job, const xsflib::psf::Tags& tags)
    {
        return hsf::rip::BuildJaudioMini(job, descriptor, lib_name, tags);
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
        // A variant plays with its descriptor.
        const auto analyze = [&files, &descriptor](hsf::rip::JaudioJob& job)
        {
            const hsf::rip::JaudioLengthAnalyzer analyzer(*files, hsf::rip::JobDescriptor(descriptor, job));
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
    const auto variants =
        static_cast<std::size_t>(std::count_if(selection.jobs.begin(), selection.jobs.end(), hsf::rip::IsVariant));
    const std::string summary = std::to_string(pieces) + " pieces of music, " + std::to_string(variants) +
                                " variants of them and " + std::to_string(selection.jobs.size() - pieces - variants) +
                                " sound effects";
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
        // A variant plays with its descriptor.
        const hsf::rip::Jaudio1LengthAnalyzer analyzer(*files, descriptor);
        const auto analyze = [&files, &descriptor, &analyzer](hsf::rip::JaudioJob& job)
        {
            const hsf::rip::JaudioTiming result =
                hsf::rip::IsVariant(job)
                    ? hsf::rip::Jaudio1LengthAnalyzer(*files, hsf::rip::JobDescriptor(descriptor, job)).Analyze(job.id)
                    : analyzer.Analyze(job.id);
            job.timing = result.timing;
            job.missing_bank_notes = result.missing_bank_notes;
            job.sequence_draws = result.sequence_draws;
            return true;
        };
        xsflib::cli::AnalyzeLengths(jobs, o.jobs, analyze);

        // A sound that never sounds may take another branch where its sequence draws a number at random. The game's
        // generator can be in another state as the sound starts. The rip plays the first state after 1 to
        // kSearchDraws draws in which the sound sounds, and the descriptor lists that sound's draws. A variant shares
        // its piece's draws.
        for (hsf::rip::JaudioJob& job : jobs)
        {
            if (hsf::rip::IsVariant(job) || job.missing_bank_notes != 0 || !job.timing || !job.timing->silent ||
                job.sequence_draws == 0)
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
// come from the game's main, one of the builds that hsfrip reads. The folder of the rip has the bank file's name.
// Returns the exit code.
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

    const std::string build_id = hsf::rip::NsoBuildId(main);
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
                     paths.main.c_str(), known.c_str(), other.c_str());
        return 1;
    }

    hsf::rip::JacGameCode code;
    std::vector<ntdlib::jaudio::ArchiveFile> files;
    std::vector<ntdlib::jaudio::WaveSystem> systems;
    try
    {
        code = hsf::rip::ReadJacGameCode(ntdlib::nx::ReadNso(main), build->at);
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
                    paths.bank.c_str(), bank.data.size(), summary.c_str(), std::string(build->version).c_str());
        return 0;
    }

    std::fprintf(stderr, "Jac: %s, %s; Pikmin's settings from main version %s\n", paths.bank.c_str(), summary.c_str(),
                 std::string(build->version).c_str());
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

        const hsf::rip::JacLengthAnalyzer analyzer(*sound, descriptor, std::move(selection.later_events));
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
    if (!o.only_archive.empty() && std::find(archives.begin(), archives.end(), o.only_archive) == archives.end())
    {
        std::fprintf(stderr, "error: the game has no sound archive %s (--list shows them)\n", o.only_archive.c_str());
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

    const auto find_group = [&game](const std::string& name)
    {
        return FindGameGroup(game, name);
    };

    const auto find_sarcs = [&game](const std::string& archive_path)
    {
        return hsf::game::SarcsBeside(game, archive_path);
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
            return file == path ||
                   (file.size() > path.size() && file.ends_with(path) && file[file.size() - path.size() - 1] == '/');
        };
        const auto found = std::find_if(game.files.begin(), game.files.end(), named);

        return found != game.files.end() && game.read_file(*found, data);
    };

    return ntdlib::rip::RipArchives(ArchiveTool(), input, out_dir, archives, common, game.read_file, find_group,
                                    game_name, o, registries, switch_archive, find_sarcs);
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
