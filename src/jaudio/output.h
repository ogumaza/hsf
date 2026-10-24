// SPDX-License-Identifier: MIT

// Hagi's path from the emulated audio interface to the Switch's AudioOut in the Super Mario Galaxy games' `main`. The
// audio interface plays the DSP's output at 32,000 Hz. Hagi converts each block of the interface's samples to 48,000 Hz
// by linear interpolation as the block arrives. AudioOut then plays the result at a gain of 127/128.
//
// AudioOut has a resampler too. It follows the fill of AudioOut's ring and moves its rate between 0.99 and 1.01. At a
// rate of 1, it copies the samples. A rip has no clock to follow. The model therefore takes that rate.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace hsf::jaudio
{

class AudioOut
{
public:
    // The rates of Hagi's resampler, and the step of its fraction: 1/1.5 in 2.14.
    static constexpr int32_t kInputRate = 32000;
    static constexpr int32_t kOutputRate = 48000;
    static constexpr int32_t kStep = (kInputRate << 14) / kOutputRate;

    // AudioOut's gain from Hagi's volume of 100: e^(4.65 v) / e^4.65 for v = 1, in 1.7 below 1.
    static constexpr int32_t kGain = 127;

    // Appends the 48 kHz output of `samples`, interleaved stereo pairs at 32 kHz, to `out`. The first pair gives no
    // output. Each later pair gives one or two pairs: 1.5 a pair on average.
    void Push(std::span<const int16_t> samples, std::vector<int16_t>& out);

private:
    int32_t phase_ = 0;
    int32_t fraction_ = 0;
    std::array<int16_t, 2> last_{};
};

} // namespace hsf::jaudio
