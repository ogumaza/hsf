// SPDX-License-Identifier: MIT

// HSF player core (see player.h).

#include "hsf/player.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/sar/set_archive.h"
#include "xsflib/text.h"

#include "atk/effect.h"
#include "atk/engine.h"
#include "audio/i3dl2.h"
#include "hsf/sar.h"

namespace hsf
{
namespace
{

// A sound's nn::atk item id is its index in the archive's sound table, with the sound type in its top byte.
constexpr uint32_t kSoundItem = 0x01;

} // namespace

struct Player::SourceTraits
{
    using Set = LoadedSet;
    using Archive = fsar::SoundArchive;

    // The model, and whether the last frame's output was silence.
    struct Model
    {
        Model(const Archive& archive, const atk::EngineOptions& options) : model(archive, options)
        {
        }

        atk::ArchiveModel model;
        bool silent = false;
    };

    // What the descriptor gives the model, and the sound.
    struct Prepared
    {
        atk::EngineOptions options;
        std::vector<AuxEffect> effects;
        uint32_t sound = 0;
    };

    // The archive loads at the first start, and later starts reuse it.
    static constexpr bool kKeepArchive = true;

    // The player has checked the descriptor. LoadSet requires a sound. A set put together without one asks for item 0,
    // which no sequence is.
    static std::optional<std::string> Prepare(Set& set, Prepared& prepared)
    {
        const ArchiveDescriptor& d = *set.archive;
        prepared.options.voice_count = static_cast<int>(d.voice_count);
        prepared.effects = d.effects;
        prepared.sound = set.sound.value_or(0);

        return std::nullopt;
    }

    // The set's bytes move into the archive. The archive is only given out once all its group files are in it.
    static std::optional<std::string> LoadArchive(Set& set, const Prepared&, std::optional<Archive>& archive)
    {
        const ArchiveDescriptor& d = *set.archive;
        return fsar::LoadSetArchive(set, d.archive_path, d.group_paths, kSarOptions, archive);
    }

    static std::unique_ptr<Model> MakeModel(const Prepared& prepared, const Archive& archive)
    {
        return std::make_unique<Model>(archive, prepared.options);
    }

    // The scene's effects, in their order, before the sound starts.
    static std::optional<std::string> StartModel(Model& model, const Archive&, const Prepared& prepared)
    {
        for (const AuxEffect& effect : prepared.effects)
        {
            const std::vector<float>& v = effect.parameters;
            const audio::I3dl2ReverbParameterSet parameters{v[0], v[1], v[2], v[3], v[4],  v[5],
                                                            v[6], v[7], v[8], v[9], v[10], v[11]};
            model.model.engine->AppendEffect(static_cast<int>(effect.bus),
                                             std::make_unique<atk::I3dl2AuxEffect>(parameters));
        }

        return std::nullopt;
    }

    static std::optional<std::string> StartSequence(Model& model, const Set&, const Prepared& prepared)
    {
        if ((prepared.sound >> 24) != kSoundItem || !model.model.engine->StartSequence(prepared.sound & 0xFFFFFF))
        {
            return "sound " + xsflib::Hex(prepared.sound, 8) + " isn't a sequence whose files the set holds";
        }

        return std::nullopt;
    }

    // A frame of 5 ms: the device's output, 240 interleaved stereo pairs. The model reports a failure by throwing.
    static bool RunFrame(Model& model, std::vector<int16_t>& out)
    {
        model.model.engine->RunFrame();
        const std::span<const int16_t> output = model.model.renderer.DeviceOutput();
        model.silent = std::all_of(output.begin(), output.end(), [](int16_t sample) { return sample == 0; });
        out.insert(out.end(), output.begin(), output.end());

        return true;
    }

    // The sound has finished once the sequence has ended and the output is silence. The effects have then died away
    // too.
    static bool Busy(const Model& model)
    {
        return !model.silent || model.model.engine->IsBusy();
    }
};

Player::Player() = default;

Player::~Player() = default;

void Player::Unload()
{
    source_.reset();
}

bool Player::StartSource()
{
    output_.clear();

    // The source keeps the archive for restarts. Unload drops it with the set.
    if (!source_)
    {
        source_ = std::make_unique<Source>();
    }

    if (const std::optional<std::string> err = source_->Start(set_, output_))
    {
        return Fail(*err);
    }

    return true;
}

bool Player::RunStep()
{
    // The model reports a failure by throwing.
    return source_->RunFrame(output_);
}

bool Player::Finished() const
{
    return !source_->Busy();
}

} // namespace hsf
