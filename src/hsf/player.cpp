// SPDX-License-Identifier: MIT

// HSF player core (see player.h).

#include "hsf/player.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atk/effect.h"
#include "atk/engine.h"
#include "audio/i3dl2.h"
#include "fsar/formats.h"

namespace hsf
{
namespace
{

// A sound's nn::atk item id is its index in the archive's sound table, with the sound type in its top byte.
constexpr uint32_t kSoundItem = 0x01;

std::string Hex(uint32_t value)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08x", static_cast<unsigned>(value));

    return buf;
}

std::string Missing(const std::string& path)
{
    return "the set has no file " + path;
}

} // namespace

class Player::Source
{
public:
    // The model playing the sound from `archive`, with the descriptor's voice count and aux effects.
    std::optional<std::string> Start(const LoadedSet& set, const fsar::SoundArchive& archive);

    // Runs a frame of 5 ms and returns the device's output: 240 interleaved stereo pairs.
    std::span<const int16_t> Frame();

    bool Finished() const
    {
        return silent_ && !model_->engine->IsBusy();
    }

private:
    std::unique_ptr<atk::ArchiveModel> model_;
    bool silent_ = false; // the last frame's output was silence
};

std::optional<std::string> Player::Source::Start(const LoadedSet& set, const fsar::SoundArchive& archive)
{
    const ArchiveDescriptor& d = *set.archive;
    atk::EngineOptions options;
    options.voice_count = static_cast<int>(d.voice_count);
    model_ = std::make_unique<atk::ArchiveModel>(archive, options);

    // The scene's effects, in their order, before the sound starts.
    for (const AuxEffect& effect : d.effects)
    {
        const std::vector<float>& v = effect.parameters;
        const audio::I3dl2ReverbParameterSet parameters{v[0], v[1], v[2], v[3], v[4],  v[5],
                                                        v[6], v[7], v[8], v[9], v[10], v[11]};
        model_->engine->AppendEffect(static_cast<int>(effect.bus), std::make_unique<atk::I3dl2AuxEffect>(parameters));
    }

    if ((set.sound >> 24) != kSoundItem || !model_->engine->StartSequence(set.sound & 0xFFFFFF))
    {
        return "sound " + Hex(set.sound) + " isn't a sequence whose files the set holds";
    }

    return std::nullopt;
}

std::span<const int16_t> Player::Source::Frame()
{
    model_->engine->RunFrame();
    const std::span<const int16_t> output = model_->renderer.DeviceOutput();
    silent_ = std::all_of(output.begin(), output.end(), [](int16_t sample) { return sample == 0; });

    return output;
}

Player::Player() = default;

Player::~Player() = default;

bool Player::Fail(const std::string& message)
{
    error_ = message;
    state_ = State::kError;

    return false;
}

bool Player::Load(const std::string& path, const FileReader& reader)
{
    LoadedSet set;
    if (auto err = LoadSet(path, reader, set))
    {
        return Fail(*err);
    }

    return Load(std::move(set));
}

bool Player::Load(LoadedSet set)
{
    source_.reset();
    archive_.reset();
    set_ = std::move(set);
    state_ = State::kIdle;
    error_.clear();
    if (!set_.archive)
    {
        return Fail("no archive descriptor");
    }

    return true;
}

bool Player::Start()
{
    // The model's failures arrive as exceptions.
    try
    {
        return StartImpl();
    }
    catch (const std::exception& e)
    {
        source_.reset();
        return Fail(std::string("the sound couldn't start: ") + e.what());
    }
}

bool Player::StartImpl()
{
    output_.clear();
    consumed_ = 0;
    dropped_ = 0;
    finished_frame_ = 0;
    error_.clear();
    if (!set_.archive)
    {
        return Fail("nothing loaded");
    }

    if (auto err = ReadArchive())
    {
        return Fail(*err);
    }

    source_ = std::make_unique<Source>();
    const std::optional<std::string> err = source_->Start(set_, *archive_);
    if (err)
    {
        source_.reset();
        return Fail(*err);
    }

    state_ = State::kPlaying;

    return true;
}

std::optional<std::string> Player::ReadArchive()
{
    if (archive_)
    {
        return std::nullopt;
    }

    // The set's bytes move into the archive. A restart reuses the archive.
    const ArchiveDescriptor& d = *set_.archive;
    std::vector<uint8_t>* bytes = set_.File(d.archive_path);
    if (!bytes)
    {
        return Missing(d.archive_path);
    }

    auto archive = std::make_unique<fsar::SoundArchive>(fsar::SoundArchive::Load(std::move(*bytes)));
    for (const std::string& path : d.group_paths)
    {
        std::vector<uint8_t>* group = set_.File(path);
        if (!group)
        {
            return Missing(path);
        }

        archive->AddGroupFile(std::move(*group));
    }

    archive_ = std::move(archive);

    return std::nullopt;
}

bool Player::Step()
{
    try
    {
        const std::span<const int16_t> frame = source_->Frame();
        output_.insert(output_.end(), frame.begin(), frame.end());
        if (state_ == State::kPlaying && source_->Finished())
        {
            state_ = State::kFinished;
            finished_frame_ = (dropped_ + output_.size()) / 2;
        }
    }
    catch (const std::exception& e)
    {
        return Fail(std::string("the model failed: ") + e.what());
    }

    return true;
}

std::size_t Player::Render(int16_t* out, std::size_t frames)
{
    if (state_ != State::kPlaying && state_ != State::kFinished)
    {
        std::fill(out, out + frames * 2, int16_t{0});
        return 0;
    }

    while ((output_.size() - consumed_) / 2 < frames)
    {
        if (!Step())
        {
            std::fill(out, out + frames * 2, int16_t{0});
            return 0;
        }
    }

    std::copy(output_.begin() + static_cast<std::ptrdiff_t>(consumed_),
              output_.begin() + static_cast<std::ptrdiff_t>(consumed_ + frames * 2), out);
    consumed_ += frames * 2;

    // Drop consumed samples now and then to bound memory.
    if (consumed_ > (1u << 20))
    {
        output_.erase(output_.begin(), output_.begin() + static_cast<std::ptrdiff_t>(consumed_));
        dropped_ += consumed_;
        consumed_ = 0;
    }

    return frames;
}

long long Player::LengthMs() const
{
    const auto it = set_.tags.find("length");
    return it == set_.tags.end() ? -1 : ParseTime(it->second);
}

long long Player::FadeMs() const
{
    const auto it = set_.tags.find("fade");
    return it == set_.tags.end() ? -1 : ParseTime(it->second);
}

double Player::Volume() const
{
    const auto it = set_.tags.find("volume");
    if (it == set_.tags.end())
    {
        return 1.0;
    }

    // Text after the number, as in "0.5dB", is ignored. So is a value that isn't a finite number, such as 1e999: it
    // would turn every sample into nonsense.
    const std::optional<DecimalNumber> volume = ReadDecimal(it->second);
    return volume && std::isfinite(volume->value) ? volume->value : 1.0;
}

} // namespace hsf
