// SPDX-License-Identifier: MIT

// HSF player core (see player.h).

#include "hsf/player.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/jac/engine.h"
#include "ntdlib/jaudio1/engine.h"
#include "ntdlib/jaudio2/engine.h"
#include "ntdlib/jaudio2/output.h"
#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/nx/archive_model.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/set_archive.h"
#include "xsflib/rip/length.h"
#include "xsflib/text.h"

#include "hsf/format.h"
#include "hsf/jac.h"
#include "hsf/jaudio.h"
#include "hsf/jaudio1.h"
#include "hsf/jaudio_models.h"
#include "hsf/model.h"
#include "hsf/nx.h"
#include "hsf/sar.h"

namespace hsf
{
namespace
{

// Runs a sub-frame of 2.5 ms on the engine of a JAudio2 or JAudio1 model: 80 stereo pairs of the DSP's output at
// 32 kHz. AudioOut plays them as 120 pairs at 48 kHz. The first sub-frame gives 119. `model.silent` is set when the
// sub-frame's output is silence.
template <typename Model>
bool RunSubframe(Model& model, std::vector<int16_t>& out)
{
    model.engine.RunSubframe();
    const std::span<const int16_t> output = model.engine.Output();
    model.silent = std::all_of(output.begin(), output.end(), [](int16_t sample) { return sample == 0; });
    model.out.Push(output, out);

    return true;
}

} // namespace

struct Player::SourceTraits
{
    using Set = LoadedSet;

    // The set's sound archive and the add-on archives that its scripts start sounds from (docs/hsf.md, "Scripted
    // starts"). The add-ons are all loaded before a model points at them.
    struct Archive
    {
        fsar::SoundArchive archive;
        std::vector<fsar::SoundArchive> addons;
    };

    // The model, how long the effects have rung in its output (EffectTail), the sound's scripted start and the frames
    // that the model has run. The engine plays the add-on archives as numbers 1 and up, in the descriptor's order.
    struct Model
    {
        Model(const Archive& archives, const atk::EngineOptions& options, const std::vector<AuxEffect>& effects)
            : model(archives.archive, options), tail(effects)
        {
            for (const fsar::SoundArchive& addon : archives.addons)
            {
                model.engine->AddArchive(addon);
            }
        }

        atk::ArchiveModel model;
        EffectTail tail;
        std::optional<ScriptRunner> script;
        uint64_t frame = 0;
    };

    // What the descriptor gives the model, the sound, the volumes of the sound's tracks, the sound's scripted start and
    // the game's handler of user procedures.
    struct Prepared
    {
        atk::EngineOptions options;
        std::vector<AuxEffect> effects;
        uint32_t sound = 0;
        SoundTrackVolumes track_volumes;
        ScriptedStart script;
        UserProcedureHandler procedures;
    };

    // The model between frames: ntdlib's snapshot of the renderer and the engine, the effects' tail, the frames run,
    // and the script's writes of kind 2 that have taken effect with the state of the game's handler of user procedures.
    struct Snapshot
    {
        atk::ArchiveModel::Snapshot model;
        EffectTail tail;
        uint64_t frame = 0;
        ScriptRunner::State script;
    };

    // The archive loads at the first start, and later starts reuse it.
    static constexpr bool kKeepArchive = true;

    // The player has checked the descriptor. LoadSet requires a sound. A set put together without one asks for item 0,
    // which no sequence is.
    static std::optional<std::string> Prepare(const Set& set, Prepared& prepared)
    {
        const ArchiveDescriptor& d = *set.archive;
        prepared.options = EngineOptionsFor(d);
        prepared.effects = d.effects;
        prepared.sound = set.sound.value_or(0);
        if (const SoundTrackVolumes* volumes = d.TrackVolumesOf(prepared.sound))
        {
            prepared.track_volumes = *volumes;
        }

        const ScriptedStart* script = d.ScriptOf(prepared.sound);
        prepared.script = script ? *script : ScriptedStart::Plain(prepared.sound);
        prepared.procedures = d.procedures;

        return std::nullopt;
    }

    // The set's bytes move into the archives. They are only given out once every archive has all its group files.
    static std::optional<std::string> LoadArchive(Set& set, const Prepared&, std::optional<Archive>& archive)
    {
        const ArchiveDescriptor& d = *set.archive;
        std::optional<fsar::SoundArchive> loaded;
        if (auto err = fsar::LoadSetArchive(set, d.archive_path, d.group_paths, kSarOptions, loaded))
        {
            return err;
        }

        Archive archives{std::move(*loaded), {}};
        for (const AddonArchive& addon : d.addons)
        {
            std::optional<fsar::SoundArchive> added;
            if (auto err = fsar::LoadSetArchive(set, addon.archive_path, addon.group_paths, kSarOptions, added))
            {
                return "an add-on archive: " + *err;
            }

            archives.addons.push_back(std::move(*added));
        }

        archive.emplace(std::move(archives));

        return std::nullopt;
    }

    static std::unique_ptr<Model> MakeModel(const Prepared& prepared, const Archive& archive)
    {
        return std::make_unique<Model>(archive, prepared.options, prepared.effects);
    }

    // The scene's effects, in their order, before the sound starts.
    static std::optional<std::string> StartModel(Model& model, const Archive&, const Prepared& prepared)
    {
        AppendEffects(*model.model.engine, prepared.effects);

        return std::nullopt;
    }

    // Runs the model's next frame, then takes the script's steps of the frame after it. StartSequence takes frame 0's.
    // The effects' tail follows the frames whose output is `heard`. The length analysis doesn't follow the lead-in's
    // either.
    static std::optional<std::string> Advance(Model& model, bool heard)
    {
        model.model.engine->RunFrame();
        if (heard)
        {
            model.tail.Frame(model.model.renderer.DeviceOutput());
        }

        ++model.frame;

        return model.script->Apply(*model.model.engine, model.frame);
    }

    // A sound's nn::atk item ID is its index in the archive's sound table, with the sound type in its top byte. The
    // sound starts as its script says: the steps of frame 0, then the lead-in's frames, whose output nobody hears. The
    // game's code gives the sound's tracks their volumes as it starts the sound (ScriptRunner).
    static std::optional<std::string> StartSequence(Model& model, const Set&, const Prepared& prepared)
    {
        model.script.emplace(prepared.script, prepared.track_volumes.volumes, prepared.procedures);
        if (auto err = model.script->Apply(*model.model.engine, 0))
        {
            return err;
        }

        for (uint32_t i = 0; i < prepared.script.lead_in; ++i)
        {
            if (auto err = Advance(model, false))
            {
                return err;
            }
        }

        return std::nullopt;
    }

    // A frame of 5 ms: the device's output, 240 interleaved stereo pairs. The model reports a failure by throwing.
    static bool RunFrame(Model& model, std::vector<int16_t>& out)
    {
        if (auto err = Advance(model, true))
        {
            throw std::runtime_error(*err);
        }

        const std::span<const int16_t> output = model.model.renderer.DeviceOutput();
        out.insert(out.end(), output.begin(), output.end());

        return true;
    }

    // The sound has finished once the sequence has ended and the effects no longer ring.
    static bool Busy(const Model& model)
    {
        return model.tail.Ringing() || model.model.engine->IsBusy() || model.script->Pending(model.frame);
    }

    // ntdlib's snapshot shares nothing with an earlier one. `previous` goes unused.
    static Snapshot Save(const Model& model, const Snapshot*)
    {
        return {model.model.Save(), model.tail, model.frame, model.script->Saved()};
    }

    // ntdlib's Restore throws for a snapshot of another model, and then changes nothing. The restore drops the players
    // that started after the snapshot. The steps of the snapshot's next frame came before it.
    static void Restore(Model& model, const Snapshot& snapshot)
    {
        model.model.Restore(snapshot.model);
        model.tail = snapshot.tail;
        model.frame = snapshot.frame;
        model.script->Rewind(snapshot.frame + 1, snapshot.script);
    }
};

// The model of JAudio (ntdlib's jaudio2): the game's sound files, and an engine that plays a sound of them through
// Hagi's path to AudioOut.
struct Player::JaudioTraits
{
    using Set = LoadedSet;
    using Archive = jaudio::SoundFiles;

    // The engine, AudioOut's resampler, and whether the last frame's output was silence.
    struct Model
    {
        Model(const Archive& files, const jaudio::Settings& settings) : engine(files, settings)
        {
        }

        jaudio::Engine engine;
        jaudio::AudioOut out;
        bool silent = false;
    };

    struct Prepared
    {
        jaudio::Settings settings;
        jaudio::TrackStates states;
        uint32_t sound = 0;
    };

    // The files load at the first start, and later starts reuse them.
    static constexpr bool kKeepArchive = true;

    static std::optional<std::string> Prepare(const Set& set, Prepared& prepared)
    {
        const ArchiveDescriptor& d = *set.archive;
        prepared.settings = JaudioEngineSettings(d.jaudio);
        prepared.sound = set.sound.value_or(0);
        prepared.states = TrackStatesFor(d.jaudio, prepared.sound);

        return std::nullopt;
    }

    // The model copies what it plays from the set's files: the start-up archive, the sequences and the waves. The set's
    // copies are then dropped.
    static std::optional<std::string> LoadArchive(Set& set, const Prepared&, std::optional<Archive>& files)
    {
        const ArchiveDescriptor& d = *set.archive;
        const auto file = [&set](const std::string& path)
        {
            return static_cast<const std::vector<uint8_t>*>(set.File(path));
        };
        if (std::optional<std::string> err = LoadJaudioFiles(d, file, files))
        {
            return err;
        }

        for (FileChunk& chunk : set.files)
        {
            std::vector<uint8_t>().swap(chunk.data);
        }

        return std::nullopt;
    }

    static std::unique_ptr<Model> MakeModel(const Prepared& prepared, const Archive& files)
    {
        return std::make_unique<Model>(files, prepared.settings);
    }

    static std::optional<std::string> StartModel(Model&, const Archive&, const Prepared&)
    {
        return std::nullopt;
    }

    static std::optional<std::string> StartSequence(Model& model, const Set&, const Prepared& prepared)
    {
        if (const std::optional<std::string> err = model.engine.Start(prepared.sound, prepared.states))
        {
            return "sound " + xsflib::Hex(prepared.sound, 8) + ": " + *err;
        }

        return std::nullopt;
    }

    static bool RunFrame(Model& model, std::vector<int16_t>& out)
    {
        return RunSubframe(model, out);
    }

    // The sound has finished once the sequence has ended, no DSP channel plays and the output is silence. The FX lines
    // have then died away too.
    static bool Busy(const Model& model)
    {
        return !model.silent || model.engine.Busy();
    }
};

// The model of JAudio1 (ntdlib's jaudio1) for a set of library 2: the game's sound files, and an engine that plays a
// sound of them through Hagi's path to AudioOut.
struct Player::Jaudio1Traits
{
    using Set = LoadedSet;
    using Archive = Jaudio1Files;

    // The engine, AudioOut's resampler, and whether the last frame's output was silence. The files outlive the model.
    struct Model
    {
        Model(const Archive& archive, const jaudio1::Settings& settings)
            : files(archive), engine(archive.sound, settings)
        {
        }

        const Archive& files;
        jaudio1::Engine engine;
        jaudio::AudioOut out;
        bool silent = false;
    };

    struct Prepared
    {
        jaudio1::Settings settings;
        JaudioLibrary2 library;
        uint32_t sound = 0;
    };

    // The files load at the first start, and later starts reuse them.
    static constexpr bool kKeepArchive = true;

    static std::optional<std::string> Prepare(const Set& set, Prepared& prepared)
    {
        const ArchiveDescriptor& d = *set.archive;
        prepared.settings = Jaudio1EngineSettings(d.jaudio);
        prepared.library = d.jaudio.library2;
        prepared.sound = set.sound.value_or(0);

        return std::nullopt;
    }

    // The model copies what it plays from the set's files. The set's copies are then dropped.
    static std::optional<std::string> LoadArchive(Set& set, const Prepared&, std::optional<Archive>& files)
    {
        const ArchiveDescriptor& d = *set.archive;
        const auto file = [&set](const std::string& path)
        {
            return static_cast<const std::vector<uint8_t>*>(set.File(path));
        };
        if (std::optional<std::string> err = LoadJaudio1Files(d, file, files))
        {
            return err;
        }

        for (FileChunk& chunk : set.files)
        {
            std::vector<uint8_t>().swap(chunk.data);
        }

        return std::nullopt;
    }

    static std::unique_ptr<Model> MakeModel(const Prepared& prepared, const Archive& files)
    {
        return std::make_unique<Model>(files, prepared.settings);
    }

    static std::optional<std::string> StartModel(Model&, const Archive&, const Prepared&)
    {
        return std::nullopt;
    }

    static std::optional<std::string> StartSequence(Model& model, const Set&, const Prepared& prepared)
    {
        if (const std::optional<std::string> err =
                StartJaudio1Sound(model.engine, model.files, prepared.library, prepared.sound))
        {
            return "sound " + xsflib::Hex(prepared.sound, 8) + ": " + *err;
        }

        return std::nullopt;
    }

    static bool RunFrame(Model& model, std::vector<int16_t>& out)
    {
        return RunSubframe(model, out);
    }

    // The sound has finished once the sequence or the effect has ended, no DSP channel plays and the output is silence.
    // The FX lines have then died away too.
    static bool Busy(const Model& model)
    {
        return !model.silent || model.engine.Busy();
    }
};

// The model of the C JAudio (ntdlib's jac) for a set of library 3: the game's sound files, and an engine that plays a
// sound of them after the game's start-up, through Hagi's path to AudioOut.
struct Player::JacTraits
{
    using Set = LoadedSet;
    using Archive = JacFiles;

    // The engine, AudioOut's resampler, and whether the last frame's output was silence. The files outlive the model.
    struct Model
    {
        Model(const Archive& archive, const jac::Settings& settings) : engine(*archive.sound, settings)
        {
        }

        jac::Engine engine;
        jaudio::AudioOut out;
        bool silent = false;
    };

    struct Prepared
    {
        jac::Settings settings;
        JaudioLibrary3 library;
        uint32_t sound = 0;
    };

    // The files load at the first start, and later starts reuse them.
    static constexpr bool kKeepArchive = true;

    static std::optional<std::string> Prepare(const Set& set, Prepared& prepared)
    {
        const ArchiveDescriptor& d = *set.archive;
        prepared.settings = JacEngineSettings(d.jaudio);
        prepared.library = d.jaudio.library3;
        prepared.sound = set.sound.value_or(0);

        return std::nullopt;
    }

    // The set's chunks move into the files. The model reads the wave archives from them as the game's scenes load.
    static std::optional<std::string> LoadArchive(Set& set, const Prepared&, std::optional<Archive>& files)
    {
        const ArchiveDescriptor& d = *set.archive;
        files.emplace();
        for (FileChunk& chunk : set.files)
        {
            files->chunks[chunk.path] = std::move(chunk.data);
        }

        const auto file = [&chunks = files->chunks](const std::string& path) -> const std::vector<uint8_t>*
        {
            const auto found = chunks.find(path);
            return found == chunks.end() ? nullptr : &found->second;
        };
        return LoadJacFiles(d, file, files->sound);
    }

    static std::unique_ptr<Model> MakeModel(const Prepared& prepared, const Archive& files)
    {
        return std::make_unique<Model>(files, prepared.settings);
    }

    static std::optional<std::string> StartModel(Model&, const Archive&, const Prepared&)
    {
        return std::nullopt;
    }

    // The game's start-up runs first. Its sub-frames make no sound, and they don't reach AudioOut.
    static std::optional<std::string> StartSequence(Model& model, const Set&, const Prepared& prepared)
    {
        if (const std::optional<std::string> err = StartJacSound(model.engine, prepared.library, prepared.sound))
        {
            return "sound " + xsflib::Hex(prepared.sound, 8) + ": " + *err;
        }

        return std::nullopt;
    }

    // A sub-frame of 2.5 ms, as for JAudio2: Hagi's DSP renders it, then the CPU side and the game's code run.
    static bool RunFrame(Model& model, std::vector<int16_t>& out)
    {
        model.engine.RunFrame();
        const std::span<const int16_t> output = model.engine.Output();
        // The FX lines' feedback can hold quieter values than xsflib's level of silence for ever.
        model.silent = !xsflib::rip::Audible(output);
        model.out.Push(output, out);

        return true;
    }

    // The sound has finished once it has stopped playing (jac::Engine::Playing), no DSP channel plays and the output is
    // silent. The FX lines have then died away too.
    static bool Busy(const Model& model)
    {
        return !model.silent || model.engine.Busy();
    }
};

// The model of NW4R SND on Skyward Sword HD's layer (ntdlib's rvl::nx): the sound archive, and the stack that plays a
// sequence of it.
struct Player::Nw4rTraits
{
    using Set = LoadedSet;
    using Archive = ntdlib::rsar::SoundArchive;

    // The reverbs on the aux buses ring until a second has passed without a frame louder than ±8 (xsflib's Audible), as
    // in the length analysis (ntdlib::rvl::nx::LengthConsole). Their arithmetic rounds down, and samples of ±1 can stay
    // in their output for ever. A frame of silence can also come before a reverb's tail, during its pre-delay.
    static constexpr int kRingingFrames = 200;

    // The stack, and the frames that the reverbs may still ring.
    struct Model
    {
        ntdlib::rvl::nx::ArchiveModel model;
        int ringing_frames = 0;
    };

    // The stack's snapshot, and the frames that the reverbs could still ring when it was taken.
    struct Snapshot
    {
        ntdlib::rvl::nx::ArchiveModel::Snapshot model;
        int ringing_frames = 0;
    };

    struct Prepared
    {
        ntdlib::SndBuild build;
        uint32_t sound = 0;
    };

    // The archive loads at the first start, and later starts reuse it.
    static constexpr bool kKeepArchive = true;

    static std::optional<std::string> Prepare(const Set& set, Prepared& prepared)
    {
        prepared.build = ntdlib::SndBuild{set.archive->nw4r.snd_build, false};
        prepared.sound = set.sound.value_or(0);

        return std::nullopt;
    }

    // The set's bytes of the archive move into the archive.
    static std::optional<std::string> LoadArchive(Set& set, const Prepared&, std::optional<Archive>& archive)
    {
        const std::string& path = set.archive->archive_path;
        for (FileChunk& chunk : set.files)
        {
            if (chunk.path == path)
            {
                archive.emplace(Archive::Load(std::move(chunk.data), ntdlib::rvl::snd::kRsarOptions));
                return std::nullopt;
            }
        }

        return "the sound archive " + path + " isn't in the set";
    }

    static std::unique_ptr<Model> MakeModel(const Prepared& prepared, const Archive&)
    {
        auto model = std::make_unique<Model>();
        model->model.build = prepared.build;

        return model;
    }

    static std::optional<std::string> StartModel(Model& model, const Archive& archive, const Prepared&)
    {
        model.model.Start(archive);

        return std::nullopt;
    }

    // The sound is its index in the archive's sound table.
    static std::optional<std::string> StartSequence(Model& model, const Set&, const Prepared& prepared)
    {
        if (model.model.engine->StartSequence(prepared.sound) == nullptr)
        {
            return "sound " + std::to_string(prepared.sound) + " isn't a sequence that the archive can play";
        }

        return std::nullopt;
    }

    // A frame of 5 ms: 240 stereo pairs. The model reports what it doesn't model by throwing.
    static bool RunFrame(Model& model, std::vector<int16_t>& out)
    {
        const std::size_t start = out.size();
        model.model.RunFrame(out);
        const std::span<const int16_t> frame(out.begin() + static_cast<std::ptrdiff_t>(start), out.end());
        model.ringing_frames = xsflib::rip::Audible(frame) ? kRingingFrames : std::max(model.ringing_frames - 1, 0);

        return true;
    }

    // The sound has finished once the engine has nothing playing and the reverbs have stopped ringing.
    static bool Busy(const Model& model)
    {
        return model.model.engine->IsBusy() || (model.model.effects.Any() && model.ringing_frames > 0);
    }

    // The snapshot shares its unchanged memory pages with `previous`.
    static Snapshot Save(const Model& model, const Snapshot* previous)
    {
        return {model.model.Save(previous != nullptr ? &previous->model : nullptr), model.ringing_frames};
    }

    // The engine throws for a snapshot that it can't return to, and then changes nothing.
    static void Restore(Model& model, const Snapshot& snapshot)
    {
        model.model.Restore(snapshot.model);
        model.ringing_frames = snapshot.ringing_frames;
    }
};

struct Player::SourceSnapshot
{
    std::shared_ptr<SourceTraits::Snapshot> model;
    std::shared_ptr<Nw4rTraits::Snapshot> nw4r; // for a set of version 3
};

Player::Player() = default;

Player::~Player() = default;

void Player::Unload()
{
    source_.reset();
    nw4r_.reset();
    jaudio_.reset();
    jaudio1_.reset();
    jac_.reset();
}

bool Player::Jaudio() const
{
    return set_.archive && set_.archive->version == ArchiveDescriptor::kJaudioVersion;
}

bool Player::Jaudio1() const
{
    return Jaudio() && set_.archive->jaudio.library == JaudioSettings::kPikmin2Library;
}

bool Player::Jac() const
{
    return Jaudio() && set_.archive->jaudio.library == JaudioSettings::kPikminLibrary;
}

bool Player::Nw4r() const
{
    return set_.archive && set_.archive->version == ArchiveDescriptor::kNw4rVersion;
}

bool Player::StartSource()
{
    output_.clear();

    // The source keeps the archive for restarts. Unload drops it with the set.
    std::optional<std::string> err;
    if (Nw4r())
    {
        if (!nw4r_)
        {
            nw4r_ = std::make_unique<Nw4rSource>();
        }
        err = nw4r_->Start(set_, output_);
    }
    else if (Jac())
    {
        if (!jac_)
        {
            jac_ = std::make_unique<JacSource>();
        }
        err = jac_->Start(set_, output_);
    }
    else if (Jaudio1())
    {
        if (!jaudio1_)
        {
            jaudio1_ = std::make_unique<Jaudio1Source>();
        }
        err = jaudio1_->Start(set_, output_);
    }
    else if (Jaudio())
    {
        if (!jaudio_)
        {
            jaudio_ = std::make_unique<JaudioSource>();
        }
        err = jaudio_->Start(set_, output_);
    }
    else
    {
        if (!source_)
        {
            source_ = std::make_unique<Source>();
        }
        err = source_->Start(set_, output_);
    }

    if (err)
    {
        return Fail(*err);
    }

    return true;
}

bool Player::RunStep()
{
    // The model reports a failure by throwing.
    if (Nw4r())
    {
        return nw4r_->RunFrame(output_);
    }

    if (Jac())
    {
        return jac_->RunFrame(output_);
    }

    if (Jaudio1())
    {
        return jaudio1_->RunFrame(output_);
    }

    return Jaudio() ? jaudio_->RunFrame(output_) : source_->RunFrame(output_);
}

bool Player::Finished() const
{
    if (Nw4r())
    {
        return !nw4r_->Busy();
    }

    if (Jac())
    {
        return !jac_->Busy();
    }

    if (Jaudio1())
    {
        return !jaudio1_->Busy();
    }

    return Jaudio() ? !jaudio_->Busy() : !source_->Busy();
}

std::shared_ptr<const Player::SourceSnapshot> Player::SaveSource(const SourceSnapshot* previous)
{
    // A start that failed before the model existed leaves nothing to save. The model of JAudio takes no snapshots. A
    // snapshot of NW4R SND's model shares its unchanged memory with the one before.
    if (Nw4r())
    {
        if (!nw4r_ || nw4r_->GetModel() == nullptr)
        {
            return nullptr;
        }

        const Nw4rTraits::Snapshot* before = previous != nullptr ? previous->nw4r.get() : nullptr;
        return std::make_shared<const SourceSnapshot>(SourceSnapshot{nullptr, nw4r_->Save(before)});
    }

    if (Jaudio() || !source_ || source_->GetModel() == nullptr)
    {
        return nullptr;
    }

    return std::make_shared<const SourceSnapshot>(SourceSnapshot{source_->Save(nullptr), nullptr});
}

bool Player::CanRestore(const SourceSnapshot* source) const
{
    if (Nw4r())
    {
        return source != nullptr && source->nw4r != nullptr && nw4r_ && nw4r_->GetModel() != nullptr;
    }

    return !Jaudio() && source != nullptr && source->model != nullptr && source_ && source_->GetModel() != nullptr;
}

void Player::RestoreSource(const SourceSnapshot& source)
{
    if (Nw4r())
    {
        nw4r_->Restore(*source.nw4r);
        return;
    }

    source_->Restore(*source.model);
}

} // namespace hsf
