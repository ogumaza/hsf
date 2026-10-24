// SPDX-License-Identifier: MIT

#include "jaudio/instruments.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <variant>
#include <vector>

#include "ntdlib/jaudio/bank.h"
#include "xsflib/binary.h"

namespace hsf::jaudio
{

namespace
{

// JASInstRand's offset of its random value from 0 to 2: -0.99999988 (0xbf7ffffe).
const float kRandomCenter = std::bit_cast<float>(0xbf7ffffeu);

// JASDrumSet::getParam's oscillator for every drum: the volume at 1 throughout, with no tables.
const OscData kDrumOscillator{};

// A chunk of a version 1 bank, as JASBNKParser finds it: the first with the tag from 0x20 on, while the chunks start
// inside the bank's size.
struct Chunk
{
    std::size_t at = 0;
    uint32_t size = 0;
};

Chunk FindChunk(const xsflib::Reader& r, uint32_t bank_size, uint32_t tag)
{
    for (std::size_t at = 0x20; at < bank_size; at = (at + r.U32(at + 4) + 0xb) & ~std::size_t{3})
    {
        if (r.U32(at) == tag)
        {
            return {at, r.U32(at + 4)};
        }
    }

    throw xsflib::FormatError("bank lacks a chunk that the game requires");
}

constexpr uint32_t Tag(const char (&name)[5])
{
    return static_cast<uint32_t>(static_cast<uint8_t>(name[0])) << 24 |
           static_cast<uint32_t>(static_cast<uint8_t>(name[1])) << 16 |
           static_cast<uint32_t>(static_cast<uint8_t>(name[2])) << 8 | static_cast<uint8_t>(name[3]);
}

} // namespace

InstrumentBank::InstrumentBank(const ntdlib::jaudio::Bank& bank, std::span<const uint8_t> file)
{
    if (bank.version != 1)
    {
        throw xsflib::FormatError("the model reads banks of version 1 only");
    }

    number_ = bank.number;
    const bool big_endian = file.size() >= 4 && file[0] == 'I';
    const xsflib::Reader r(file, big_endian);
    const uint32_t bank_size = r.U32(4);

    // The game copies the envelope chunk's data to the heap. Each oscillator's tables point into the copy, at the
    // offsets that the oscillator gives.
    const Chunk envelopes = FindChunk(r, bank_size, Tag("ENVT"));
    const Chunk osct = FindChunk(r, bank_size, Tag("OSCT"));
    const std::size_t data = envelopes.at + 8;
    const std::size_t data_size = std::min<std::size_t>(envelopes.size, file.size() - std::min(file.size(), data));
    std::map<uint32_t, std::size_t> table_at; // by offset
    const auto table = [&](uint32_t offset) -> std::span<const OscPoint>
    {
        auto [found, added] = table_at.try_emplace(offset, tables_.size());
        if (added)
        {
            std::vector<OscPoint>& points = tables_.emplace_back();
            for (std::size_t at = offset; at + 6 <= data_size; at += 6)
            {
                points.push_back({r.S16(data + at), r.S16(data + at + 2), r.S16(data + at + 4)});
            }
        }

        return tables_[found->second];
    };

    const uint32_t count = r.U32(osct.at + 8);
    tables_.reserve(2 * std::size_t{count});
    oscillators_.reserve(count);
    for (uint32_t i = 0; i < count; i++)
    {
        const std::size_t at = osct.at + 0x10 + 0x1c * std::size_t{i};
        OscData& osc = oscillators_.emplace_back();
        osc.target = r.U8(at);
        osc.rate = r.F32(at + 4);
        osc.table = table(r.U32(at + 8));
        osc.release = table(r.U32(at + 12));
        osc.scale = r.F32(at + 16);
        osc.offset = r.F32(at + 20);
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
                to.oscillators[k] = &oscillators_[inst->oscillators[k]];
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
}

void InstrumentBank::ApplyEffect(const Effect& effect, uint32_t key, uint32_t velocity, InstParam& param,
                                 uint32_t& random)
{
    float y = 0.0f;
    if (effect.random)
    {
        // JMath's TRandom_fast_: a float from 1 to 2 by the generator's top 23 bits.
        random = random * 0x19660du + 0x3c6ef35fu;
        if (effect.target > 4)
        {
            return;
        }

        float unit = std::bit_cast<float>(0x3f800000u | random >> 9) + -1.0f;
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
            y = std::fma(effect.high + -1.0f, place, 1.0f);
        }
        else
        {
            const float place = static_cast<float>(x) / static_cast<float>(center);
            y = std::fma(1.0f - effect.low, place, effect.low);
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
        param.pan = (y + -0.5f) + param.pan;
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
        if (static_cast<int32_t>(region.high_velocity) >= static_cast<int32_t>(velocity))
        {
            return &region;
        }
    }

    return nullptr;
}

bool InstrumentBank::GetInstParam(uint32_t program, uint32_t key, uint32_t velocity, InstParam& param,
                                  uint32_t& random) const
{
    if (program >= programs_.size())
    {
        return false;
    }

    if (const auto* inst = std::get_if<Instrument>(&programs_[program]))
    {
        // JASBasicInst::getParam: always two oscillators, the second often none.
        param.type = 0;
        param.oscillators = inst->oscillators;
        param.fixed_key = false;
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
            if (static_cast<int32_t>(k.high_key) >= static_cast<int32_t>(key))
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
        // JASDrumSet::getParam.
        if (key > 0x7f || !drums->keys[key])
        {
            return false;
        }

        const Drum& drum = *drums->keys[key];
        param.type = 0;
        param.fixed_key = true;
        param.volume = drum.volume;
        param.pitch = drum.pitch;
        param.pan = drum.pan;
        param.oscillator_count = 1;
        param.direct_release = drum.release;
        param.oscillators = {&kDrumOscillator, nullptr};
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

} // namespace hsf::jaudio
