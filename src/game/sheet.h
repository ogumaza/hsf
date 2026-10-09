// SPDX-License-Identifier: MIT

// Bezel Engine's data sheets (Data/SheetBin/*.elf): little-endian AArch64 ELF relocatable objects. Their symbols name
// arrays of records, with a relocation for each record's pointer to a string.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace hsf::game
{

class Sheet
{
public:
    // Reads the sections, symbols and relocations. Returns an error message if the file isn't such an object.
    static std::optional<std::string> Parse(std::vector<uint8_t> data, Sheet& out);

    // The bytes of symbol `name`'s data, or none if there's no such symbol.
    std::span<const uint8_t> Symbol(const std::string& name) const;

    // The string a pointer at `offset` into symbol `name`'s data points to, or nothing if there's no pointer there.
    std::optional<std::string> StringAt(const std::string& name, std::size_t offset) const;

private:
    struct Section
    {
        uint64_t offset = 0;
        uint64_t size = 0;
    };

    struct SymbolInfo
    {
        uint16_t section = 0;
        uint64_t value = 0;
        uint64_t size = 0;
    };

    // A pointer's target: a section and an offset in it.
    struct Target
    {
        uint16_t section = 0;
        uint64_t offset = 0;
    };

    std::vector<uint8_t> data_;
    std::vector<Section> sections_;
    std::map<std::string, SymbolInfo> symbols_;
    std::map<std::pair<uint16_t, uint64_t>, Target> pointers_; // by the pointer's section and offset
};

// An I3DL2 reverb's parameters in the order of nn::audio's I3dl2ReverbParameterSet.
using I3dl2Preset = std::array<float, 12>;

// The I3DL2 reverbs of a game's sound effect list (Data/SheetBin/SoundEffectList.elf), by name, in the units the game
// gives nn::audio: it divides the times by 1000 (ms to s), and the ratio and the dry gain by 100.
std::map<std::string, I3dl2Preset> ReadI3dl2Presets(const Sheet& sheet);

} // namespace hsf::game
