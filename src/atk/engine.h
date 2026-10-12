// SPDX-License-Identifier: MIT

// Archive mode's model of nn::atk for one sound archive. Each frame, the model runs what the sound thread runs
// (SoundThread::Run, 0xc18b40, around its FrameProcess, 0xc18590), then the audio renderer's frame. The model reads the
// archive's banks and waves where they lie in the archive, as a game's sound heap holds them, and plays the archive's
// sequence sounds. Each function's comment gives the address of the routine it follows in WarioWare: Get It Together!'s
// main.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "ntdlib/nwsnd/archive_files.h"

#include "atk/channel.h"
#include "atk/effect.h"
#include "atk/hardware.h"
#include "atk/low_level_voice.h"
#include "atk/sequence.h"
#include "atk/util.h"
#include "atk/voice.h"
#include "atk/voice_renderer.h"
#include "audio/i3dl2.h"
#include "audio/renderer.h"
#include "hsf/sar.h"

namespace hsf::atk
{

struct EngineOptions
{
    // The sound system's voice count (SoundSystemParam's +0x58): 96 unless the game changes it before its sound system
    // starts. The sound system's channel count and the renderer's voice count are the same.
    int voice_count = 96;
    HardwareSettings hardware;

    // The I3DL2 reverb a scene of the reference game puts on each of the aux buses A to C, if any (I3dl2AuxEffect).
    std::array<std::optional<audio::I3dl2ReverbParameterSet>, 3> aux_reverbs;
};

class Engine
{
public:
    // Plays on `renderer`. The voices output to the renderer's mixes through `receiver`. The archive must outlive the
    // engine.
    Engine(audio::Renderer& renderer, const fsar::SoundArchive& archive, const OutputReceiver& receiver,
           EngineOptions options = {});

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Starts sequence sound `index`, by its index in the archive's sound table. The game's sound archive player does
    // the same when a sound is started before a frame. The sound starts in the second sound frame after that (the first
    // frame takes the start to the sound thread). Returns the player, or nullptr if there's no such sequence or its
    // files aren't in the archive.
    SequenceSoundPlayer* StartSequence(uint32_t index);

    // A frame of 5 ms: the sound thread's frame, then the renderer's frame.
    void RunFrame();

    // Puts an effect after the others on aux bus `bus` (0 to 2: A to C) of the voices' receiver, and enables the
    // effect. The reference game does the same (SoundSystem::AppendEffect for an aux, 0xc12200,
    // OutputMixer::AppendEffect, 0xc2d6b0, and EffectAux::SetEnabled, 0xc2b510). The effect's buffers hold one ring of
    // whole frames, with all of the effect's channels in each frame.
    void AppendEffect(int bus, std::unique_ptr<EffectAux> effect);

    // Whether a sound still plays or a channel still sounds.
    bool IsBusy() const;

    Random& Rng()
    {
        return random_;
    }

    // The sequences' global variables, which every player shares, from -1.
    std::array<int16_t, 16>& GlobalVariables()
    {
        return global_vars_;
    }

    ChannelManager& Channels()
    {
        return channels_;
    }

    const ChannelManager& Channels() const
    {
        return channels_;
    }

    MultiVoiceManager& Voices()
    {
        return voices_;
    }

    const MultiVoiceManager& Voices() const
    {
        return voices_;
    }

    // The MultiVoices' voices on the audio renderer.
    VoiceRenderer& Renderer()
    {
        return voice_renderer_;
    }

    LowLevelVoiceAllocator& LowLevelVoices()
    {
        return allocator_;
    }

    // A note on the bank in a sequence's bank slot (Bank::NoteOn, 0xc0b140): the velocity region for the key and
    // velocity, its wave, and a channel that starts it. Returns nullptr if the slot, the program, the region or the
    // wave isn't there, or no channel is free.
    Channel* NoteOn(const std::vector<uint32_t>& banks, int slot, const NoteOnInfo& info);

    // Passes a channel's callback on to its owner: track `track` of the player at `player` among the engine's players.
    void ChannelEvent(int player, int track, Channel& channel, Channel::CallbackStatus status);

    // The length analysis's hook for the sequence's loops. While it's set, the players report their loops to it (see
    // ntdlib's SequenceSoundPlayer).
    void SetLoopHook(std::function<void(int track, uint32_t target)> hook)
    {
        on_loop_ = std::move(hook);
    }

    bool FollowsLoops() const
    {
        return static_cast<bool>(on_loop_);
    }

    void OnLoop(const SequenceSoundPlayer&, int track, uint32_t target)
    {
        on_loop_(track, target);
    }

    // The counted loops that the length analysis takes as endless, each a track and its loop's body (see ntdlib's
    // LoopsForEver).
    void SetEndlessLoops(std::set<std::pair<int, uint32_t>> loops)
    {
        endless_loops_ = std::move(loops);
    }

    bool LoopsForEver(const SequenceSoundPlayer&, int track, uint32_t address) const
    {
        return endless_loops_.contains({track, address});
    }

private:
    struct PendingStart
    {
        SequenceSoundPlayer* player;
        int frames; // sound frames before it starts
    };

    // An effect on a bus, with the renderer's aux and the buffer it processes the samples in.
    struct AttachedEffect
    {
        std::unique_ptr<EffectAux> effect;
        int aux = -1;
        std::vector<int32_t> work;
    };

    void FrameProcess();

    // Each effect of each bus, in order, reads what the renderer sent, processes it and writes it back
    // (HardwareManager's update of the effects, 0xc05f30, for the receiver's sub-mix, 0xc2d3b0).
    void UpdateEffects();

    audio::Renderer* renderer_;
    const fsar::SoundArchive* archive_;
    const OutputReceiver* receiver_;
    EngineOptions options_;
    LowLevelVoiceAllocator allocator_;
    VoiceRenderer voice_renderer_;
    MultiVoiceManager voices_;
    ChannelManager channels_;
    Random random_;
    std::array<int16_t, 16> global_vars_;

    // Declared after the channels. The players are then destroyed first, and their tracks' hold on channels goes with
    // them.
    std::vector<std::unique_ptr<SequenceSoundPlayer>> players_;
    std::vector<PendingStart> pending_;
    std::array<std::vector<AttachedEffect>, 4> effects_; // by bus, the main bus first

    // The banks, wave archives and waves that notes play. Each wave reads as WaveFileReader::ReadWaveInfo (0xc0b410)
    // reads it.
    using Files = ntdlib::nwsnd::ArchiveFiles<SequencePolicy>;
    Files files_;

    std::function<void(int track, uint32_t target)> on_loop_;
    std::set<std::pair<int, uint32_t>> endless_loops_;
};

// Archive mode's whole stack for one sound archive: the renderer, set up as nn::atk sets it up by default, and the
// engine. The final mix has six channels and one bus. Sub-mix 0 has four buses of six channels: the main bus and the
// aux buses A to C, each sent to the final mix's channels at 1.0, with the options' reverbs on the aux buses. The
// voices output to sub-mix 0, and the device sink takes the final mix's front left and right.
struct ArchiveModel
{
    // The renderer's frame: 240 samples at 48 kHz.
    static constexpr int32_t kSampleRate = 48000;
    static constexpr int32_t kFrameSamples = 240;

    // `observer`, if any, sees every call made on the renderer, from the first (audio::Renderer::SetObserver).
    explicit ArchiveModel(const fsar::SoundArchive& archive, EngineOptions options = {},
                          audio::RendererObserver* observer = nullptr);

    audio::Renderer renderer;
    OutputReceiver sub_mix;
    std::unique_ptr<Engine> engine;
};

} // namespace hsf::atk
