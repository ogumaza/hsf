// SPDX-License-Identifier: MIT

#include "audio/voice.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace hsf::audio
{
namespace
{

// AArch64's 32-bit integer instructions wrap.
int32_t WrapAdd(int32_t a, int32_t b)
{
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

int32_t WrapMul(int32_t a, int32_t b)
{
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}

// The scratch buffer holds the converter's history and a batch's input: 0x3F00 samples at most.
constexpr int kScratchSamples = 0x3F00 + 8;

// One wave buffer's part of a frame: decodes from the channel's place in it, up to `wanted` samples.
int32_t Decode(const DecodeFromWaveBuffersArgs& args, const WaveBuffer& buffer, bool looped, int32_t offset,
               int32_t wanted, int16_t* out)
{
    DecodeArg arg;
    arg.buffer = buffer.buffer;
    arg.start = looped ? buffer.loop_start : buffer.start;
    arg.end = looped ? buffer.loop_end : buffer.end;
    arg.channel_count = args.channel_count;
    arg.channel = args.channel;
    arg.coefficients = args.coefficients;
    arg.context = &args.state->adpcm;
    arg.offset = offset;
    arg.count = wanted;
    switch (args.format)
    {
    case SampleFormat::kPcmInt16:
        return DecodePcm16(out, arg);
    case SampleFormat::kAdpcm:
        return DecodeAdpcm(out, arg);
    case SampleFormat::kPcmFloat:
        return DecodePcmFloat(out, arg);
    default:
        // Nintendo's code takes -1 samples from a buffer of any other format and goes round for ever. Here such a voice
        // is silent.
        return 0;
    }
}

} // namespace

void DecodeFromWaveBuffers(const DecodeFromWaveBuffersArgs& args)
{
    VoiceState& state = *args.state;
    const std::array<WaveBuffer, kWaveBufferCount>& buffers = *args.buffers;

    // The step from the rates and the pitch, in single precision, and nothing at all for a frame that runs backwards or
    // requires more input than the scratch buffer holds.
    const float rates = static_cast<float>(args.source_rate) / static_cast<float>(args.target_rate);
    const float step = rates * static_cast<float>(args.pitch);
    int32_t ratio = 0;
    ConvertToInt(&ratio, &step, 1);
    const int history = args.quality == SrcQuality::kHigh ? 8 : 4;
    const int32_t frame_end = WrapAdd(WrapMul(args.count, ratio), state.fraction);
    if (frame_end < 0 || history + (frame_end >> 15) > 0x3F00)
    {
        return;
    }

    // The frame goes in batches whose input fits: in practice, the whole frame at once.
    const int32_t limit = ratio == 0 ? 0 : (0x1F800000 - state.fraction) / ratio;
    const int32_t batch = limit < args.count ? limit : args.count;
    int32_t* out = args.out;
    int32_t remaining = args.count;
    int32_t fraction = state.fraction;
    int32_t offset = state.offset;
    int32_t index = state.index;
    uint64_t played = state.played;
    int32_t consumed = state.consumed;
    std::array<int16_t, kScratchSamples> scratch;

    // A buffer is done with: the next one plays.
    const auto consume = [&](const WaveBuffer& buffer)
    {
        state.valid[static_cast<std::size_t>(index)] = false;
        state.loop_count = 0;
        ++consumed;
        index = index > 2 ? 0 : index + 1;
        if (buffer.end_of_stream)
        {
            played = 0;
        }
    };

    while (remaining >= 1)
    {
        const int32_t count = remaining < batch ? remaining : batch;
        const int32_t batch_end = WrapAdd(WrapMul(count, ratio), fraction);
        int16_t* input = scratch.data();
        if (!args.skip_src)
        {
            for (int k = 0; k < history; ++k)
            {
                scratch[static_cast<std::size_t>(k)] = state.history[static_cast<std::size_t>(k)];
            }
            input += history;
        }

        // Decode the samples the batch requires, buffer after buffer.
        const int32_t need = batch_end >> 15;
        int32_t decoded = 0;
        bool starved = false;
        while (batch_end >= 0x8000)
        {
            if (index >= kWaveBufferCount)
            {
                state.valid = {};
                played = 0;
                index = 0;
                consumed = kWaveBufferCount;
            }
            if (!state.valid[static_cast<std::size_t>(index)])
            {
                starved = true;
                break;
            }

            const WaveBuffer& buffer = buffers[static_cast<std::size_t>(index)];
            if (offset == 0 && args.format == SampleFormat::kAdpcm && buffer.context != nullptr)
            {
                state.adpcm = *buffer.context;
            }
            const bool looped = buffer.loop && state.loop_count >= 1 && buffer.loop_end >= buffer.loop_start;
            const int32_t got = Decode(args, buffer, looped, offset, need - decoded, input + decoded);
            played += static_cast<uint64_t>(static_cast<int64_t>(got));
            decoded += got;
            if (got != 0)
            {
                offset += got;
                const int32_t length = looped ? buffer.loop_end - buffer.loop_start : buffer.end - buffer.start;
                if (offset < length)
                {
                    if (need > decoded)
                    {
                        continue;
                    }
                    break;
                }
            }

            // The buffer has ended, or gave nothing: it loops, or the next one plays.
            if (buffer.loop)
            {
                ++state.loop_count;
                if (buffer.loop_count >= 0 && (got == 0 || state.loop_count > buffer.loop_count))
                {
                    consume(buffer);
                }
                if (got == 0)
                {
                    offset = 0;
                    starved = true;
                    break;
                }
                if (args.reset_played_at_loop)
                {
                    played = 0;
                }
            }
            else
            {
                consume(buffer);
            }
            offset = 0;
            if (need <= decoded)
            {
                break;
            }
        }

        // Convert the batch, with zeros for what the buffers didn't give, and keep the history for the next one.
        if (args.skip_src)
        {
            for (int32_t i = 0; i < decoded; ++i)
            {
                out[i] = scratch[static_cast<std::size_t>(i)];
            }
        }
        else
        {
            for (int32_t i = decoded; i < need; ++i)
            {
                input[i] = 0;
            }
            Resample(out, scratch.data(), ratio, fraction, count, args.quality, need != count || args.pitch != 0x8000);
            for (int k = 0; k < history; ++k)
            {
                state.history[static_cast<std::size_t>(k)] = scratch[static_cast<std::size_t>(need + k)];
            }
        }

        remaining -= count;
        if (starved)
        {
            break;
        }
        out += count;
    }

    state.consumed = consumed;
    state.offset = offset;
    state.played = played;
    state.index = index;
    state.fraction = fraction;
}

} // namespace hsf::audio
