// SPDX-License-Identifier: MIT

#include "atk/sequence.h"

#include <array>
#include <cstddef>

#include "ntdlib/nwsnd/channel_impl.h"
#include "ntdlib/nwsnd/sequence_impl.h"
#include "ntdlib/nwsnd/voice_impl.h"

#include "atk/channel.h"
#include "atk/engine.h"
#include "atk/voice.h"
#include "atk/voice_renderer.h"

namespace hsf::atk
{

OutputParam SequencePolicy::CombineOutputs(const OutputParam& player, const OutputParam& track, float pan, float span,
                                           float main_send, const std::array<float, 3>& fx_send)
{
    OutputParam output = player;
    output.volume = track.volume * player.volume;
    for (std::size_t channel = 0; channel < 2; ++channel)
    {
        for (std::size_t i = 0; i < 6; ++i)
        {
            output.mix_parameter[channel][i] = track.mix_parameter[channel][i] * player.mix_parameter[channel][i];
        }
    }
    output.pan = (pan + player.pan) + track.pan;
    output.span = (span + player.span) + track.span;
    output.main_send = main_send + (track.main_send + player.main_send);
    for (std::size_t bus = 0; bus < 3; ++bus)
    {
        output.fx_send[bus] = fx_send[bus] + (track.fx_send[bus] + player.fx_send[bus]);
    }

    return output;
}

} // namespace hsf::atk

template class ntdlib::nwsnd::Channel<hsf::atk::SequencePolicy>;
template class ntdlib::nwsnd::ChannelManager<hsf::atk::SequencePolicy>;
template class ntdlib::nwsnd::MultiVoice<hsf::atk::SequencePolicy>;
template class ntdlib::nwsnd::MultiVoiceManager<hsf::atk::SequencePolicy>;
template class ntdlib::nwsnd::SequenceTrack<hsf::atk::SequencePolicy>;
template class ntdlib::nwsnd::SequenceSoundPlayer<hsf::atk::SequencePolicy>;
