// SPDX-License-Identifier: MIT

// nn::atk's MultiVoice and its manager (driver::MultiVoice and driver::MultiVoiceManager): ntdlib's model of them, with
// HSF's policy (see sequence.h), and the data they work with. nw::snd has the same MultiVoice and manager. A MultiVoice
// plays each channel of a wave on a LowLevelVoice of its own (see voice_renderer.h). Each comment gives the address of
// the routine that a type follows in WarioWare: Get It Together!'s main. The optional packet of output gains that a
// game can give a voice (+0x260) and the side of a stream's channels (+0x1c0) aren't modelled: a sequence's voices have
// neither.

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/voice.h"

#include "atk/low_level_voice.h"

namespace hsf::atk
{

using ntdlib::nwsnd::VoiceId;

// A wave as nn::atk reads it from its file (WaveInfo, filled by WaveFileReader::ReadWaveInfo at 0xc0b410): the sample
// format, the loop, the rate, the loop's start and the end (also the length when the wave doesn't loop), and each
// channel's samples, with the DSP-ADPCM coefficients and the decoder's state at the start and at the loop.
struct WaveInfo
{
    struct Channel
    {
        std::span<const uint8_t> data;
        AdpcmCoefficients coefs{};
        AdpcmContext context;
        AdpcmContext loop_context;
    };

    SampleFormat format = SampleFormat::kDspAdpcm;
    bool loop = false;
    int channel_count = 1;
    uint32_t sample_rate = 32000;
    int64_t loop_start = 0;
    int64_t loop_end = 0;
    int64_t original_loop_start = 0;
    std::array<Channel, 2> channels;
};

// A sound's output parameters (OutputParam, 0x50 bytes): its volume, whether the channels mix by pan (0) or by the mix
// parameters (1), each wave channel's mix parameters (front left and right, rear left and right, front centre and LFE),
// the pan, the surround pan, and the main and effect sends. The main send is an offset from 1.
struct OutputParam
{
    float volume = 1.0f;
    uint32_t mix_mode = 0;
    std::array<std::array<float, 6>, 2> mix_parameter{
        {{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}}};
    float pan = 0.0f;
    float span = 0.0f;
    float main_send = 0.0f;
    std::array<float, 3> fx_send{};
};

struct SequencePolicy;

using MultiVoice = ntdlib::nwsnd::MultiVoice<SequencePolicy>;
using MultiVoiceManager = ntdlib::nwsnd::MultiVoiceManager<SequencePolicy>;

} // namespace hsf::atk
