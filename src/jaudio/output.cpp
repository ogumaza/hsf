// SPDX-License-Identifier: MIT

// Hagi's path from the audio interface to AudioOut (see output.h).

#include "jaudio/output.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hsf::jaudio
{

void AudioOut::Push(std::span<const int16_t> samples, std::vector<int16_t>& out)
{
    // The interface's samples come to the resampler in blocks. Each input sample gives the outputs that fall between
    // it and the sample before it. The phase drops by the input rate for each output and rises by the output rate for
    // each input. The fraction, in 2.14, rises by kStep for each output and drops by 1 for each input. It starts again
    // at 0 when the phase reaches 0 exactly.
    for (std::size_t i = 0; i + 1 < samples.size(); i += 2)
    {
        const std::array<int16_t, 2> next = {samples[i], samples[i + 1]};
        while (phase_ > 0)
        {
            for (std::size_t c = 0; c < 2; c++)
            {
                const int32_t product = (next[c] - last_[c]) * fraction_;
                const auto value = static_cast<int16_t>(last_[c] + (product >> 14) + ((product >> 13) & 1));

                // AudioOut's gain. Its vector code shifts the product down by 7 bits, rounding down.
                out.push_back(static_cast<int16_t>((value * kGain) >> 7));
            }
            fraction_ += kStep;
            phase_ -= kInputRate;
        }

        fraction_ = fraction_ > 0x4000 ? fraction_ - 0x4000 : 0;
        if (phase_ == 0)
        {
            fraction_ = 0;
        }
        phase_ += kOutputRate;
        last_ = next;
    }
}

} // namespace hsf::jaudio
