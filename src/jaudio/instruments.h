// SPDX-License-Identifier: MIT

// JAudio2's banks as the Super Mario Galaxy games' `main` builds them for AArch64: an instrument bank (JASBasicBank)
// with its instruments (JASBasicInst), drum sets (JASDrumSet) and their effects (JASInstRand and JASInstSense), and
// the wave bank (JASBasicWaveBank) that gives a note its wave. ntdlib reads the bank's file. The model keeps the
// oscillators' tables as the game's parser leaves them: each points into the game's copy of the bank's envelope
// chunk. A table that the game reads past its end then reads the next tables of the chunk, as in the game.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "jaudio/channel.h"
#include "jaudio/oscillator.h"

namespace ntdlib::jaudio
{
struct Bank;
} // namespace ntdlib::jaudio

namespace hsf::jaudio
{

// What an instrument or a drum gives a note (JASInstParam). The defaults are JASBank::noteOn's.
struct InstParam
{
    float volume = 1.0f;
    float fxmix = 0.0f;
    float pitch = 1.0f;
    float pan = 0.5f;
    float dolby = 0.0f;
    uint32_t type = 0;
    uint32_t wave_id = 0;
    std::array<const OscData*, 2> oscillators{};
    uint32_t oscillator_count = 0;
    bool fixed_key = false; // a drum's wave plays at its rate whatever the key
    uint16_t direct_release = 0;
};

// A wave as a wave bank gives it: its settings and the address of its samples in the DSP's memory.
struct BankWave
{
    WaveInfo info;
    uint32_t address = 0;
};

// A wave bank (JASBasicWaveBank) by wave id. A wave whose AW file isn't loaded isn't in it. The game's handle of such
// a wave gives no address, and a note on it doesn't start.
class WaveBank
{
public:
    void Add(uint32_t id, const BankWave& wave)
    {
        waves_[id] = wave;
    }

    // getWaveHandle, then the handle's getWaveInfo and getWavePtr. Null for a wave that the bank lacks.
    const BankWave* Find(uint32_t id) const
    {
        const auto wave = waves_.find(id);

        return wave != waves_.end() ? &wave->second : nullptr;
    }

private:
    std::map<uint32_t, BankWave> waves_;
};

// An instrument bank (JASBasicBank) of version 1, as JASBNKParser builds it.
class InstrumentBank
{
public:
    // The bank in `file`, as ntdlib has read it into `bank`. Its oscillators' tables come from `file`'s envelope chunk.
    // Throws xsflib::FormatError for a bank of version 0: the model has no layout for one yet.
    InstrumentBank(const ntdlib::jaudio::Bank& bank, std::span<const uint8_t> file);

    InstrumentBank(const InstrumentBank&) = delete;
    InstrumentBank& operator=(const InstrumentBank&) = delete;
    InstrumentBank(InstrumentBank&&) = default;
    InstrumentBank& operator=(InstrumentBank&&) = default;

    uint32_t Number() const
    {
        return number_;
    }

    // The wave bank that the bank plays (JASBank::assignWaveBank), or null for none.
    void SetWaveBank(const WaveBank* waves)
    {
        waves_ = waves;
    }

    const WaveBank* Waves() const
    {
        return waves_;
    }

    // JASBasicBank::getInstParam: the instrument's or the drum's parameters for a note of `key` and `velocity` (each
    // 0 to 255). `random` is JASInstRand's generator. Its effects step it. Returns false where the bank has no
    // instrument for the program, no region for the key and velocity, or no drum for the key.
    bool GetInstParam(uint32_t program, uint32_t key, uint32_t velocity, InstParam& param, uint32_t& random) const;

private:
    // JASInstRand and JASInstSense.
    struct Effect
    {
        bool random = false;
        uint32_t target = 0;
        float base = 1.0f; // JASInstRand
        float width = 0.0f;
        uint8_t trigger = 0; // JASInstSense
        uint8_t center = 60;
        float low = 1.0f;
        float high = 1.0f;
    };

    struct VelocityRegion
    {
        uint32_t high_velocity = 0;
        uint32_t wave_id = 0;
        float volume = 1.0f;
        float pitch = 1.0f;
    };

    struct KeyRegion
    {
        uint32_t high_key = 0;
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

    static void ApplyEffect(const Effect& effect, uint32_t key, uint32_t velocity, InstParam& param, uint32_t& random);

    // The first region whose high velocity reaches `velocity`, or null.
    static const VelocityRegion* FindVelocity(std::span<const VelocityRegion> regions, uint32_t velocity);

    uint32_t number_ = 0;
    std::vector<std::vector<OscPoint>> tables_; // each table from its start to the envelope chunk's end
    std::vector<OscData> oscillators_;
    std::array<Program, 0xf0> programs_{};
    const WaveBank* waves_ = nullptr;
};

} // namespace hsf::jaudio
