// SPDX-License-Identifier: MIT

// The oscillators of the C JAudio as the Switch release of Pikmin builds them for AArch64. An oscillator moves a
// channel's volume, pitch, pan, effect send or surround level along a table of points: an envelope of a bank's
// instrument, a sequence's envelope, vibrato or tremolo, or the release of a drum or of the DSP's oscillator. A
// track's oscillators can move the track's levels instead. Each frame, Bank_OscToOfs steps an oscillator's state and
// gives its level.

#pragma once

#include <cstddef>
#include <cstdint>

#include "jaudio/oscillator.h"

namespace hsf::jac
{

using jaudio::OscPoint;

// A table of points as the game holds it: a pointer into the data, or null. Bank_OscToOfs compares the pointers of a
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

// An oscillator's settings: what it moves, the speed of its tables, the tables for the note's start and its release,
// and the scale and offset of its output. A track copies settings by value. The game does the same.
struct OscData
{
    // The targets.
    static constexpr uint8_t kVolume = 0;
    static constexpr uint8_t kPitch = 1;
    static constexpr uint8_t kPan = 2;
    static constexpr uint8_t kFxmix = 3;
    static constexpr uint8_t kDolby = 4;

    uint8_t target = kVolume;
    float rate = 1.0f;
    OscTable attack;
    OscTable release;
    float width = 1.0f;
    float vertex = 0.0f;
};

// An oscillator's state, by the game's fields.
struct OscState
{
    // The states. kStart starts the attack table at the next step. kRelease starts the release: the release table, or
    // with a direct release, a move to 0 over the release's time (kDirectRelease). kForceStop starts the forced
    // release's table (kForceRelease).
    enum State : uint8_t
    {
        kStop = 0,
        kStart = 1,
        kAttack = 2,
        kHold = 3,
        kRelease = 4,
        kReleaseTable = 5,
        kForceStop = 6,
        kForceRelease = 7,
        kDirectRelease = 8,
    };

    uint8_t state = kStop;
    uint8_t curve = 0;           // the current segment's curve: 0 linear, 1 squared, 2 the square root, 3 a bend's
                                 // pitch (the Switch's build only)
    uint16_t point = 0;          // the next point of the table
    float countdown = 0.0f;      // the frames left in the current segment
    float value = 0.0f;          // the value before the curve
    float target = 0.0f;         // the current segment's target
    float rate = 0.0f;           // the value's step per frame
    uint16_t direct_release = 0; // a direct release: its time in 600ths of a second in the low 14 bits, its curve in
                                 // the top two
};

// The frames of a segment's time unit (600ths of a second) at the DSP's rate: 32,028.5 / 80 / 600.
float OscTimeScale();

// Bank_OscToOfs: steps `state` by a frame along `data`'s tables and returns the oscillator's level: the value through
// the curve, times the width, plus the vertex. Curve 3 gives Jam_PitchToCent of the value with the width as the range,
// and no vertex. Without `data`, the state's value becomes 1 and the level is 1. The game divides each time by the
// frames of an update (kFrameStep).
float OscToOfs(const OscData* data, OscState& state);

} // namespace hsf::jac
