// SPDX-License-Identifier: MIT

#include "audio/dsp.h"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "ntdlib/wave/adpcm.h"

#include "audio/tables.h"

namespace hsf::audio
{
namespace
{

// AArch64's 32-bit integer instructions wrap.
int32_t WrapAdd(int32_t a, int32_t b)
{
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

int32_t WrapSub(int32_t a, int32_t b)
{
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}

int32_t WrapMul(int32_t a, int32_t b)
{
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}

// The low 32 bits of a 64-bit value.
int32_t Low32(int64_t x)
{
    return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(x)));
}

int32_t Saturate(int64_t x)
{
    if (x > std::numeric_limits<int32_t>::max())
    {
        return std::numeric_limits<int32_t>::max();
    }
    if (x < std::numeric_limits<int32_t>::min())
    {
        return std::numeric_limits<int32_t>::min();
    }

    return static_cast<int32_t>(x);
}

// A 64-bit product rounded half up to Q0 and held to 32 bits (sqrshrn).
int32_t RoundShift(int64_t product, int q)
{
    return Saturate((product + (int64_t{1} << (q - 1))) >> q);
}

int32_t SaturatingAdd(int32_t a, int32_t b)
{
    return Saturate(int64_t{a} + b);
}

int32_t SaturatingSub(int32_t a, int32_t b)
{
    return Saturate(int64_t{a} - b);
}

// A byte or a 16-bit sample of a wave buffer, or 0 beyond it. Nintendo's code reads whatever lies there; the renderer
// has checked the range against the buffer's size before.
uint8_t ByteAt(std::span<const uint8_t> buffer, int64_t index)
{
    if (index < 0 || static_cast<uint64_t>(index) >= buffer.size())
    {
        return 0;
    }

    return buffer[static_cast<std::size_t>(index)];
}

int16_t SampleAt(std::span<const uint8_t> buffer, int64_t index)
{
    const uint16_t low = ByteAt(buffer, index * 2);
    const uint16_t high = ByteAt(buffer, index * 2 + 1);
    return static_cast<int16_t>(low | (high << 8));
}

// The nibble where a DSP-ADPCM sample lies: frames of 16 nibbles hold a header in their first two and 14 samples. A
// sample at a frame's start gives the header's place.
int32_t NibbleOf(int32_t sample)
{
    const int32_t frame = sample / 14;
    const int32_t in_frame = sample - frame * 14;
    return in_frame + frame * 16 + (in_frame != 0 ? 2 : 0);
}

// The sample-rate converter's filters: `taps` coefficients from a table chosen by the ratio, applied to the samples at
// the position, in 32-bit sums of the products, shifted down by 15.
void ResampleFiltered(int32_t* out, const int16_t* in, int32_t ratio, int32_t& fraction, int count, int taps,
                      const int16_t* table)
{
    for (int i = 0; i < count; ++i)
    {
        const int16_t* row = table + (fraction >> 8) * taps;
        int32_t sum = 0;
        for (int k = 0; k < taps; ++k)
        {
            sum = WrapAdd(sum, WrapMul(in[k], row[k]));
        }
        out[i] = sum >> 15;

        fraction = WrapAdd(fraction, ratio);
        in += fraction >> 15;
        fraction &= 0x7FFF;
    }
}

// The table for a ratio: up to 1, below 4/3, and from 4/3 up.
int TableFor(int32_t ratio)
{
    if (ratio <= 0x8000)
    {
        return 0;
    }

    return ratio < 0xAAAB ? 1 : 2;
}

} // namespace

int32_t DecodePcm16(int16_t* out, const DecodeArg& arg)
{
    if (arg.buffer.data() == nullptr || arg.buffer.empty() || arg.end < arg.start)
    {
        return 0;
    }

    const int32_t left = WrapSub(WrapSub(arg.end, arg.start), arg.offset);
    const int32_t count = arg.count < left ? arg.count : left;
    const int64_t first = int64_t{WrapMul(WrapAdd(arg.start, arg.offset), arg.channel_count)};
    for (int32_t i = 0; i < count; ++i)
    {
        const int64_t index = arg.channel_count == 1 ? first + i : first + int64_t{i} * arg.channel_count + arg.channel;
        out[i] = SampleAt(arg.buffer, index);
    }

    return count;
}

int32_t DecodePcmFloat(int16_t* out, const DecodeArg& arg)
{
    if (arg.buffer.data() == nullptr || arg.buffer.empty() || arg.end < arg.start)
    {
        return 0;
    }

    const int32_t left = WrapSub(WrapSub(arg.end, arg.start), arg.offset);
    const int32_t count = arg.count < left ? arg.count : left;
    const int64_t first = int64_t{WrapMul(WrapAdd(arg.start, arg.offset), arg.channel_count)};
    for (int32_t i = 0; i < count; ++i)
    {
        const int64_t index = arg.channel_count == 1 ? first + i : first + int64_t{i} * arg.channel_count + arg.channel;
        uint32_t bits = 0;
        for (int b = 0; b < 4; ++b)
        {
            bits |= static_cast<uint32_t>(ByteAt(arg.buffer, index * 4 + b)) << (8 * b);
        }
        const float scaled = std::bit_cast<float>(bits) * 32767.0f;
        int32_t value = 0;
        ConvertToInt(&value, &scaled, 1);
        out[i] = static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint32_t>(value)));
    }

    return count;
}

int32_t DecodeAdpcm(int16_t* out, const DecodeArg& arg)
{
    if (arg.buffer.data() == nullptr || arg.buffer.empty() || arg.end < arg.start)
    {
        return 0;
    }

    // The buffer must hold every byte up to the range's end.
    const int32_t end_nibble = NibbleOf(arg.end);
    const int64_t end_bytes = (int64_t{end_nibble} + 1) / 2;
    if (static_cast<uint64_t>(end_bytes) > arg.buffer.size())
    {
        return 0;
    }

    // Start from the context: its header gives the predictor and scale for a decode that starts inside a frame.
    const int32_t position = WrapAdd(arg.offset, arg.start);
    const int32_t left = WrapSub(WrapSub(arg.end, arg.start), arg.offset);
    const int32_t count = arg.count < left ? arg.count : left;
    const AdpcmCoefficients& coefs = *arg.coefficients;
    uint16_t header = arg.context->header;
    int32_t predictor = (header >> 4) & 7;
    int32_t scale = header & 0xF;
    int32_t yn1 = arg.context->yn1;
    int32_t yn2 = arg.context->yn2;
    int32_t nibble = NibbleOf(position);
    int64_t byte = nibble / 2;

    for (int32_t done = 0; done < count;)
    {
        // A frame's header, at its start.
        if ((nibble & 0xF) == 0)
        {
            header = ByteAt(arg.buffer, byte);
            predictor = (header >> 4) & 7;
            scale = header & 0xF;
            ++byte;
            nibble += 2;
        }

        // The high nibble of a byte, then the low one. The renderer adds up each sample in 32 bits that wrap.
        const uint8_t pair = ByteAt(arg.buffer, byte);
        const int32_t value = (nibble & 1) == 0 ? pair >> 4 : pair & 0xF;
        const int32_t sample =
            ntdlib::wave::DspAdpcmSample(value >= 8 ? value - 16 : value, scale, coefs[2 * predictor],
                                         coefs[2 * predictor + 1], yn1, yn2, ntdlib::wave::DspAdpcmSum::kWrap);
        out[done++] = static_cast<int16_t>(sample);
        yn2 = yn1;
        yn1 = sample;

        if ((nibble & 1) != 0)
        {
            ++byte;
        }
        ++nibble;
    }

    arg.context->header = header;
    arg.context->yn1 = static_cast<int16_t>(yn1);
    arg.context->yn2 = static_cast<int16_t>(yn2);

    return count;
}

void Resample(int32_t* out, const int16_t* in, int32_t ratio, int32_t& fraction, int count, SrcQuality quality,
              bool converting)
{
    switch (quality)
    {
    case SrcQuality::kMedium:
        if (converting)
        {
            ResampleFiltered(out, in, ratio, fraction, count, 4, &tables::kResampleMedium[TableFor(ratio)][0][0]);
        }
        else
        {
            const int16_t* row = tables::kResampleMedium[TableFor(ratio)][fraction >> 8];
            for (int i = 0; i < count; ++i)
            {
                int32_t sum = 0;
                for (int k = 0; k < 4; ++k)
                {
                    sum = WrapAdd(sum, WrapMul(in[i + k], row[k]));
                }
                out[i] = sum >> 15;
            }
            fraction = WrapAdd(fraction, WrapMul(count, ratio)) & 0x7FFF;
        }
        break;

    case SrcQuality::kHigh:
        ResampleFiltered(out, in, ratio, fraction, count, 8, &tables::kResampleHigh[TableFor(ratio)][0][0]);
        break;

    case SrcQuality::kLow:
        // The nearer of the two samples around the position, the later one from half a sample.
        for (int i = 0; i < count; ++i)
        {
            out[i] = in[fraction < 0x4000 ? 0 : 1];
            fraction = WrapAdd(fraction, ratio);
            in += fraction >> 15;
            fraction &= 0x7FFF;
        }
        break;
    }
}

void ApplyBiquadFilter(float* out, const float* in, const std::array<int16_t, 3>& b, const std::array<int16_t, 2>& a,
                       BiquadFilterState& state, int count)
{
    // The coefficients for each place in a block of four, worked out as Nintendo's code does, operation for operation
    // (the zero terms come from its general form and only change the signs of zeros).
    constexpr float kQ14 = 6.103515625e-05f;
    constexpr float kZero = 0.0f;
    const float a1 = static_cast<float>(a[0]) * kQ14;
    const float b0 = static_cast<float>(b[0]) * kQ14;
    const float b1 = static_cast<float>(b[1]) * kQ14;
    const float b2 = static_cast<float>(b[2]) * kQ14;
    const float a2 = static_cast<float>(a[1]) * kQ14;

    const float a1_zero = a1 * kZero;
    const float b1_a1 = b1 * a1;
    const float b2_a1 = b2 * a1;
    const float a1_a1 = a1 * a1;
    const float b0_a1 = b0 * a1;
    const float a2_zero = a2 * kZero;
    const float a1_a2 = a1 * a2;
    const float t29 = a1_a2 + kZero;
    const float t21 = a1_zero + kZero;
    const float t19 = b0 + a1_zero;
    const float t24 = b2 + b1_a1;
    const float t31a = a1 * t21;
    const float t30a = a1 * t19;
    const float t8 = b1 + b0_a1;
    const float t22 = a2 + a1_a1;
    const float t20a = a2 * t24;
    const float t28a = a2 * a2;
    const float t9a = a2 * t21;
    const float t14 = a2 * t8;
    const float t11a = a1 * t24;
    const float t31 = a2_zero + t31a;
    const float t18a = a2_zero + t30a;
    const float t30 = b2_a1 + kZero;
    const float t23a = b0 * a2;
    const float t25a = b1 * a2;
    const float t26a = b2 * a2;
    const float t10a = a2 * t19;
    const float t17 = a1 * t8;
    const float t12 = a2 * t22;
    const float t19b = a2 * t29;
    const float t15a = a1 * t22;
    const float t27a = a1_a2 + t15a;
    const float t20b = a2 * t30;
    const float t13a = a1 * t30;
    const float t29b = a1 * t29;
    const float t23b = t23a + t17;
    const float t25b = t25a + t11a;
    const float t18 = b1 + t18a;
    const float t23 = b2 + t23b;
    const float t26b = t26a + t13a;
    const float t28b = t28a + t29b;
    const float t29c = t31 + kZero;
    const float t31b = b0 + t31;
    const float t25 = t25b + kZero;
    const float t26 = t26b + kZero;
    const float t27 = t27a + kZero;
    const float t28 = t28b + kZero;
    const float t11b = a1 * t29c;
    const float t13b = a1 * t31b;
    const float t15b = a1 * t18;
    const float t29d = a1 * t23;
    const float t19c = a1 * t25;
    const float t18b = a1 * t27;
    const float t18c = t12 + t18b;
    const float t10b = t10a + t15b;
    const float t20c = a1 * t26;
    const float t21b = a1 * t28;
    const float t22b = t9a + t11b;
    const float t9b = t9a + t13b;
    const float t18d = t18c + kZero;
    const float t27b = b1 + t9b;
    const float t22c = b0 + t22b;
    const float t24b = b2 + t10b;
    const float t22d = t14 + t29d;
    const float t18e = t20a + t19c;
    const float t19d = t20b + t20c;
    const float t20d = t19b + t21b;
    const float t18f = t18e + kZero;
    const float t21c = t22d + kZero;
    const float t18g = t19d + kZero;
    const float t16 = t20d + kZero;

    // The weights of each input, the last two inputs and the last two outputs, for the four outputs of a block.
    const float in0[4] = {b0, t8, t23, t21c};
    const float in1[4] = {kZero, t19, t18, t24b};
    const float in2[4] = {kZero, t21, t31b, t27b};
    const float in3[4] = {kZero, t21, t29c, t22c};
    const float x1w[4] = {b1, t24, t25, t18f};
    const float x2w[4] = {b2, t30, t26, t18g};
    const float y1w[4] = {a1, t22, t27, t18d};
    const float y2w[4] = {a2, t29, t28, t16};

    float x1 = state.x1;
    float x2 = state.x2;
    float y1 = state.y1;
    float y2 = state.y2;
    for (int block = 0; block + 4 <= count; block += 4)
    {
        // The block's inputs, read before its outputs are stored: `out` may be `in`.
        float x[4];
        std::memcpy(x, in + block, sizeof(x));
        float y[4];
        for (int j = 0; j < 4; ++j)
        {
            float sum = in2[j] * x[2] + in3[j] * x[3];
            sum = in1[j] * x[1] + sum;
            sum = in0[j] * x[0] + sum;
            sum = x1w[j] * x1 + sum;
            sum = x2w[j] * x2 + sum;
            sum = y1w[j] * y1 + sum;
            y[j] = y2w[j] * y2 + sum;
        }
        std::memcpy(out + block, y, sizeof(y));

        x2 = x[2];
        x1 = x[3];
        y2 = y[2];
        y1 = y[3];
    }

    state = {x1, x2, y1, y2};
}

void ConvertToFloat(float* out, const int32_t* in, int count)
{
    for (int i = 0; i < count; ++i)
    {
        out[i] = static_cast<float>(in[i]);
    }
}

void ConvertToInt(int32_t* out, const float* in, int count)
{
    for (int i = 0; i < count; ++i)
    {
        const float x = in[i];
        if (std::isnan(x))
        {
            out[i] = 0;
        }
        else if (x >= 2147483648.0f)
        {
            out[i] = std::numeric_limits<int32_t>::max();
        }
        else if (x < -2147483648.0f)
        {
            out[i] = std::numeric_limits<int32_t>::min();
        }
        else
        {
            out[i] = static_cast<int32_t>(x);
        }
    }
}

void ApplyUniformGain(int32_t* out, const int32_t* in, int32_t gain, int count, int q)
{
    for (int i = 0; i < count; ++i)
    {
        out[i] = RoundShift(int64_t{in[i]} * gain, q);
    }
}

void ApplyLinearEnvelopeGain(int32_t* out, const int32_t* in, int32_t gain, int32_t delta, int count, int q)
{
    for (int i = 0; i < count; ++i)
    {
        out[i] = RoundShift(int64_t{in[i]} * WrapAdd(gain, WrapMul(i, delta)), q);
    }
}

void ApplyMix(int32_t* out, const int32_t* in, int32_t gain, int count, int q)
{
    for (int i = 0; i < count; ++i)
    {
        out[i] = SaturatingAdd(out[i], RoundShift(int64_t{in[i]} * gain, q));
    }
}

int32_t ApplyMixRamp(int32_t* out, const int32_t* in, int32_t gain, int32_t delta, int count, int q)
{
    if (count < 1)
    {
        return 0;
    }

    for (int i = 0; i < count; ++i)
    {
        out[i] = SaturatingAdd(out[i], RoundShift(int64_t{in[i]} * WrapAdd(gain, WrapMul(i, delta)), q));
    }

    const int64_t last = int64_t{in[count - 1]} * WrapAdd(gain, WrapMul(count - 1, delta));
    return Low32((last + (int64_t{1} << (q - 1))) >> q);
}

int32_t ApplyDepopMix(int32_t* buffer, int32_t depop, int32_t decay, int count)
{
    // The step decays before each sample: by `decay` at a time for an odd count, with sums that wrap. For an even
    // count, the first two samples so, then each of two interleaved steps by decay squared (Q30), truncated, with sums
    // held to 32 bits.
    const bool negative = depop < 1;
    const int32_t start = negative ? WrapSub(0, depop) : depop;
    if (count < 1)
    {
        return depop;
    }

    if (count % 2 != 0)
    {
        int32_t value = start;
        for (int i = 0; i < count; ++i)
        {
            value = Low32((int64_t{value} * decay) >> 15);
            buffer[i] = negative ? WrapSub(buffer[i], value) : WrapAdd(buffer[i], value);
        }

        return negative ? WrapSub(0, value) : value;
    }

    const int32_t first = Low32((int64_t{start} * decay) >> 15);
    const int32_t second = Low32((int64_t{first} * decay) >> 15);
    buffer[0] = negative ? WrapSub(buffer[0], first) : WrapAdd(buffer[0], first);
    buffer[1] = negative ? WrapSub(buffer[1], second) : WrapAdd(buffer[1], second);

    const int32_t squared = WrapMul(decay, decay);
    int32_t lanes[2] = {first, second};
    for (int i = 2; i < count; i += 2)
    {
        for (int lane = 0; lane < 2; ++lane)
        {
            lanes[lane] = Saturate((int64_t{lanes[lane]} * squared) >> 30);
            buffer[i + lane] =
                negative ? SaturatingSub(buffer[i + lane], lanes[lane]) : SaturatingAdd(buffer[i + lane], lanes[lane]);
        }
    }

    return negative ? WrapSub(0, lanes[1]) : lanes[1];
}

} // namespace hsf::audio
