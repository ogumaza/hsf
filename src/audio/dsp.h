// SPDX-License-Identifier: MIT

// The signal processing of the Switch's audio renderer: decoding a voice's wave buffers, converting its sample rate,
// its biquad filters, volumes and mixes, and the depop. This follows the build of the renderer for the CPU that
// NintendoSDK's nn::audio holds, with its arithmetic to the bit. Each function names the routine it follows and its
// address in WarioWare: Get It Together!'s sdk. Samples in the mix buffers are 32-bit integers on the scale of 16-bit
// samples.

#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace hsf::audio
{

// nn::audio::SampleFormat. The renderer decodes PCM16, PCM float and DSP-ADPCM only.
enum class SampleFormat : uint8_t
{
    kInvalid = 0,
    kPcmInt8 = 1,
    kPcmInt16 = 2,
    kPcmInt24 = 3,
    kPcmInt32 = 4,
    kPcmFloat = 5,
    kAdpcm = 6,
};

// nn::audio::SrcQuality: how a voice's sample rate is converted to the renderer's rate.
enum class SrcQuality : uint8_t
{
    kMedium = 0, // 4 taps, the default
    kHigh = 1,   // 8 taps
    kLow = 2,    // the nearest sample
};

// DSP-ADPCM's state between samples (nn::audio::AdpcmContext): the header of the frame being decoded (the predictor in
// bits 4 to 7, the scale in bits 0 to 3), and the last two samples.
struct AdpcmContext
{
    uint16_t header = 0;
    int16_t yn1 = 0;
    int16_t yn2 = 0;
};

// DSP-ADPCM's coefficients, 8 pairs in Q11 (nn::audio::AdpcmParameter).
using AdpcmCoefficients = std::array<int16_t, 16>;

// What a decoder takes (nn::audio::DecodeArg): a wave buffer, the range to play in it, and the samples wanted from
// `offset` into the range. Offsets count samples, interleaved channels as one.
struct DecodeArg
{
    std::span<const uint8_t> buffer;
    int32_t start = 0;
    int32_t end = 0;
    int32_t channel_count = 1;
    int32_t channel = 0;
    const AdpcmCoefficients* coefficients = nullptr;
    AdpcmContext* context = nullptr;
    int32_t offset = 0;
    int32_t count = 0;
};

// The decoders write min(count, (end - start) - offset) samples and return how many, or 0 for an empty buffer or an
// inverted range. DSP-ADPCM takes the coefficients and keeps the context. A decode that starts inside a frame uses the
// context for that frame's header. DSP-ADPCM decodes nothing when the buffer is too short for the range's end. A frame
// header's predictor has 3 bits here: no encoder writes 8 or more, where Nintendo's code reads past the coefficients.
int32_t DecodePcm16(int16_t* out, const DecodeArg& arg); // nn::audio::DecodePcm16, 0x6f390
int32_t DecodeAdpcm(int16_t* out, const DecodeArg& arg); // nn::audio::DecodeAdpcm, 0x6f5e0

// PCM float: each sample times 32767 in single precision, towards zero and held to 32 bits, and then the low 16 bits of
// the result (nn::audio::DecodePcmFloat, 0x6f490). A sample beyond 1.0 therefore wraps.
int32_t DecodePcmFloat(int16_t* out, const DecodeArg& arg);

// Converts `count` samples of a voice to the renderer's rate (nn::audio::Resample, 0x7b3f0): `in` holds the history (4
// samples, or 8 for high quality) and then the new samples. `ratio` is the step in Q15, and `fraction` the position in
// Q15. The conversion advances `fraction`. When `converting` is false, the medium quality filters every output with the
// first one's phase and steps one input sample an output: the renderer requests that when the pitch is exactly 1 and
// the frame takes as many samples as it gives.
void Resample(int32_t* out, const int16_t* in, int32_t ratio, int32_t& fraction, int count, SrcQuality quality,
              bool converting);

// A biquad filter's state in floating point: the last two inputs and the last two outputs.
struct BiquadFilterState
{
    float x1 = 0.0f;
    float x2 = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;
};

// The biquad filter in floating point (nn::audio::dsp::ApplyBiquadFilter, 0x6d620): y = b0 x + b1 x1 + b2 x2 + a1 y1 +
// a2 y2 with the coefficients in Q14, the feedback terms added. The filter computes four samples at a time from
// coefficients derived for each place in the block. The rounding therefore differs from the plain recursion's rounding.
// `count` is a multiple of 4, and `out` may be `in`.
void ApplyBiquadFilter(float* out, const float* in, const std::array<int16_t, 3>& b, const std::array<int16_t, 2>& a,
                       BiquadFilterState& state, int count);

// The conversions around the floating-point biquad (nn::audio::dsp::ConvertFormat, 0x70db0 and 0x70f50): to float to
// the nearest, and back towards zero, held to 32 bits.
void ConvertToFloat(float* out, const int32_t* in, int count);
void ConvertToInt(int32_t* out, const float* in, int count);

// The gains below are in Q`q` (q is 15 or 23). Each product is rounded half up and held to 32 bits, and each sum into a
// buffer is held to 32 bits.

// out = in × gain (nn::audio::dsp::ApplyUniformGain, 0x7b9a0).
void ApplyUniformGain(int32_t* out, const int32_t* in, int32_t gain, int count, int q);

// out = in × (gain + i × delta) for sample i (nn::audio::dsp::ApplyLinearEnvelopeGain, 0x7bdb0).
void ApplyLinearEnvelopeGain(int32_t* out, const int32_t* in, int32_t gain, int32_t delta, int count, int q);

// out += in × gain (nn::audio::dsp::ApplyMix, 0x79de0).
void ApplyMix(int32_t* out, const int32_t* in, int32_t gain, int count, int q);

// out += in × (gain + i × delta) (nn::audio::dsp::ApplyMixRamp, 0x7a350). Returns the last product, rounded but not
// held. The depop takes that product when the voice stops.
int32_t ApplyMixRamp(int32_t* out, const int32_t* in, int32_t gain, int32_t delta, int count, int q);

// Adds a decaying step to a mix buffer (nn::audio::dsp::ApplyDepopMix, 0x6ffc0): from `depop`, each sample decays by
// `decay` (Q15) before it is added. Returns what is left for the next frame.
int32_t ApplyDepopMix(int32_t* buffer, int32_t depop, int32_t decay, int count);

} // namespace hsf::audio
