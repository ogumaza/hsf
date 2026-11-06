// SPDX-License-Identifier: MIT

// What HSF reads from Bezel Engine's data sheets (Data/SheetBin/*.elf) on ntdlib's reader: the effects of a game's
// sound effect list.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>

#include "ntdlib/bezel/sheet.h"

namespace hsf::game
{

using Sheet = ntdlib::bezel::Sheet;

// An I3DL2 reverb's parameters in the order of nn::audio's I3dl2ReverbParameterSet.
using I3dl2Preset = std::array<float, 12>;

// The I3DL2 reverbs of a game's sound effect list (Data/SheetBin/SoundEffectList.elf), by name, in the units the game
// gives nn::audio: it divides the times by 1000 (ms to s), and the ratio and the dry gain by 100.
std::map<std::string, I3dl2Preset> ReadI3dl2Presets(const Sheet& sheet);

// A delay or a light limiter of a sound effect list, in the form that the game gives nn::atk: an aux effect's type (2
// for a delay, 3 for a light limiter) and its 8 parameters in the archive descriptor's order and units (docs/hsf.md,
// "Aux effects").
struct RendererPreset
{
    uint32_t type = 0;
    std::array<float, 8> parameters{};

    bool operator==(const RendererPreset&) const = default;
};

// The effects of a sound effect list in WarioWare: Move It!'s form (s_RawEffectDataList), by name. The game divides an
// I3DL2 reverb's times by 1000 (ms to s), and keeps its other parameters. It sets its delays and light limiters up on 2
// channels.
struct RawEffectPresets
{
    std::map<std::string, I3dl2Preset> i3dl2;
    std::map<std::string, RendererPreset> renderer;
};

RawEffectPresets ReadRawEffectPresets(const Sheet& sheet);

} // namespace hsf::game
