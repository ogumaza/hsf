// SPDX-License-Identifier: MIT

// A voice's data source in the Switch's audio renderer: the wave buffers a voice plays, each channel's processing
// state, and the decoding of a frame's samples from the buffers with the sample-rate conversion. This follows
// nn::audio::DecodeFromWaveBuffers in WarioWare: Get It Together!'s sdk (0x6eda0), the build of the renderer for the
// CPU, to the bit.

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "audio/dsp.h"

namespace hsf::audio
{

// The renderer has four wave buffer slots for each voice.
inline constexpr int kWaveBufferCount = 4;

// A wave buffer as the renderer keeps it: the samples, the range to play, how it loops, and for DSP-ADPCM the context
// its start has, if the game gave one.
struct WaveBuffer
{
    std::span<const uint8_t> buffer;
    const AdpcmContext* context = nullptr;
    int32_t start = 0;
    int32_t end = 0;
    int32_t loop_start = 0;
    int32_t loop_end = 0;
    int32_t loop_count = 0; // the loops after the first time through, or -1 for ever
    bool loop = false;
    bool end_of_stream = false;
};

// A voice channel's processing state (nn::audio::VoiceState): the samples played, the place in the current wave buffer,
// which buffers are still to play, the converter's history and position, the DSP-ADPCM context, the current buffer's
// loops, the biquad filters' states, and the last sample mixed into each buffer of the destination, for the depop.
struct VoiceState
{
    uint64_t played = 0;
    int32_t offset = 0;
    int32_t index = 0;
    std::array<bool, kWaveBufferCount> valid{};
    int32_t consumed = 0;
    std::array<int16_t, 8> history{};
    int32_t fraction = 0;
    AdpcmContext adpcm;
    int32_t loop_count = 0;
    std::array<BiquadFilterState, 2> biquads;
    std::array<int32_t, 24> last_samples{};
};

// What a data source command gives the decoding (nn::audio::DecodeFromWaveBuffersArgs).
struct DecodeFromWaveBuffersArgs
{
    SampleFormat format = SampleFormat::kPcmInt16;
    int32_t* out = nullptr;
    VoiceState* state = nullptr;
    const std::array<WaveBuffer, kWaveBufferCount>* buffers = nullptr;
    int32_t channel = 0;
    int32_t channel_count = 1;
    SrcQuality quality = SrcQuality::kMedium;
    int32_t pitch = 0x8000; // Q15
    int32_t source_rate = 48000;
    int32_t target_rate = 48000;
    int32_t count = 240;
    const AdpcmCoefficients* coefficients = nullptr;
    bool reset_played_at_loop = false;
};

// Decodes and converts `count` samples of one channel into `out` (nn::audio::DecodeFromWaveBuffers, 0x6eda0). The
// buffers play in turn from the current one. A buffer that ends is consumed unless it loops. A looping buffer plays its
// loop range until the loops are done. When no buffer is left, the rest of the input is zeros, and the frame stops
// there. The state is the channel's processing state.
void DecodeFromWaveBuffers(const DecodeFromWaveBuffersArgs& args);

} // namespace hsf::audio
