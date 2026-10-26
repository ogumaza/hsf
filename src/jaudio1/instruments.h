// SPDX-License-Identifier: MIT

// JAudio1's banks as the Switch release of Pikmin 2 builds them for AArch64: an instrument bank (JASBasicBank) of
// version 0 with its instruments (JASBasicInst), drum sets (JASDrumSet) and their effects (JASInstRand and
// JASInstSense). ntdlib reads the bank's file. The game's parser copies each oscillator's two tables separately. Their
// pointers then differ whenever both exist, and the model keeps them apart too.
//
// JASInstRand's generator (JMath's TRandom_fast_) serves every bank. The game's parser steps it as it reads a bank:
// for an oscillator that an earlier instrument of the bank also plays, it asks that instrument for a note of key 60 at
// velocity 127, and each random effect of that instrument takes a number.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "jaudio/instruments.h"
#include "jaudio1/calc.h"
#include "jaudio1/oscillator.h"

namespace ntdlib::jaudio
{
struct Bank;
} // namespace ntdlib::jaudio

namespace hsf::jaudio1
{

using jaudio::BankWave;
using jaudio::WaveBank;

// What an instrument or a drum gives a note (JASInstParam). The defaults are JASBankMgr::noteOn's.
struct InstParam
{
    uint8_t wave_format = 0; // 0 a wave, 2 the DSP's oscillator
    uint32_t wave_id = 0;
    std::array<const OscData*, 2> oscillators{};
    uint32_t oscillator_count = 0;
    float volume = 1.0f;
    float pitch = 1.0f;
    float pan = 0.5f;
    float fxmix = 0.0f;
    float dolby = 0.0f;
    bool percussion = false; // a drum's wave plays at its rate whatever the key
    uint16_t release = 0;    // a direct release for the first oscillator, or 0 for none
};

// An instrument bank (JASBasicBank) of version 0, as JASBNKParser::createBasicBank builds it.
class InstrumentBank
{
public:
    // The bank as ntdlib has read it into `bank`. The parser's draws step `random`. Throws xsflib::FormatError for a
    // bank of version 1.
    InstrumentBank(const ntdlib::jaudio::Bank& bank, RandomFast& random);

    InstrumentBank(const InstrumentBank&) = delete;
    InstrumentBank& operator=(const InstrumentBank&) = delete;
    InstrumentBank(InstrumentBank&&) = default;
    InstrumentBank& operator=(InstrumentBank&&) = default;

    // The number that the sequences give the bank by (the virtual number in its header).
    uint32_t Number() const
    {
        return number_;
    }

    void SetWaveBank(const WaveBank* waves)
    {
        waves_ = waves;
    }

    const WaveBank* Waves() const
    {
        return waves_;
    }

    // JASBasicInst::getParam or JASDrumSet::getParam for a note of `key` and `velocity` (each 0 to 255) on program
    // `program`. The effects step `random`, whether a region matches or not. Returns false where the bank has no such
    // program, or the program has no region for the key and velocity.
    bool GetInstParam(uint32_t program, uint32_t key, uint32_t velocity, InstParam& param, RandomFast& random) const;

    // Whether the bank has program `program` (JASBasicBank::getInst).
    bool HasProgram(uint32_t program) const;

private:
    struct Effect
    {
        bool random = false;
        uint32_t target = 0;
        float base = 1.0f; // JASInstRand
        float width = 0.0f;
        uint8_t trigger = 0; // JASInstSense: 1 velocity, 2 key
        uint8_t center = 60;
        float low = 1.0f;
        float high = 1.0f;
    };

    struct VelocityRegion
    {
        int32_t high_velocity = 0;
        uint32_t wave_id = 0;
        float volume = 1.0f;
        float pitch = 1.0f;
    };

    struct KeyRegion
    {
        int32_t high_key = 0;
        std::vector<VelocityRegion> velocities;
    };

    struct Instrument
    {
        float volume = 1.0f;
        float pitch = 1.0f;
        std::array<const OscData*, 2> oscillators{};
        std::vector<Effect> effects;
        std::vector<KeyRegion> keys;
    };

    struct Drum
    {
        float volume = 1.0f;
        float pitch = 1.0f;
        float pan = 0.5f;
        uint16_t release = 1000;
        std::vector<Effect> effects;
        std::vector<VelocityRegion> velocities;
    };

    struct DrumSet
    {
        std::array<std::optional<Drum>, 128> keys;
    };

    using Program = std::variant<std::monostate, Instrument, DrumSet>;

    // The effect's value for the note, and its change to the note's levels.
    static void ApplyEffect(const Effect& effect, uint32_t key, uint32_t velocity, InstParam& param,
                            RandomFast& random);

    static const VelocityRegion* FindVelocity(std::span<const VelocityRegion> regions, uint32_t velocity);

    uint32_t number_ = 0;
    std::vector<std::unique_ptr<std::vector<OscPoint>>> tables_;
    std::vector<std::unique_ptr<OscData>> oscillators_;
    std::array<Program, 0x100> programs_{};
    const WaveBank* waves_ = nullptr;
};

} // namespace hsf::jaudio1
