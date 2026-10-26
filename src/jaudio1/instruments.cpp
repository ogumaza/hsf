// SPDX-License-Identifier: MIT

#include "jaudio1/instruments.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <variant>
#include <vector>

#include "ntdlib/jaudio/bank.h"
#include "xsflib/binary.h"

namespace hsf::jaudio1
{

namespace
{

// JASInstRand's offset of its random value from 0 to 2: -0.99999988 (0xbf7ffffe).
const float kRandomCenter = std::bit_cast<float>(0xbf7ffffeu);

// JASDrumSet::getParam's oscillator for every drum: the volume at 1 throughout, with no tables.
const OscData kDrumOscillator{};

// The instruments that JASBNKParser::findOscPtr searches.
constexpr uint32_t kInstruments = 128;

} // namespace

InstrumentBank::InstrumentBank(const ntdlib::jaudio::Bank& bank, RandomFast& random)
{
    if (bank.version != 0)
    {
        throw xsflib::FormatError("the model of Pikmin 2 reads banks of version 0 only");
    }

    number_ = bank.number;

    // Each oscillator gets its copies of its two tables, as the game's parser makes them.
    const auto copy = [&](const std::vector<ntdlib::jaudio::OscPoint>& points) -> OscTable
    {
        if (points.empty())
        {
            return {};
        }

        const auto& table = tables_.emplace_back(std::make_unique<std::vector<OscPoint>>());
        for (const ntdlib::jaudio::OscPoint& point : points)
        {
            table->push_back({point.mode, point.time, point.value});
        }

        return {table->data(), table->size()};
    };
    std::vector<const OscData*> oscillators;
    oscillators.reserve(bank.oscillators.size());
    for (const ntdlib::jaudio::Oscillator& from : bank.oscillators)
    {
        auto& osc = oscillators_.emplace_back(std::make_unique<OscData>());
        osc->target = from.target;
        osc->rate = from.rate;
        osc->attack = copy(from.attack);
        osc->release = copy(from.release_is_attack ? from.attack : from.release);
        osc->width = from.scale;
        osc->vertex = from.vertex;
        oscillators.push_back(osc.get());
    }

    const auto effects = [](const std::vector<ntdlib::jaudio::InstEffect>& from)
    {
        std::vector<Effect> to;
        for (const ntdlib::jaudio::InstEffect& e : from)
        {
            Effect& effect = to.emplace_back();
            effect.random = e.kind == ntdlib::jaudio::InstEffect::Kind::kRandom;
            effect.target = e.target;
            effect.base = e.base;
            effect.width = e.width;
            effect.trigger = e.trigger;
            effect.center = e.center;
            effect.low = e.low;
            effect.high = e.high;
        }

        return to;
    };
    const auto velocities = [](const std::vector<ntdlib::jaudio::VelocityRegion>& from)
    {
        std::vector<VelocityRegion> to;
        for (const ntdlib::jaudio::VelocityRegion& v : from)
        {
            to.push_back({v.high_velocity, v.wave_id, v.volume, v.pitch});
        }

        return to;
    };

    for (const auto& [number, program] : bank.programs)
    {
        if (number >= programs_.size())
        {
            continue;
        }

        if (const auto* inst = std::get_if<ntdlib::jaudio::Instrument>(&program))
        {
            Instrument& to = programs_[number].emplace<Instrument>();
            to.volume = inst->volume;
            to.pitch = inst->pitch;
            for (std::size_t k = 0; k < std::min<std::size_t>(inst->oscillators.size(), 2); k++)
            {
                to.oscillators[k] = oscillators[inst->oscillators[k]];
            }
            to.effects = effects(inst->effects);
            for (const ntdlib::jaudio::KeyRegion& key : inst->keys)
            {
                to.keys.push_back({key.high_key, velocities(key.velocities)});
            }
        }
        else
        {
            const auto& drums = std::get<ntdlib::jaudio::DrumSet>(program);
            DrumSet& to = programs_[number].emplace<DrumSet>();
            for (std::size_t key = 0; key < drums.keys.size(); key++)
            {
                if (const auto& drum = drums.keys[key])
                {
                    to.keys[key] = Drum{drum->volume,
                                        drum->pitch,
                                        drum->pan,
                                        drum->release,
                                        effects(drum->effects),
                                        velocities(drum->velocities)};
                }
            }
        }
    }

    // The parser's draws (findOscPtr): for each oscillator of an instrument, in the order of the programs and of the
    // instrument's slots, the first earlier instrument that plays the same oscillator gives a note of key 60 at
    // velocity 127. Each of its random effects takes a number. The drum sets come after the instruments and draw none.
    for (const auto& [number, program] : bank.programs)
    {
        const auto* inst = std::get_if<ntdlib::jaudio::Instrument>(&program);
        if (number >= kInstruments || inst == nullptr)
        {
            continue;
        }

        for (const std::size_t osc : inst->oscillators)
        {
            for (const auto& [earlier, other] : bank.programs)
            {
                if (earlier >= number)
                {
                    break;
                }

                const auto* shares = std::get_if<ntdlib::jaudio::Instrument>(&other);
                if (shares == nullptr ||
                    std::find(shares->oscillators.begin(), shares->oscillators.end(), osc) == shares->oscillators.end())
                {
                    continue;
                }

                for (const ntdlib::jaudio::InstEffect& e : shares->effects)
                {
                    if (e.kind == ntdlib::jaudio::InstEffect::Kind::kRandom)
                    {
                        random.Next();
                    }
                }
                break;
            }
        }
    }
}

bool InstrumentBank::HasProgram(uint32_t program) const
{
    return program < programs_.size() && !std::holds_alternative<std::monostate>(programs_[program]);
}

void InstrumentBank::ApplyEffect(const Effect& effect, uint32_t key, uint32_t velocity, InstParam& param,
                                 RandomFast& random)
{
    float y = 0.0f;
    if (effect.random)
    {
        float unit = random.Next();
        unit = unit + unit;
        unit = unit + kRandomCenter;
        y = effect.base + effect.width * unit;
    }
    else
    {
        const int32_t x = effect.trigger == 2   ? static_cast<int32_t>(key)
                          : effect.trigger == 1 ? static_cast<int32_t>(velocity)
                                                : 0;
        const int32_t center = effect.center;
        if (center == 0x7f || center == 0)
        {
            y = effect.low + ((effect.high - effect.low) * static_cast<float>(x)) / 127.0f;
        }
        else if (x - center >= 0)
        {
            const float place = static_cast<float>(x - center) / static_cast<float>(0x7f - center);
            y = place * (effect.high + -1.0f) + 1.0f;
        }
        else
        {
            const float place = static_cast<float>(x) / static_cast<float>(static_cast<uint32_t>(center));
            y = effect.low + place * (1.0f - effect.low);
        }
    }

    switch (effect.target)
    {
    case 0:
        param.volume = y * param.volume;
        break;
    case 1:
        param.pitch = y * param.pitch;
        break;
    case 2:
        // The game moves the pan in double precision.
        param.pan = static_cast<float>((static_cast<double>(y) + -0.5) + static_cast<double>(param.pan));
        break;
    case 3:
        param.fxmix = y + param.fxmix;
        break;
    case 4:
        param.dolby = y + param.dolby;
        break;
    default:
        break;
    }
}

const InstrumentBank::VelocityRegion* InstrumentBank::FindVelocity(std::span<const VelocityRegion> regions,
                                                                   uint32_t velocity)
{
    for (const VelocityRegion& region : regions)
    {
        if (region.high_velocity >= static_cast<int32_t>(velocity))
        {
            return &region;
        }
    }

    return nullptr;
}

bool InstrumentBank::GetInstParam(uint32_t program, uint32_t key, uint32_t velocity, InstParam& param,
                                  RandomFast& random) const
{
    if (program >= programs_.size())
    {
        return false;
    }

    if (const auto* inst = std::get_if<Instrument>(&programs_[program]))
    {
        // JASBasicInst::getParam: always two oscillators, the second often none.
        param.wave_format = 0;
        param.percussion = false;
        param.oscillators = inst->oscillators;
        param.oscillator_count = 2;
        param.volume = inst->volume;
        param.pitch = inst->pitch;
        for (const Effect& effect : inst->effects)
        {
            ApplyEffect(effect, key, velocity, param, random);
        }

        const KeyRegion* region = nullptr;
        for (const KeyRegion& k : inst->keys)
        {
            if (k.high_key >= static_cast<int32_t>(key))
            {
                region = &k;
                break;
            }
        }
        if (region == nullptr)
        {
            return false;
        }

        const VelocityRegion* v = FindVelocity(region->velocities, velocity);
        if (v == nullptr)
        {
            return false;
        }

        param.volume = v->volume * param.volume;
        param.pitch = v->pitch * param.pitch;
        param.wave_id = v->wave_id;

        return true;
    }

    if (const auto* drums = std::get_if<DrumSet>(&programs_[program]))
    {
        // JASDrumSet::getParam. A key without a drum has no regions.
        if (key > 0x7f || !drums->keys[key])
        {
            return false;
        }

        const Drum& drum = *drums->keys[key];
        param.wave_format = 0;
        param.percussion = true;
        param.volume = drum.volume;
        param.pitch = drum.pitch;
        param.pan = drum.pan;
        param.release = drum.release;
        param.oscillators = {&kDrumOscillator, nullptr};
        param.oscillator_count = 1;
        for (const Effect& effect : drum.effects)
        {
            ApplyEffect(effect, key, velocity, param, random);
        }

        const VelocityRegion* v = FindVelocity(drum.velocities, velocity);
        if (v == nullptr)
        {
            return false;
        }

        param.volume = v->volume * param.volume;
        param.pitch = v->pitch * param.pitch;
        param.wave_id = v->wave_id;

        return true;
    }

    return false;
}

} // namespace hsf::jaudio1
