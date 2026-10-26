// SPDX-License-Identifier: MIT

// The envelopes of JAudio1 as the Switch release of Pikmin 2 builds them for AArch64 (JASOscillator), and a track's
// vibrato (JASVibrate). A note's channel has four oscillators. An oscillator moves the channel's volume, pitch, pan,
// effect send or surround level along a table of points. In this library, a vibrato and a tremolo are oscillators
// with looping tables (JASPlayer's sVibratoDef and sTremoroDef).

#pragma once

#include <cstddef>
#include <cstdint>

#include "jaudio/oscillator.h"

namespace hsf::jaudio1
{

using jaudio::OscPoint;

// A table of points as the game holds it: a pointer into the data, or null. The game compares the pointers of a
// setting's two tables. `size` counts the points that the data holds from the pointer on. A point past it reads as a
// stop.
struct OscTable
{
    const OscPoint* points = nullptr;
    std::size_t size = 0;

    bool Null() const
    {
        return points == nullptr;
    }

    OscPoint At(std::size_t index) const
    {
        return index < size ? points[index] : OscPoint{15, 0, 0};
    }

    bool operator==(const OscTable& other) const
    {
        return points == other.points;
    }
};

// An oscillator's settings (JASOscillator::Data): what it moves, the speed of its tables, the tables for the note's
// start and its release, and the scale and offset of its output.
struct OscData
{
    uint32_t target = 0; // 0 volume, 1 pitch, 2 pan, 3 effect send, 4 surround
    float rate = 1.0f;
    OscTable attack;
    OscTable release;
    float width = 1.0f;
    float vertex = 0.0f;
};

class Oscillator
{
public:
    // The states. A direct release moves the value to 0 over a time that the release word gives.
    enum State : uint8_t
    {
        kStop = 0,
        kStart = 1,
        kHold = 2,
        kRelease = 3,
        kForceStop = 4,
        kDirectRelease = 5,
    };

    // The fields of JASOscillator, for tests. `countdown` counts down the current segment's time.
    // `rate` is the step of the value per tick for a linear segment, and the whole step for a curve.
    struct Fields
    {
        float countdown = 0.0f;
        float phase = 0.0f;
        float target = 0.0f;
        float rate = 0.0f;
        float length = 0.0f; // the segment's time
        uint16_t point = 0;
        uint16_t direct_release = 0; // its time in the low 14 bits, its curve in the top two
        uint8_t state = kStop;
        uint8_t curve = 0;
    };

    // The step of an oscillator's time per sub-frame, from the DSP's rate: a segment's time counts in 600ths of a
    // second.
    static float TimeScale(float dac_rate);

    void Init();
    void InitStart(const OscData* data, float dac_rate);
    void IncCounter(float dac_rate);
    bool Release(float dac_rate);

    // The output: 1 for a stopped oscillator, else the phase times the width, plus the vertex.
    float Value() const;

    bool Valid() const
    {
        return data_ != nullptr;
    }

    bool Stopped() const
    {
        return fields_.state == kStop;
    }

    // Whether the oscillator repeats a part of its attack table for ever. A vibrato and a tremolo do that. The table's
    // points play in order up to the first jump, hold or stop. When that point is a jump to an earlier point, the
    // points from there on repeat. The oscillator repeats them once it has read the point that the jump goes to.
    bool Cycling() const;

    uint32_t Target() const
    {
        return data_->target;
    }

    const OscData* Data() const
    {
        return data_;
    }

    void SetDirectRelease(uint16_t release)
    {
        fields_.direct_release = release;
    }

    const Fields& State() const
    {
        return fields_;
    }

private:
    // JASOscillator::calc: the next segments while the countdown has run out, then the value along the segment.
    float Calc(OscTable table, float dac_rate);

    const OscData* data_ = nullptr;
    Fields fields_;
};

// A track's vibrato (JASVibrate): a sine of a depth in semitones, with a phase that turns at `pitch` quarter turns
// a tick.
struct Vibrate
{
    float phase = 0.0f;
    float depth = 0.0f;
    float pitch = 1.0f / 18.0f;

    void Init();
    void IncCounter();
    float Value() const;
};

} // namespace hsf::jaudio1
