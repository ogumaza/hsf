// SPDX-License-Identifier: MIT

// The parts of the archives of one note that the player test and the model test send to a delay or a reverb: the note,
// the wave that it plays and the delay on aux bus A.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ntdlib/testing/sar_files.h"

#include "hsf/format.h"

namespace hsf::testing
{

// A sequence of one note: C4 for 250 ms, at full level on the main bus and on aux bus A.
inline const std::vector<uint8_t> kSentNote = {0xB0, 48, 0xE1, 120, 0x00, 0xD9, 127, 60, 127, 24, 0xFF};

// A PCM16 wave of 1 s that ends: a square wave of 0x1000 and -0x1000 by turns, 16 samples of each.
inline ntdlib::testing::sar_files::WaveSpec SquareWave()
{
    std::vector<uint8_t> samples(64000);
    for (std::size_t i = 0; i < samples.size(); i += 2)
    {
        const uint16_t value = (i / 32) % 2 == 0 ? 0x1000 : 0xF000;
        samples[i] = static_cast<uint8_t>(value);
        samples[i + 1] = static_cast<uint8_t>(value >> 8);
    }

    ntdlib::testing::sar_files::WaveSpec wave;
    wave.encoding = 1;
    wave.loop_end = 32000;
    wave.channels = {samples};

    return wave;
}

// A delay on aux bus A: `delay` µs of up to 1 s, a feedback of `feedback`, and the bus's sound at full level with no
// dry part, on 2 channels.
inline AuxEffect Delay(float delay, float feedback)
{
    AuxEffect effect;
    effect.type = AuxEffect::kDelay;
    effect.parameters = {1000000.0f, delay, 1.0f, feedback, 0.0f, 0.0f, 0.0f, 1.0f};

    return effect;
}

} // namespace hsf::testing
