// SPDX-License-Identifier: MIT

// The arithmetic of the C JAudio as the Switch release of Pikmin builds it for AArch64: the generator of random numbers
// that the sequences and the instruments' effects share (GetRandom_s32), the two tables of sines that the mixers read
// (Jac_InitSinTable, sinf3 and the surround levels' curve), the pitch of a bend (Jam_PitchToCent) and the pitches of
// the keys around C5 (C5BASE_PITCHTABLE). The build fuses no product into a sum: every step rounds on its own.

#pragma once

#include <array>
#include <cstdint>

namespace hsf::jac
{

// JAC_DAC_RATE (0x6afb54 in main's data): the DSP's output rate.
inline constexpr float kDacRate = 32028.5f;

// The frames that each update of a DSP channel stands for (the word at 0x6afb64 in main's data, 1). The update
// divides an oscillator's times and counts down a note's time by it. A pitch sweep moves by it. No code of the Jac
// writes it.
inline constexpr uint32_t kFrameStep = 1;

// The generator of random numbers (GetRandom_s32 and the functions on it). The game's sequences, the instruments'
// random effects and the game's code draw from one generator. Its state as a sound starts depends on what the game
// played before. The model starts it as the game starts.
class Random
{
public:
    // GetRandom_s32.
    int32_t S32();

    // GetRandom_u32: S32 without its sign bit.
    uint32_t U32()
    {
        return static_cast<uint32_t>(S32()) & 0x7fffffffu;
    }

    // GetRandom_ulimit: U32 modulo `limit`. AArch64's division by 0 gives 0, and the remainder is then U32 itself.
    uint32_t ULimit(uint32_t limit)
    {
        const uint32_t value = U32();
        return limit != 0 ? value % limit : value;
    }

    // GetRandom_sf32: the low 24 bits of S32 over 16,777,215, negative when S32 is.
    float SF32();

    int32_t V0() const
    {
        return v0_;
    }

    int32_t V1() const
    {
        return v1_;
    }

private:
    int32_t v0_ = 0x1000;
    int32_t v1_ = 0x5555;
};

// Jac_InitSinTable's first table: 257 sines of a quarter turn, from the system's sinf. sinf3: the table's entry at
// `x` × 256, truncated, for an x from 0 to 1. For an x above 1, the game reads on into the second table. That table
// follows the first in memory. The model reads the same entries up to the second table's end. It reads the last entry
// for an index past that, and entry 0 for a negative index.
float Sinf3(float x);

// Jac_InitSinTable's second table: the sines of 257 steps from 0.32612 to 0.67388 of a quarter turn. A mixer's
// surround level reads it at `x` × 256 (0x273000 in main). A negative index reads back into the first table.
float SurroundCurve(float x);

// Jam_PitchToCent: the pitch factor of a track's pitch level `value` with the bend's range `range` (register 7), from
// C5BASE_PITCHTABLE and the table of the fractions of a semitone (KEY_TABLE). The game reads the key's table past its
// ends for a bend beyond 60 semitones. The model keeps the key at the table's end.
float PitchToCent(float value, float range);

// C5BASE_PITCHTABLE: the pitch of each key from 0 to 127 against key 60. A key above 127 reads as 127.
float KeyPitch(uint32_t key);

} // namespace hsf::jac
