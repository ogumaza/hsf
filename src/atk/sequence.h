// SPDX-License-Identifier: MIT

// nn::atk's sequence playback (driver::SequenceTrack with its MmlParser, and driver::SequenceSoundPlayer): ntdlib's
// model of it, and of the channels and MultiVoices that play its notes. HSF's policy gives the model the Switch's
// arithmetic and steps, and HSF's engine, waves, voices, one output and the mixes that voices output to.

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "ntdlib/nwsnd/archive_files.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/sequence.h"

#include "atk/channel.h"
#include "atk/hardware.h"
#include "atk/low_level_voice.h"
#include "atk/voice.h"
#include "hsf/sar.h"

namespace hsf::atk
{

class Engine;

// The sequence model's policy: the Switch's arithmetic and steps, and HSF's types.
struct SequencePolicy : ntdlib::nwsnd::NxPolicy
{
    using Channel = atk::Channel;
    using Engine = atk::Engine;
    using NoteOnInfo = atk::NoteOnInfo;
    using SoundInfo = fsar::SoundInfo;
    using WaveInfo = atk::WaveInfo;
    using Outputs = OutputParam;
    using Receiver = OutputReceiver;
    using VoiceParam = atk::VoiceParam;
    using SampleFormat = atk::SampleFormat;

    // A channel's samples are where the wave archive's file has them. The renderer reads them there.
    static void PlaceSamples(WaveInfo::Channel& channel, std::span<const uint8_t> samples,
                             const ntdlib::nwsnd::LoadedWaveArchive&)
    {
        channel.data = samples;
    }

    // The one output device's parameters, from each of `outputs` in step.
    template <typename Function, typename... Each>
    static void ForEachOutput(const Function& function, Each&... outputs)
    {
        function(outputs...);
    }

    // A voice's parameters when it's allocated (Voice::AllocVoice, 0xc27bc0): gains of 1 to the main bus's front left
    // and right, and no filters.
    static VoiceParam InitialVoiceParam()
    {
        VoiceParam param;
        param.mix[0] = 1.0f;
        param.mix[1] = 1.0f;

        return param;
    }

    // The output parameters combine the player's, the track's, and the bytecode's pan, surround pan and sends. The
    // player's and the track's sends are added before the bytecode's sends.
    static OutputParam CombineOutputs(const OutputParam& player, const OutputParam& track, float pan, float span,
                                      float main_send, const std::array<float, 3>& fx_send);

    static void SetOutputs(Channel& channel, const OutputParam& output)
    {
        channel.outputs = output;
    }

    // nn::atk's tracks set no front bypass on their voices.
    static void SetFrontBypass(Engine&, Channel&, bool)
    {
    }

    // A channel's voices output to the player's mix.
    static void SetReceiver(NoteOnInfo& info, const OutputReceiver* receiver)
    {
        info.receiver = receiver;
    }
};

using SequenceTrack = ntdlib::nwsnd::SequenceTrack<SequencePolicy>;
using SequenceSoundPlayer = ntdlib::nwsnd::SequenceSoundPlayer<SequencePolicy>;

} // namespace hsf::atk
