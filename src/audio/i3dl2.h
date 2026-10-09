// SPDX-License-Identifier: MIT

// Nintendo's I3DL2 reverb as nn::audio runs it in a game's process on an aux effect's samples (the AuxI3dl2Reverb
// functions). The model follows the build in WarioWare: Get It Together!'s sdk. Each function gives the address of the
// routine it follows there. The reverb mixes Q14 fixed point and floats. The reverb's powers of ten, sines and square
// root come from tables and integer arithmetic. The reverb therefore computes the same everywhere. Only two channels
// are modelled.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace hsf::audio
{

// nn::audio::I3dl2ReverbParameterSet: gains in millibels, times in seconds, the diffusion and density in percent, and
// the reference frequency in Hz. The dry gain scales the input that passes through.
struct I3dl2ReverbParameterSet
{
    float room_gain = 0.0f;
    float room_hf_gain = 0.0f;
    float late_reverb_decay_time = 0.0f;
    float late_reverb_hf_decay_ratio = 0.0f;
    float reflections_gain = 0.0f;
    float reflections_delay = 0.0f;
    float reverb_gain = 0.0f;
    float late_reverb_delay = 0.0f;
    float late_reverb_diffusion = 0.0f;
    float late_reverb_density = 0.0f;
    float hf_reference = 0.0f;
    float dry_gain = 0.0f;
};

// An I3DL2 reverb over two channels of a frame's samples (nn::audio::AuxI3dl2ReverbType and its buffer).
class AuxI3dl2Reverb
{
public:
    // InitializeAuxI3dl2Reverb (0x3dbc0) for two channels at `sample_rate`: every parameter 0, and the reverb set up at
    // the first Process.
    explicit AuxI3dl2Reverb(int32_t sample_rate);

    // SetAuxI3dl2ReverbInputOutput (0x3dc50): the block of the samples each channel takes and gives. The reverb is set
    // up again at the next Process.
    void SetInputOutput(const std::array<int8_t, 2>& inputs, const std::array<int8_t, 2>& outputs);

    // SetAuxI3dl2ReverbParameters (0x3ddb0): the reverb works out its coefficients again at the next Process.
    void SetParameters(const I3dl2ReverbParameterSet& parameters);

    // ProcessAuxI3dl2Reverb (0x3dea0): `sample_count` samples of each channel, channel k's samples at input + inputs[k]
    // × sample_count, and likewise for the output. The output may be the input.
    void Process(int32_t* output, const int32_t* input, uint32_t sample_count);

private:
    // A delay line (nn::audio::Axfx2DelayLine): cells 0 to `length` of the buffer from `start`, positions that wrap to
    // 0 past `length`, and an all-pass's gain.
    struct DelayLine
    {
        std::size_t start = 0;
        int32_t length = 0; // +0x10: its last cell
        int32_t write = 0;  // +0x18
        int32_t read = 0;   // +0x20
        int32_t delay = 0;  // +0x28
        float gain = 0.0f;  // +0x2c
    };

    // The reverb's state (nn::audio::I3dl2ReverbState), at the offsets the code uses.
    struct State
    {
        float lowpass = 0.0f;                     // +0x0: the input low-pass's last output
        float lowpass_feedback = 0.0f;            // +0x4
        float lowpass_input = 0.0f;               // +0x8
        DelayLine early;                          // +0x10: the input, for the early reflections and the late reverb
        std::array<int32_t, 20> early_taps{};     // +0x40
        float early_gain = 0.0f;                  // +0x90
        float late_gain = 0.0f;                   // +0x94
        int32_t late_tap = 0;                     // +0x98: where the late reverb takes the input
        std::array<DelayLine, 4> fdn;             // +0xa0: the feedback lines
        std::array<DelayLine, 4> decay1;          // +0x160: each one's first all-pass
        std::array<DelayLine, 4> decay2;          // +0x220: and second
        DelayLine center;                         // +0x2e8: for six channels
        std::array<float, 4> shelf_gain{};        // +0x318: each feedback line's shelf filter
        std::array<float, 4> shelf_feedforward{}; // +0x328
        std::array<float, 4> shelf_feedback{};    // +0x338
        std::array<float, 4> shelf_state{};       // +0x348
        float dry_gain = 0.0f;                    // +0x358
    };

    void Initialize();       // InitializeI3dl2ReverbEffect, 0x77d80
    void Update(bool reset); // UpdateI3dl2ReverbEffectParameter, 0x77fe0
    void Clear(const DelayLine& line);
    DelayLine MakeLine(std::size_t& cursor, int32_t length);

    // Ticks the two all-passes of a feedback line with `x`, and the line with their output (Axfx2AllPassTick, 0x6d490).
    float AllPassTick(DelayLine& first, DelayLine& second, DelayLine& fdn, float x);

    // The two channels' samples (detail::ApplyI3dl2ReverbEffect2ch, 0x76310).
    void Apply2ch(const int32_t* in0, const int32_t* in1, int32_t* out0, int32_t* out1, uint32_t count);

    std::array<int8_t, 2> inputs_{};
    std::array<int8_t, 2> outputs_{};
    int32_t sample_rate_;
    I3dl2ReverbParameterSet parameters_;
    uint8_t parameter_state_ = 0; // 0 set up, 1 compute the coefficients again, 2 done
    State state_;
    std::vector<float> lines_; // the delay lines' cells, laid out as the game's buffer holds them
};

} // namespace hsf::audio
