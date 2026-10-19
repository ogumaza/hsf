// SPDX-License-Identifier: MIT

// The sound archives (BFSAR) and the files in them, read with ntdlib::sar: the Wii U's formats, little-endian on the
// Switch. HSF reads them by the players' rules. Those rules stop at the first problem and give HSF's messages.

#pragma once

#include "ntdlib/sar/formats.h"

namespace hsf
{

namespace fsar = ntdlib::sar;

// How HSF reads sound archives and the files in them.
inline constexpr fsar::Options kSarOptions{fsar::Flavor::kNx, {}, false, fsar::Rules::kStopAtFirstProblem};

} // namespace hsf
