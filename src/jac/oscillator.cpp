// SPDX-License-Identifier: MIT

#include "jac/oscillator.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "jac/calc.h"

namespace hsf::jac
{

namespace
{

// FORCE_RELEASE_TABLE (0x5bf3fa in main's ro): a linear move to 0 over 5 time units, then the stop. The GameCube's
// build holds the same points.
constexpr std::array<OscPoint, 2> kForceRelease = {{{0, 5, 0}, {15, 0, 0}}};

// A point's modes that don't move the value: a loop to the point in the value, a hold, and a stop.
constexpr uint16_t kLoop = 13;
constexpr uint16_t kHold = 14;
constexpr uint16_t kEnd = 15;

// A point's value: a 16-bit fraction of 1.
constexpr float kPointScale = 1.0f / 32768.0f;

// The frames of a segment of `time` units.
float SegmentFrames(uint16_t time)
{
    return OscTimeScale() * static_cast<float>(time) / static_cast<float>(kFrameStep);
}

} // namespace

float OscTimeScale()
{
    return kDacRate / 80.0f / 600.0f;
}

float OscToOfs(const OscData* data, OscState& state)
{
    if (data == nullptr)
    {
        state.value = 1.0f;
        return 1.0f;
    }

    // The table and the step of this frame by the state.
    OscTable table = data->attack;
    float step = data->rate;
    switch (state.state)
    {
    case OscState::kRelease:
        if (!(data->attack == data->release))
        {
            state.point = 0;
            state.countdown = 0.0f;
            state.target = state.value;
        }
        if (data->release.Null() && state.direct_release == 0)
        {
            state.direct_release = 16;
        }
        if (state.direct_release != 0)
        {
            // A direct release: a move to 0 over the release's time, at least a frame.
            state.state = OscState::kDirectRelease;
            state.curve = static_cast<uint8_t>(state.direct_release >> 14);
            state.target = 0.0f;
            const float frames = std::fmax(SegmentFrames(static_cast<uint16_t>(state.direct_release & 0x3fff)), 1.0f);
            state.countdown = frames;
            state.rate = (0.0f - state.value) / frames;
        }
        else
        {
            state.state = OscState::kReleaseTable;
            table = data->release;
        }
        break;

    case OscState::kReleaseTable:
        if (data->release.Null())
        {
            state.value = 1.0f;
            return 1.0f;
        }
        table = data->release;
        break;

    case OscState::kForceStop:
        state.state = OscState::kForceRelease;
        state.countdown = 0.0f;
        state.target = state.value;
        state.point = 0;
        table = OscTable{kForceRelease.data(), kForceRelease.size()};
        step = 1.0f;
        break;

    case OscState::kForceRelease:
        table = OscTable{kForceRelease.data(), kForceRelease.size()};
        step = 1.0f;
        break;

    default:
        if (data->attack.Null() && state.state != OscState::kDirectRelease)
        {
            state.value = 1.0f;
            return 1.0f;
        }
        if (state.state == OscState::kStop)
        {
            return 1.0f;
        }
        if (state.state == OscState::kHold)
        {
            return state.value * data->width + data->vertex;
        }
        if (state.state == OscState::kStart)
        {
            state.state = OscState::kAttack;
            state.point = 0;
            state.countdown = 0.0f;
            state.target = 0.0f;
            state.direct_release = 0;
        }
        break;
    }

    // The step. A segment that ends gives the value its target. The points that follow then set the next segment: a
    // point with a time starts a move from the target to the point's value over that time, and one without a time
    // sets the target at once. The game reads the next point while the frames left reach no further than 0.
    float countdown = state.countdown - step;
    state.countdown = countdown;
    float target = state.target;
    if (countdown <= 0.0f)
    {
        if (state.state == OscState::kDirectRelease)
        {
            state.value = target;
            state.state = OscState::kStop;
        }
        else
        {
            uint16_t point = state.point;
            for (;;)
            {
                state.value = target;
                const OscPoint p = table.At(point);
                const uint16_t mode = static_cast<uint16_t>(p.mode);
                if (mode == kLoop)
                {
                    point = static_cast<uint16_t>(p.value);
                    state.point = point;
                    if (!(countdown <= 0.0f))
                    {
                        break;
                    }
                    continue;
                }
                if (mode == kHold)
                {
                    state.state = OscState::kHold;
                    return data->width * target + data->vertex;
                }
                if (mode == kEnd)
                {
                    state.state = OscState::kStop;
                    break;
                }

                state.curve = static_cast<uint8_t>(mode);
                const float next = static_cast<float>(p.value) * kPointScale;
                const uint16_t time = static_cast<uint16_t>(p.time);
                if (time != 0)
                {
                    countdown = SegmentFrames(time);
                    state.countdown = countdown;
                    state.rate = (next - target) / countdown;
                }
                state.target = next;
                target = next;
                point = static_cast<uint16_t>(point + 1);
                state.point = point;
                if (!(countdown <= 0.0f))
                {
                    break;
                }
            }
        }
    }

    // The value on the segment, through its curve.
    float value = target - state.rate * countdown;
    state.value = value;
    switch (state.curve)
    {
    case 3:
        return PitchToCent(value, data->width);
    case 2:
        value = value > 0.0f ? std::sqrt(value) : -std::sqrt(-value);
        break;
    case 1:
        value = value > 0.0f ? value * value : value * -value;
        break;
    default:
        break;
    }

    return value * data->width + data->vertex;
}

} // namespace hsf::jac
