// SPDX-License-Identifier: MIT

// The rips of a sound archive's sounds in the states that a game's code gives them (ntdlib/sar/game_rules.h). A game's
// code may turn tracks of a piece down as the piece plays, through the sound's handle (nn::atk's SetTrackVolume). It
// may put a reverb on aux bus B while a sound plays. A rip of such a sound plays it in the state that the game starts
// it in. Each later state gets a rip too. The rip's descriptor holds the state's track volumes (docs/hsf.md, "Track
// volumes") and its effects.

#pragma once

#include <cstdint>
#include <vector>

#include "ntdlib/rip/archive.h"
#include "ntdlib/sar/game_rules.h"

#include "hsf/format.h"

namespace hsf::rip
{

// The volumes of the tracks in a state whose muted tracks are `muted`: 0 for each track whose bit is set, and 1 for
// the rest.
ntdlib::rip::TrackVolumes TrackVolumesOf(uint16_t muted);

// The jobs that rip `selected` in the states that `rules` give their sounds (ntdlib::sar::StatesOf). A sound's job
// takes its first state. The jobs of its later states follow it, under the sound's label, a space and the state's name.
// The other jobs stay as they are.
std::vector<ntdlib::rip::Job> WithStates(const ntdlib::sar::ArchiveRules& rules,
                                         const std::vector<ntdlib::rip::Job>& selected);

// `settings` for a sound that plays with `reverb` on aux bus B: the reverb in place of any effect that `settings` put
// on bus B. The preset's type makes the reverb an I3DL2 reverb or the renderer's reverb. `settings` as they are for no
// reverb.
ArchiveDescriptor WithBusBReverb(const ArchiveDescriptor& settings, const ntdlib::sar::ReverbPreset* reverb);

} // namespace hsf::rip
