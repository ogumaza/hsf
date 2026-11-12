// SPDX-License-Identifier: MIT

// hsfrip's archive mode (see archive_rip.h).

#include "cli/archive_rip.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/nx/bex/music_sync.h"
#include "ntdlib/rip/archive.h"
#include "ntdlib/rip/archive_mode.h"
#include "xsflib/cli/rip.h"

#include "hsf/format.h"
#include "hsf/sar.h"
#include "rip/rip.h"
#include "rip/sound_states.h"
#include "rip/variable_states.h"

namespace hsf::cli
{

namespace
{

using ntdlib::rip::Job;

// The latest start, in frames of 5 ms, that the nn::atk ripper tries for a sound that never sounds and whose sequence
// draws a number, and the frames after each start in which the sound has to sound (AnalyzeLengths). A branch that one
// draw in ten takes would have to be missed 400 times in a row.
constexpr uint32_t kStartDelays = 400;
constexpr uint32_t kStartWindow = 2000;

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

// The companions in `archive` of the waiting pieces among `jobs` that aren't jobs themselves, such as the rhythm
// controller of a piece that --only picks alone. Each piece's script starts its companion (CompanionScriptOf). The
// library therefore keeps the companion's files.
std::vector<uint32_t> CompanionsInArchive(const ArchiveToRip& to_rip, const hsf::fsar::SoundArchive& archive,
                                          const std::vector<Job>& jobs)
{
    const auto is_job = [&jobs](uint32_t sound)
    {
        return std::any_of(jobs.begin(), jobs.end(), [sound](const Job& job) { return job.index == sound; });
    };
    std::vector<uint32_t> companions;
    for (const Job& job : jobs)
    {
        const hsf::rip::WaitingPiece* piece = hsf::rip::WaitingPieceOf(PiecesOf(to_rip), job.label);
        if (!piece || piece->companion.empty())
        {
            continue;
        }

        const std::optional<uint32_t> companion = archive.FindSound(std::string(piece->companion));
        if (companion && !is_job(*companion) &&
            std::find(companions.begin(), companions.end(), *companion) == companions.end())
        {
            companions.push_back(*companion);
        }
    }

    return companions;
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
    const std::optional<uint32_t> companion = archive.FindSound(std::string(piece.companion));
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
        if (piece.companion.empty() || archive.FindSound(std::string(piece.companion)))
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

        const uint32_t sound = hsf::fsar::ItemId(hsf::fsar::kSoundItem, job.index);
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
    const uint32_t sound = hsf::fsar::ItemId(hsf::fsar::kSoundItem, job.index);
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
    const auto number = job.variables.find(hsf::rip::kRhythmNumber);
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
        return hsf::fsar::ItemId(hsf::fsar::kSoundItem, index);
    };

    CompanionStart start;
    std::optional<hsf::fsar::SoundArchive> addon_archive;
    std::vector<const hsf::fsar::SoundArchive*> addons;
    if (const std::optional<uint32_t> companion = archive.FindSound(std::string(piece->companion)))
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
    const std::optional<uint32_t> frames =
        analyzer.FramesUntil(script, hsf::rip::kRhythmNumber, number->second, kLimit, addons);
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

    hsf::ArchiveDescriptor descriptor =
        ntdlib::rip::LibraryDescriptor<hsf::rip::RipFormat>(to_rip.archive, to_rip.groups, with_reverb);

    const uint32_t sound = hsf::fsar::ItemId(hsf::fsar::kSoundItem, job.index);
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

} // namespace

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
        t.played_sounds = CompanionsInArchive;

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

        // The hsf_sound tag gives the sound's label (docs/hsf.md).
        t.sound_tag_holds_sound_label = true;

        // A library also holds the add-on archives that its waiting pieces' companions need (CompanionAddonsOf).
        // The minis' descriptors list them. The library's descriptor doesn't.
        t.build_library = [](const ArchiveToRip& to_rip, std::span<const uint32_t>, const xsflib::psf::Tags& tags)
        {
            const hsf::ArchiveDescriptor settings =
                ntdlib::rip::LibraryDescriptor<hsf::rip::RipFormat>(to_rip.archive, to_rip.groups, to_rip.settings);
            hsf::ProgramBuilder program;
            program.AddFileRef(to_rip.archive.path, to_rip.archive.data);
            for (const FileChunk& group : to_rip.groups)
            {
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

} // namespace hsf::cli
