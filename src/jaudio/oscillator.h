// SPDX-License-Identifier: MIT

// JAudio2's envelopes and LFOs (JASOscillator and JASLfo) as the Super Mario Galaxy games' `main` builds them for
// AArch64. A note's channel has two oscillators and two LFOs. An oscillator moves the channel's volume, pitch, pan or
// effect sends along a table of points. The LFOs are the vibrato and the tremolo.

#pragma once

#include <cstdint>
#include <span>

namespace hsf::jaudio
{

// A point of an oscillator's table (JASOscillator::Point). Modes 0 to 3 move to `value` / 32768 along a curve over
// `time` (linear, square, square root or the curve of the DSP's sample cells). Mode 13 goes on at the point that
// `value` names. Mode 14 holds the value, and mode 15 stops the oscillator.
struct OscPoint
{
    int16_t mode = 0;
    int16_t time = 0;
    int16_t value = 0;
};

// An oscillator's settings (JASOscillator::Data): what it moves, the speed of its table, its tables for the note's
// start and its release, and the scale and offset of its output.
struct OscData
{
    uint32_t target = 0; // 0 volume, 1 pitch, 2 pan, 3 effect send, 4 surround, 5 vibrato's depth, 6 tremolo's depth
    float rate = 1.0f;
    std::span<const OscPoint> table;
    std::span<const OscPoint> release;
    float scale = 1.0f;
    float offset = 0.0f;
};

class Oscillator
{
public:
    // The oscillator's states. A direct release moves the value to 0 over a time that the release gives.
    enum State : int32_t
    {
        kStop = 0,
        kAttack = 1,
        kSustain = 2,
        kRelease = 3,
        kDirectRelease = 4,
    };

    // The fields of JASOscillator, for tests.
    struct Fields
    {
        float phase = 0.0f;
        float value = 0.0f;
        float start = 0.0f; // the value that the current segment starts from
        float target = 0.0f;
        uint16_t point = 0;
        uint16_t direct_release = 0; // its time in the low 14 bits, its curve in the top two
        uint8_t curve = 0;
        int32_t state = kStop;
    };

    void InitStart(const OscData* data);
    void IncCounter(float step);
    void Update();
    void Release();

    // The output: the value times the scale, plus the offset.
    float Value() const;

    bool Valid() const
    {
        return data_ != nullptr;
    }

    bool Stopped() const
    {
        return fields_.state == kStop;
    }

    bool Releasing() const
    {
        return fields_.state == kRelease || fields_.state == kDirectRelease;
    }

    uint32_t Target() const
    {
        return data_->target;
    }

    void SetDirectRelease(uint16_t release)
    {
        fields_.direct_release = release;
    }

    const OscData* Data() const
    {
        return data_;
    }

    const Fields& State() const
    {
        return fields_;
    }

    void SetState(const OscData* data, const Fields& fields)
    {
        data_ = data;
        fields_ = fields;
    }

private:
    // The value at the phase's place in a segment of `time`, along the segment's curve.
    void UpdateCurrentValue(float time);

    // The table's point `index`. A point past the table's end reads as a stop.
    static OscPoint PointAt(std::span<const OscPoint> table, uint32_t index);

    const OscData* data_ = nullptr;
    Fields fields_;
};

// An LFO (JASLfo): a sine of a depth and a speed, after a delay in sub-frames.
class Lfo
{
public:
    // The LFO's start: mode 0 takes the free-running LFO's phase, mode 1 starts at 0, and any other mode keeps the
    // phase. Its value returns after the delay.
    struct Fields
    {
        uint32_t mode = 1;
        float phase = 0.0f;
        float depth = 0.0f;
        float pitch = 1.0f;
        uint16_t delay = 0;
        uint16_t countdown = 0;
    };

    float Value() const;
    void IncCounter(float step);

    // Starts the LFO again. Mode 0 takes `free_run_phase`, the free-running LFO's phase.
    void ResetCounter(float free_run_phase);

    void SetDepth(float depth)
    {
        fields_.depth = depth;
    }

    void SetPitch(float pitch)
    {
        fields_.pitch = pitch;
    }

    void SetDelay(uint16_t delay)
    {
        fields_.delay = delay;
    }

    float Phase() const
    {
        return fields_.phase;
    }

    const Fields& State() const
    {
        return fields_;
    }

    void SetState(const Fields& fields)
    {
        fields_ = fields;
    }

private:
    Fields fields_;
};

} // namespace hsf::jaudio
