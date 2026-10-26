// SPDX-License-Identifier: MIT

#include "game/sheet.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

namespace hsf::game
{
namespace
{

constexpr uint32_t kSymbolTable = 2;  // SHT_SYMTAB
constexpr uint32_t kRelocations = 4;  // SHT_RELA
constexpr uint32_t kNoBits = 8;       // SHT_NOBITS: a section with no bytes in the file
constexpr uint32_t kAbsolute64 = 257; // R_AARCH64_ABS64
constexpr uint16_t kAarch64 = 183;    // EM_AARCH64

// The sound effect list's records (s_SoundEffectList, counted by s_SoundEffectListCount): 0x88 bytes, a pointer to the
// name at +0x0, the type at +0x8 (1 for an I3DL2 reverb), and an I3DL2 reverb's 12 parameters from +0x38.
constexpr std::size_t kRecordSize = 0x88;
constexpr uint32_t kI3dl2Type = 1;
constexpr std::size_t kI3dl2Parameters = 0x38;

// The parameters that the list gives in other units than nn::audio, by their indices in I3dl2Preset: the times in ms,
// and the decay ratio and the dry gain in percent.
constexpr std::size_t kDecayTime = 2;
constexpr std::size_t kHfDecayRatio = 3;
constexpr std::size_t kReflectionsDelay = 5;
constexpr std::size_t kReverbDelay = 7;
constexpr std::size_t kDryGain = 11;

// WarioWare: Move It!'s list (s_RawEffectDataList, counted by s_RawEffectDataListCount): 0xB8 bytes a record, a pointer
// to the name at +0x0, and the type at +0x8: 1 for an I3DL2 reverb, 2 for a delay and 3 for a light limiter. An I3DL2
// reverb's 12 parameters start at +0x38, a delay's 7 at +0x68 and a light limiter's 7 at +0x84. The game reads them at
// 0xce372c in its main, and divides the times by 1000 (ms to s).
constexpr std::size_t kRawRecordSize = 0xB8;
constexpr uint32_t kRawDelay = 2;
constexpr uint32_t kRawLightLimiter = 3;
constexpr std::size_t kRawDelayParameters = 0x68;
constexpr std::size_t kRawLightLimiterParameters = 0x84;
constexpr std::size_t kRawParameters = 7;

// The channel mode that the game gives its delays and light limiters: 2 channels.
constexpr float kTwoChannels = 1.0f;

bool Fits(std::size_t size, uint64_t offset, uint64_t length)
{
    return offset <= size && length <= size - offset;
}

// The float at `offset` in `record`.
float FloatAt(std::span<const uint8_t> record, std::size_t offset)
{
    return std::bit_cast<float>(xsflib::LoadLe32(&record[offset]));
}

// AArch64's fcvtzs to 64 bits: a float to an integer, toward 0. NaN gives 0. A value past the range gives its end.
int64_t ToInteger(float value)
{
    if (std::isnan(value))
    {
        return 0;
    }
    if (value >= 9.2233720368547758e18f)
    {
        return INT64_MAX;
    }
    if (value <= -9.2233720368547758e18f)
    {
        return INT64_MIN;
    }

    return static_cast<int64_t>(value);
}

// The microseconds of a time that the game gives nn::atk as whole milliseconds: fcvtzs(s * 1000) * 1,000,000 ns.
float WholeMilliseconds(float seconds)
{
    return static_cast<float>(ToInteger(seconds * 1000.0f) * 1000);
}

// The delay that a record's 7 parameters set up. The list gives the longest delay and the delay in ms, the in, feedback
// and dry gains, the low-pass amount and the channel spread. The game gives nn::atk the longest delay in whole ms, and
// the delay in whole µs (0xe9313c in its main).
std::array<float, 8> DelayParameters(std::span<const uint8_t> record)
{
    std::array<float, 8> p{};
    for (std::size_t k = 0; k < kRawParameters; ++k)
    {
        p[k] = FloatAt(record, kRawDelayParameters + 4 * k);
    }

    p[0] = WholeMilliseconds(p[0] / 1000.0f);
    p[1] = static_cast<float>(ToInteger(p[1] / 1000.0f * 1e6f));
    p[7] = kTwoChannels;

    return p;
}

// The light limiter that a record's 7 parameters set up. The list gives the longest look-ahead, the attack, the release
// and the look-ahead in ms, the threshold, and the input and output gains. The game gives nn::atk each time in whole
// ms.
std::array<float, 8> LightLimiterParameters(std::span<const uint8_t> record)
{
    std::array<float, 8> p{};
    for (std::size_t k = 0; k < kRawParameters; ++k)
    {
        p[k] = FloatAt(record, kRawLightLimiterParameters + 4 * k);
    }

    for (std::size_t k = 0; k < 4; ++k)
    {
        p[k] = WholeMilliseconds(p[k] / 1000.0f);
    }
    p[7] = kTwoChannels;

    return p;
}

} // namespace

std::optional<std::string> Sheet::Parse(std::vector<uint8_t> data, Sheet& out)
{
    out = Sheet{};
    if (data.size() < 0x40 || data[0] != 0x7F || data[1] != 'E' || data[2] != 'L' || data[3] != 'F' || data[4] != 2 ||
        data[5] != 1 || xsflib::LoadLe16(&data[0x12]) != kAarch64)
    {
        return std::string("not a little-endian 64-bit AArch64 ELF object");
    }

    std::vector<Header> headers;
    if (std::optional<std::string> error = out.ReadSections(data, headers))
    {
        return error;
    }

    out.ReadPointers(data, headers, out.ReadSymbols(data, headers));
    out.data_ = std::move(data);

    return std::nullopt;
}

std::optional<std::string> Sheet::ReadSections(const std::vector<uint8_t>& data, std::vector<Header>& headers)
{
    const std::size_t size = data.size();
    const uint64_t table = xsflib::LoadLe64(&data[0x28]);
    const uint16_t entry = xsflib::LoadLe16(&data[0x3A]);
    const uint16_t count = xsflib::LoadLe16(&data[0x3C]);
    if (entry < 0x40 || !Fits(size, table, uint64_t{entry} * count))
    {
        return std::string("its section headers run past its end");
    }

    for (uint16_t i = 0; i < count; ++i)
    {
        const uint8_t* h = &data[table + uint64_t{entry} * i];
        const Header header{xsflib::LoadLe32(h + 4), xsflib::LoadLe64(h + 0x18), xsflib::LoadLe64(h + 0x20),
                            xsflib::LoadLe32(h + 0x28), xsflib::LoadLe32(h + 0x2C)};
        const bool has_bytes = header.type != kNoBits;
        if (has_bytes && !Fits(size, header.offset, header.size))
        {
            return "section " + std::to_string(i) + " runs past its end";
        }

        sections_.push_back({header.offset, has_bytes ? header.size : 0});
        headers.push_back(header);
    }

    return std::nullopt;
}

std::vector<Sheet::Target> Sheet::ReadSymbols(const std::vector<uint8_t>& data, const std::vector<Header>& headers)
{
    std::vector<Target> places;
    for (const Header& h : headers)
    {
        if (h.type != kSymbolTable || h.link >= headers.size())
        {
            continue;
        }

        const Header& strings = headers[h.link];
        for (uint64_t at = h.offset; at + 24 <= h.offset + h.size; at += 24)
        {
            const uint32_t name = xsflib::LoadLe32(&data[at]);
            const uint16_t section = xsflib::LoadLe16(&data[at + 6]);
            const uint64_t value = xsflib::LoadLe64(&data[at + 8]);
            places.push_back({section, value});
            if (name == 0 || name >= strings.size)
            {
                continue;
            }

            const auto* text = reinterpret_cast<const char*>(&data[strings.offset + name]);
            const std::size_t length = std::string(text, static_cast<std::size_t>(strings.size - name)).find('\0');
            if (length != std::string::npos)
            {
                symbols_[std::string(text, length)] = {section, value, xsflib::LoadLe64(&data[at + 16])};
            }
        }
    }

    return places;
}

void Sheet::ReadPointers(const std::vector<uint8_t>& data, const std::vector<Header>& headers,
                         const std::vector<Target>& symbols)
{
    for (const Header& h : headers)
    {
        if (h.type != kRelocations || h.info >= headers.size())
        {
            continue;
        }

        for (uint64_t at = h.offset; at + 24 <= h.offset + h.size; at += 24)
        {
            const uint64_t info = xsflib::LoadLe64(&data[at + 8]);
            const uint64_t symbol = info >> 32;
            if ((info & 0xFFFFFFFF) != kAbsolute64 || symbol >= symbols.size())
            {
                continue;
            }

            const Target& place = symbols[symbol];
            const auto addend = static_cast<uint64_t>(static_cast<int64_t>(xsflib::LoadLe64(&data[at + 16])));
            pointers_[{static_cast<uint16_t>(h.info), xsflib::LoadLe64(&data[at])}] = {place.section,
                                                                                       place.offset + addend};
        }
    }
}

std::span<const uint8_t> Sheet::Symbol(const std::string& name) const
{
    const auto found = symbols_.find(name);
    if (found == symbols_.end() || found->second.section >= sections_.size())
    {
        return {};
    }

    const Section& section = sections_[found->second.section];
    if (!Fits(static_cast<std::size_t>(section.size), found->second.value, found->second.size))
    {
        return {};
    }

    return std::span<const uint8_t>(data_).subspan(static_cast<std::size_t>(section.offset + found->second.value),
                                                   static_cast<std::size_t>(found->second.size));
}

std::optional<std::string> Sheet::StringAt(const std::string& name, std::size_t offset) const
{
    const auto symbol = symbols_.find(name);
    if (symbol == symbols_.end())
    {
        return std::nullopt;
    }

    const auto pointer = pointers_.find({symbol->second.section, symbol->second.value + offset});
    if (pointer == pointers_.end() || pointer->second.section >= sections_.size())
    {
        return std::nullopt;
    }

    const Section& section = sections_[pointer->second.section];
    if (pointer->second.offset >= section.size)
    {
        return std::nullopt;
    }

    const auto* text =
        reinterpret_cast<const char*>(&data_[static_cast<std::size_t>(section.offset + pointer->second.offset)]);
    const std::string rest(text, static_cast<std::size_t>(section.size - pointer->second.offset));
    return rest.substr(0, rest.find('\0'));
}

std::map<std::string, I3dl2Preset> ReadI3dl2Presets(const Sheet& sheet)
{
    std::map<std::string, I3dl2Preset> presets;
    const std::span<const uint8_t> count_bytes = sheet.Symbol("s_SoundEffectListCount");
    const std::span<const uint8_t> records = sheet.Symbol("s_SoundEffectList");
    if (count_bytes.size() < 4)
    {
        return presets;
    }

    const uint32_t count = xsflib::LoadLe32(count_bytes.data());
    for (uint32_t i = 0; i < count && (i + 1) * kRecordSize <= records.size(); ++i)
    {
        const std::size_t base = i * kRecordSize;
        const std::optional<std::string> name = sheet.StringAt("s_SoundEffectList", base);
        if (!name || xsflib::LoadLe32(&records[base + 8]) != kI3dl2Type)
        {
            continue;
        }

        I3dl2Preset p;
        for (std::size_t k = 0; k < p.size(); ++k)
        {
            p[k] = std::bit_cast<float>(xsflib::LoadLe32(&records[base + kI3dl2Parameters + 4 * k]));
        }
        p[kDecayTime] = p[kDecayTime] / 1000.0f;
        p[kHfDecayRatio] = p[kHfDecayRatio] / 100.0f;
        p[kReflectionsDelay] = p[kReflectionsDelay] / 1000.0f;
        p[kReverbDelay] = p[kReverbDelay] / 1000.0f;
        p[kDryGain] = p[kDryGain] / 100.0f;
        presets[*name] = p;
    }

    return presets;
}

RawEffectPresets ReadRawEffectPresets(const Sheet& sheet)
{
    RawEffectPresets presets;
    const std::span<const uint8_t> count_bytes = sheet.Symbol("s_RawEffectDataListCount");
    const std::span<const uint8_t> records = sheet.Symbol("s_RawEffectDataList");
    if (count_bytes.size() < 4)
    {
        return presets;
    }

    const uint32_t count = xsflib::LoadLe32(count_bytes.data());
    for (uint32_t i = 0; i < count && (i + 1) * kRawRecordSize <= records.size(); ++i)
    {
        const std::span<const uint8_t> record = records.subspan(i * kRawRecordSize, kRawRecordSize);
        const std::optional<std::string> name = sheet.StringAt("s_RawEffectDataList", i * kRawRecordSize);
        const uint32_t type = xsflib::LoadLe32(&record[8]);
        if (!name)
        {
            continue;
        }

        if (type == kI3dl2Type)
        {
            I3dl2Preset p;
            for (std::size_t k = 0; k < p.size(); ++k)
            {
                p[k] = FloatAt(record, kI3dl2Parameters + 4 * k);
            }
            p[kDecayTime] = p[kDecayTime] / 1000.0f;
            p[kReflectionsDelay] = p[kReflectionsDelay] / 1000.0f;
            p[kReverbDelay] = p[kReverbDelay] / 1000.0f;
            presets.i3dl2[*name] = p;
        }
        else if (type == kRawDelay)
        {
            presets.renderer[*name] = {type, DelayParameters(record)};
        }
        else if (type == kRawLightLimiter)
        {
            presets.renderer[*name] = {type, LightLimiterParameters(record)};
        }
    }

    return presets;
}

} // namespace hsf::game
