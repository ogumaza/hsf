// SPDX-License-Identifier: MIT

// The pieces of music that wait for values that the game gives their sequences' variables, and the states they play
// in. A rip of such a piece plays it in the state that the game starts it in. Each later state that stays set gets a
// rip too. The rip's descriptor holds the values (docs/hsf.md, "Scripted starts").

#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "ntdlib/rip/archive.h"
#include "xsflib/psf/psf.h"

#include "hsf/format.h"

namespace hsf::rip
{

// A variable that a state sets: its number in the sequence commands, 0 to 31, and its value.
struct StateVariable
{
    int variable = 0;
    int16_t value = -1;
};

// A state of a piece: the words that the state's rip adds to the piece's label, and the variables that the state sets.
// The state that the piece starts in adds no words.
struct VariableState
{
    std::string_view name;
    std::vector<StateVariable> variables;
};

// A piece that waits for its variables: its label in the sound archive, its states and its companion. The first state
// is the state that the game starts the piece in. The companion is the label of a sound that the game plays beside the
// piece, and that plays a part under it, or empty. When the piece's archive doesn't hold it, the game's sound archive
// named `companion_archive` (its file name without the extension) does. The game plays that archive as an add-on.
struct WaitingPiece
{
    std::string_view label;
    std::vector<VariableState> states;
    std::string_view companion;
    std::string_view companion_archive;
};

// The waiting piece among `pieces` that `label` rips in a state of: the piece's label, or the label and a state's name
// after a space (WithVariableStates). Null for any other label.
const WaitingPiece* WaitingPieceOf(std::span<const WaitingPiece> pieces, std::string_view label);

// The global variables of a rhythm piece's state (RhythmScript): its tempo, and its number. The controller hands over
// to the piece when variable 28 takes the number.
constexpr uint16_t kRhythmTempo = 27;
constexpr uint16_t kRhythmNumber = 28;

// The scripted start of a rhythm piece of Super Mario Party Jamboree or of Super Mario Party's Sound Stage beside the
// mode's controller (docs/hsf.md, "Scripted starts"). `piece` and `controller` are the sounds' nn::atk item IDs, and
// `variables` is the piece's state: variable 27 holds its tempo and 28 its number.
//
// The game starts the controller first. The controller sets the global variables 24 to 26, 28 and 29 to -1, sets 26 to
// 1 and waits until variable 29 holds 1, 200, 300 or 400. The mode's first piece writes one of those numbers to 29.
// Each later piece writes its number there, and the piece before it then sets 26 to 1. At the end of each bar, the
// controller copies 29 to 28 while 26 holds 1, and then sets 26 to 0. A piece plays from the tick at which 28 holds its
// number. Its number also tells the controller whether to play a kick on each beat under it. The controller never plays
// its ending.
//
// The script does the same. The tempo goes into variable 27 at frame 0, with the controller's start. A first piece
// starts at frame 0 after the controller. The game starts the two in the same frame, in that order. For a later piece,
// the first piece's 1 goes into 29 at frame 2, the piece starts at frame 4, and 26 takes 1 at frame 8. The script's
// lead-in is 0. The rip sets it from the frame at which variable 28 takes the piece's number
// (LengthAnalyzer::FramesUntil). The controller comes from the set's archive, or from add-on archive
// `controller_archive` (ScriptStep::Start).
//
// The Sound Stage's controller waits for 1 or 200 only. It uses variable 26 as two flags: bit 0 asks it to copy 29 to
// 28 at the end of the bar, and bit 1 asks it for a fill. After the copy it clears bit 0, and each piece clears bit 1
// as it starts. The script's 1 in variable 26 sets bit 0 and leaves bit 1 clear. The same script therefore serves both
// games.
ScriptedStart RhythmScript(uint32_t piece, uint32_t controller, const xsflib::psf::Variables& variables,
                           uint16_t controller_archive = ScriptStep::kNone);

// The waiting pieces of the sound archive `archive`, which a table recognizes by the archive's size and SHA-1. None for
// any other archive.
std::span<const WaitingPiece> WaitingPieces(std::span<const uint8_t> archive);

// The jobs that rip `selected` in the states of `pieces`. A piece's job takes the piece's first state. The jobs of its
// later states follow it, under the piece's label, a space and the state's name. The other jobs stay as they are. A
// job matches a piece by its whole label. A job of a track state, which WithStates names "LABEL state", therefore gets
// no variables. No game's rules give a waiting piece track states.
std::vector<ntdlib::rip::Job> WithVariableStates(std::span<const WaitingPiece> pieces,
                                                 const std::vector<ntdlib::rip::Job>& selected);

} // namespace hsf::rip
