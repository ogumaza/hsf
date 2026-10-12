// SPDX-License-Identifier: MIT

// nn::atk's arithmetic helpers (nn::atk::detail::Util): ntdlib's model of them, with the Switch's policy. nn::atk's
// helpers are nw::snd's on AArch64. Each function's comment in ntdlib gives the address of the routine it follows in
// WarioWare: Get It Together!'s main.

#pragma once

#include <cstdint>

#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/util.h"

namespace hsf::atk
{

using ntdlib::nwsnd::OutputMode;
using ntdlib::nwsnd::PanInfo;
using ntdlib::nwsnd::PanInfoOf;
using ntdlib::nwsnd::Random;

namespace util
{

using ntdlib::nwsnd::CalcLowPassBiquad;
using ntdlib::nwsnd::CalcPitchRatio;

inline float Fmax(float a, float b)
{
    return ntdlib::nwsnd::NxPolicy::Fmax(a, b);
}

inline float Fminnm(float a, float b)
{
    return ntdlib::nwsnd::NxPolicy::Fminnm(a, b);
}

inline float Fmin(float a, float b)
{
    return ntdlib::nwsnd::NxPolicy::Fmin(a, b);
}

inline float CalcVolumeRatio(float decibel)
{
    return ntdlib::nwsnd::CalcVolumeRatio<ntdlib::nwsnd::NxPolicy>(decibel);
}

inline float CalcPanRatio(float pan, const PanInfo& info, OutputMode mode)
{
    return ntdlib::nwsnd::CalcPanRatio<ntdlib::nwsnd::NxPolicy>(pan, info, mode);
}

inline float CalcSurroundPanRatio(float span, const PanInfo& info)
{
    return ntdlib::nwsnd::CalcSurroundPanRatio<ntdlib::nwsnd::NxPolicy>(span, info);
}

inline uint16_t CalcLpfFreq(float lpf)
{
    return ntdlib::nwsnd::CalcLpfFreq<ntdlib::nwsnd::NxPolicy>(lpf);
}

} // namespace util
} // namespace hsf::atk
