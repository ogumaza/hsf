// SPDX-License-Identifier: MIT

// What nn::atk's voices read of its HardwareManager, and the mixes that voices output to. The defaults are the values a
// game's sound system starts with (HardwareManager::Initialize, 0xc05340, and SoundSystemParam's constructor, 0xc11270,
// in WarioWare: Get It Together!'s main), with the output mode WarioWare sets.

#pragma once

#include <array>
#include <cstdint>

#include "atk/util.h"

namespace hsf::atk
{

// A mix that voices output to (OutputReceiver): the renderer's final mix or a sub-mix, with the channels and buses it
// has (GetChannelCount, GetBusCount and IsSoundSendClampEnabled of FinalMix and SubMix).
struct OutputReceiver
{
    int mix = 0; // the renderer's id of the mix
    int channel_count = 6;
    int bus_count = 4;
    std::array<bool, 4> clamp_send{}; // whether each bus holds its sends to 0 to 1
};

struct HardwareSettings
{
    // The output mode nn::atk mixes for (+0xe0), which WarioWare sets to stereo (0xced314), and the mode of the end
    // user's output (+0xe4).
    OutputMode output_mode = OutputMode::kStereo;
    OutputMode end_user_output_mode = OutputMode::kSurround;

    // The devices each bit of an output line reaches (+0x510): bit 0 reaches device 0, the console's output.
    std::array<uint8_t, 32> output_line_devices{1};

    // The product of the two volume ramps of GetOutputVolume (0xc06230), 1 while neither moves.
    float output_volume = 1.0f;

    // A voice passes every bus of its receiver to the renderer, not only bus 0 (+0x950, the sound system's +0x76).
    bool all_buses = true;

    // The low-pass from the voice's rate rather than from the tables (+0x956, the sound system's +0x7b).
    bool exact_low_pass = false;

    // Voices resample at the high quality (+0x958, the sound system's +0x97).
    bool high_src_quality = false;

    // The surround pan curves for surround output (+0xa65, the sound system's +0x7e).
    bool surround_pan_curves = false;

    // The renderer's sample rate (+0x8c0).
    int32_t renderer_sample_rate = 48000;
};

} // namespace hsf::atk
