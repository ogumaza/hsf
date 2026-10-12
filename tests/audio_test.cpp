// SPDX-License-Identifier: MIT

// Checks the model of the audio renderer without game data: the decoders on hand-made buffers, the sample-rate
// converter's filters at a ratio of 1, the biquad on filters whose output is known, the gains, mixes and depop against
// their formulas, a voice's frames through the renderer (its samples, what the updates report and when, and the fade
// when it stops), and the I3DL2 reverb with a density below 0.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "xsflib/testing/check.h"

#include "audio/dsp.h"
#include "audio/i3dl2.h"
#include "audio/renderer.h"

using namespace xsflib::testing;

namespace
{

namespace audio = hsf::audio;

constexpr int kFrame = 240;
constexpr int32_t kUnity = 1 << 23; // 1.0 in Q23

void TestPcm16()
{
    // Samples 2 to 5 of a buffer of 8, and the same range's second channel in an interleaved buffer.
    std::vector<uint8_t> bytes;
    for (int16_t s = 0; s < 16; ++s)
    {
        const uint16_t value = static_cast<uint16_t>(s * 1000 - 3000);
        bytes.push_back(static_cast<uint8_t>(value & 0xFF));
        bytes.push_back(static_cast<uint8_t>(value >> 8));
    }
    audio::DecodeArg arg;
    arg.buffer = bytes;
    arg.start = 2;
    arg.end = 6;
    arg.offset = 1;
    arg.count = 10;
    std::array<int16_t, 8> out{};

    const int32_t decoded = audio::DecodePcm16(out.data(), arg);

    XSFLIB_CHECK(decoded == 3);
    XSFLIB_CHECK(out[0] == 0 && out[1] == 1000 && out[2] == 2000);

    arg.channel_count = 2;
    arg.channel = 1;
    arg.offset = 0;
    XSFLIB_CHECK(audio::DecodePcm16(out.data(), arg) == 4);
    XSFLIB_CHECK(out[0] == 2000 && out[1] == 4000 && out[3] == 8000);

    arg.end = 1;
    XSFLIB_CHECK(audio::DecodePcm16(out.data(), arg) == 0);
}

void TestAdpcm()
{
    // One frame with predictor 1 (coefficients 2048, 0: the last sample plus the nibble) and scale 4: samples 16, 32,
    // ..., then a nibble of -8 takes 128 away.
    audio::AdpcmCoefficients coefs{};
    coefs[2] = 2048;
    std::vector<uint8_t> frame = {0x14, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x88};
    audio::AdpcmContext context;
    audio::DecodeArg arg;
    arg.buffer = frame;
    arg.end = 14;
    arg.count = 14;
    arg.coefficients = &coefs;
    arg.context = &context;
    std::array<int16_t, 14> out{};

    XSFLIB_CHECK(audio::DecodeAdpcm(out.data(), arg) == 14);

    XSFLIB_CHECK(out[0] == 16 && out[1] == 32 && out[11] == 192);
    XSFLIB_CHECK(out[12] == 64 && out[13] == -64);
    XSFLIB_CHECK(context.header == 0x14 && context.yn1 == -64 && context.yn2 == 64);

    // From sample 5, the context gives the frame's header and the two samples before it.
    context = {0x14, 80, 64};
    arg.offset = 5;
    arg.count = 2;
    XSFLIB_CHECK(audio::DecodeAdpcm(out.data(), arg) == 2);
    XSFLIB_CHECK(out[0] == 96 && out[1] == 112);

    // A buffer too short for the range's end gives nothing.
    arg.end = 16;
    arg.offset = 0;
    XSFLIB_CHECK(audio::DecodeAdpcm(out.data(), arg) == 0);
}

void TestResample()
{
    // At a ratio of 1 the medium filter is row 0 of its first table, and the high one row 0 of its first table: a
    // constant comes out at their gains, 32635 / 32768 and 32735 / 32768.
    std::vector<int16_t> in(kFrame + 8, 10000);
    std::vector<int32_t> out(kFrame);
    int32_t fraction = 0;
    audio::Resample(out.data(), in.data(), 0x8000, fraction, kFrame, audio::SrcQuality::kMedium, true);
    XSFLIB_CHECK(out[0] == 9959 && out[kFrame - 1] == 9959 && fraction == 0);
    audio::Resample(out.data(), in.data(), 0x8000, fraction, kFrame, audio::SrcQuality::kHigh, true);
    XSFLIB_CHECK(out[0] == 9989 && fraction == 0);

    // The low quality takes the nearer sample: at half a sample a step, each input twice.
    for (int i = 0; i < kFrame; ++i)
    {
        in[i] = static_cast<int16_t>(i);
    }
    fraction = 0;
    audio::Resample(out.data(), in.data(), 0x4000, fraction, 8, audio::SrcQuality::kLow, true);
    XSFLIB_CHECK(out[0] == 0 && out[1] == 1 && out[2] == 1 && out[3] == 2 && fraction == 0);
}

void TestBiquad()
{
    // b0 = 1 passes the samples as they are, and a one-pole low-pass with a1 = 0.5 settles at a constant input.
    std::vector<float> in(kFrame, 1000.0f);
    std::vector<float> out(kFrame);
    audio::BiquadFilterState state;
    audio::ApplyBiquadFilter(out.data(), in.data(), {16384, 0, 0}, {0, 0}, state, kFrame);
    XSFLIB_CHECK(out[0] == 1000.0f && out[kFrame - 1] == 1000.0f);

    state = {};
    audio::ApplyBiquadFilter(out.data(), in.data(), {8192, 0, 0}, {8192, 0}, state, kFrame);
    XSFLIB_CHECK(out[0] == 500.0f && out[1] == 750.0f);
    XSFLIB_CHECK(out[kFrame - 1] > 999.999f && out[kFrame - 1] < 1000.001f && state.x1 == 1000.0f);
}

void TestGains()
{
    // Halving rounds half up, and doubling the largest sample holds it.
    const std::array<int32_t, 4> kIn = {3, -3, 2147483647, -5};
    std::array<int32_t, 4> out{};
    audio::ApplyUniformGain(out.data(), kIn.data(), kUnity / 2, 4, 23);
    XSFLIB_CHECK(out[0] == 2 && out[1] == -1 && out[3] == -2);
    audio::ApplyUniformGain(out.data(), kIn.data(), kUnity * 2, 4, 23);
    XSFLIB_CHECK(out[2] == 2147483647);

    // A ramp from 0 to 1 steps by trunc(1 / 240) and returns its last product.
    std::vector<int32_t> samples(kFrame, 10000);
    std::vector<int32_t> mixed(kFrame, 5);
    const int32_t kDelta = kUnity / kFrame;
    const int32_t last = audio::ApplyMixRamp(mixed.data(), samples.data(), 0, kDelta, kFrame, 23);
    const int64_t expected = (int64_t{10000} * (kDelta * (kFrame - 1)) + (1 << 22)) >> 23;
    XSFLIB_CHECK(mixed[0] == 5 && last == expected && mixed[kFrame - 1] == 5 + expected);

    audio::ApplyMix(mixed.data(), samples.data(), kUnity, kFrame, 23);
    XSFLIB_CHECK(mixed[0] == 10005);
}

void TestDepop()
{
    // The step decays by 0.962 a sample from 10000, and runs on into the next frame.
    std::vector<int32_t> buffer(kFrame, 0);
    const int32_t left = audio::ApplyDepopMix(buffer.data(), 10000, 31529, kFrame);
    XSFLIB_CHECK(buffer[0] == 9621 && buffer[1] == 9257);
    XSFLIB_CHECK(buffer[2] < buffer[1] && buffer[kFrame - 1] >= 0 && left == buffer[kFrame - 1]);

    std::vector<int32_t> negative(kFrame, 0);
    XSFLIB_CHECK(audio::ApplyDepopMix(negative.data(), -10000, 31529, kFrame) == -left);
    XSFLIB_CHECK(negative[0] == -9621 && negative[kFrame - 1] == -left);
}

// A mono PCM16 voice at the renderer's rate, at full volume to the final mix's first buffer, playing samples of 10000.
struct OneVoice
{
    OneVoice() : renderer(Parameter())
    {
        for (int i = 0; i < kFrame * 8; ++i)
        {
            const uint16_t kValue = 10000;
            samples.push_back(static_cast<uint8_t>(kValue & 0xFF));
            samples.push_back(static_cast<uint8_t>(kValue >> 8));
        }
        renderer.AcquireFinalMix(2);
        const int kInputs[] = {0, 1};
        renderer.AddDeviceSink(kInputs);
        voice = renderer.AcquireVoiceSlot(48000, 1, audio::SampleFormat::kPcmInt16, 0, nullptr);
        renderer.SetVoiceDestination(voice, audio::Renderer::kFinalMix);
        renderer.SetVoiceMixVolume(voice, 1.0f, 0, 0);
    }

    static audio::RendererParameter Parameter()
    {
        audio::RendererParameter parameter;
        parameter.mix_buffer_count = 2;
        parameter.voice_count = 4;

        return parameter;
    }

    // Appends samples [start, end) of the buffer.
    void Append(int start, int end, const void* tag)
    {
        audio::WaveBufferInfo buffer;
        buffer.buffer = samples;
        buffer.start = start;
        buffer.end = end;
        buffer.tag = tag;
        renderer.AppendWaveBuffer(voice, buffer);
    }

    std::vector<uint8_t> samples;
    audio::Renderer renderer;
    int voice = -1;
};

void TestRendererSamples()
{
    // At a ratio of 1 the medium filter's phase is 0: the filter's first row on the history (zeros) and the new
    // samples. The voice therefore comes in over 4 samples and then holds at 10000 × 32635 / 32768.
    OneVoice one;
    one.Append(0, kFrame * 8, nullptr);
    one.renderer.SetVoicePlayState(one.voice, audio::PlayState::kStarted);

    one.renderer.RenderFrame();

    const std::span<const int16_t> out = one.renderer.DeviceOutput();
    XSFLIB_CHECK(out.size() == static_cast<std::size_t>(kFrame) * 2);
    XSFLIB_CHECK(out[0] == 0 && out[2] == -2 && out[4] == 19 && out[6] == 9980 && out[8] == 9959);
    XSFLIB_CHECK(out[2 * (kFrame - 1)] == 9959 && out[1] == 0 && out[2 * kFrame - 1] == 0);
}

void TestRendererStatuses()
{
    // The updates report the state two frames late: the samples played, and a buffer done with.
    OneVoice one;
    constexpr int kTag = 0;
    constexpr int kSecondTag = 1;
    one.Append(0, kFrame, &kTag);
    one.Append(kFrame, kFrame * 8, &kSecondTag);
    one.renderer.SetVoicePlayState(one.voice, audio::PlayState::kStarted);
    std::vector<uint64_t> played;
    std::vector<const void*> released;

    for (int frame = 0; frame < 4; ++frame)
    {
        one.renderer.RenderFrame();
        played.push_back(one.renderer.GetVoicePlayedSampleCount(one.voice));
        released.push_back(one.renderer.GetReleasedWaveBuffer(one.voice));
    }

    XSFLIB_CHECK(played[0] == 0 && played[1] == 0 && played[2] == 240 && played[3] == 480);
    XSFLIB_CHECK(released[0] == nullptr && released[1] == nullptr && released[2] == &kTag && released[3] == nullptr);
}

void TestRendererStop()
{
    // A voice that stops leaves its last sample to fade out by the depop, and plays no more.
    OneVoice one;
    one.Append(0, kFrame * 8, nullptr);
    one.renderer.SetVoicePlayState(one.voice, audio::PlayState::kStarted);
    one.renderer.RenderFrame();
    one.renderer.RenderFrame();
    one.renderer.SetVoicePlayState(one.voice, audio::PlayState::kStopped);

    one.renderer.RenderFrame();

    const std::span<const int16_t> out = one.renderer.DeviceOutput();
    XSFLIB_CHECK(out[0] == (9959 * 31529) >> 15);
    bool falling = true;
    for (int t = 1; t < kFrame; ++t)
    {
        falling = falling && out[2 * t] <= out[2 * (t - 1)] && out[2 * t] >= 0;
    }
    XSFLIB_CHECK(falling && out[2 * (kFrame - 1)] < 10);

    // The span shows the renderer's buffer. The next frame overwrites it.
    const int16_t last = out[2 * (kFrame - 1)];
    one.renderer.RenderFrame();

    const std::span<const int16_t> next = one.renderer.DeviceOutput();
    bool quiet = true;
    for (int t = 0; t < kFrame; ++t)
    {
        quiet = quiet && next[2 * t] >= 0 && next[2 * t] <= last;
    }
    XSFLIB_CHECK(quiet);
}

// The output of a new reverb at the density given, for 64 frames (320 ms) from a click. The feedback lines' longest
// delays reach 272 ms.
std::vector<int32_t> I3dl2Click(float density)
{
    audio::AuxI3dl2Reverb reverb(48'000);
    reverb.SetInputOutput({0, 1}, {0, 1});
    audio::I3dl2ReverbParameterSet parameters;
    parameters.late_reverb_decay_time = 1.0f;
    parameters.late_reverb_hf_decay_ratio = 0.5f;
    parameters.late_reverb_density = density;
    parameters.hf_reference = 5000.0f;
    parameters.dry_gain = 1.0f;
    reverb.SetParameters(parameters);

    std::vector<int32_t> samples(static_cast<std::size_t>(kFrame) * 2);
    samples[0] = 0x4000;
    samples[kFrame] = 0x4000;

    std::vector<int32_t> output;
    for (int frame = 0; frame < 64; ++frame)
    {
        reverb.Process(samples.data(), samples.data(), kFrame);
        output.insert(output.end(), samples.begin(), samples.end());
        std::fill(samples.begin(), samples.end(), 0);
    }

    return output;
}

void TestI3dl2NegativeDelay()
{
    // A density below 0 gives the feedback lines delays below 0. The lines of a new reverb then keep their longest
    // delays. A density of 100 gives the same delays. A density of 50 gives shorter ones. A build with AddressSanitizer
    // also checks that the reverb writes only inside its buffer.
    const std::vector<int32_t> longest = I3dl2Click(100.0f);
    const std::vector<int32_t> shorter = I3dl2Click(50.0f);

    const std::vector<int32_t> below = I3dl2Click(-1000.0f);

    XSFLIB_CHECK(below == longest);
    XSFLIB_CHECK(below != shorter);
}

} // namespace

int main()
{
    TestPcm16();
    TestAdpcm();
    TestResample();
    TestBiquad();
    TestGains();
    TestDepop();
    TestRendererSamples();
    TestRendererStatuses();
    TestRendererStop();
    TestI3dl2NegativeDelay();
    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);

        return 1;
    }

    return 0;
}
