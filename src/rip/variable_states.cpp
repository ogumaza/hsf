// SPDX-License-Identifier: MIT

#include "rip/variable_states.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <utility>

#include "ntdlib/nx/bex/rhythm_modes.h"

namespace hsf::rip
{

namespace
{

// The waiting pieces of the rhythm mode `mode`. Each piece plays beside the mode's controller (RhythmScript). The game
// gives a piece's tempo to the global variable 27 and its number to the global variable 28 (kRhythmTempo and
// kRhythmNumber).
std::vector<WaitingPiece> BesideController(const ntdlib::nx::bex::RhythmMode& mode)
{
    std::vector<WaitingPiece> waiting;
    for (const ntdlib::nx::bex::RhythmPiece& piece : mode.pieces)
    {
        waiting.push_back({piece.label,
                           {{"", {{kRhythmTempo, piece.bpm}, {kRhythmNumber, piece.number}}}},
                           mode.controller.label,
                           mode.controller.archive});
    }

    return waiting;
}

} // namespace

std::span<const WaitingPiece> WaitingPieces(std::span<const uint8_t> archive)
{
    const ntdlib::nx::bex::RhythmMode mode = ntdlib::nx::bex::RhythmModeOf(archive);
    if (mode.pieces.empty())
    {
        return {};
    }

    // ntdlib keeps each archive's pieces until the program ends. The pieces made from them stay as long.
    static std::mutex mutex;
    static std::map<const ntdlib::nx::bex::RhythmPiece*, std::vector<WaitingPiece>> made;
    const std::lock_guard lock(mutex);
    const auto [found, added] = made.try_emplace(mode.pieces.data());
    if (added)
    {
        found->second = BesideController(mode);
    }

    return found->second;
}

const WaitingPiece* WaitingPieceOf(std::span<const WaitingPiece> pieces, std::string_view label)
{
    for (const WaitingPiece& piece : pieces)
    {
        if (label == piece.label ||
            (label.size() > piece.label.size() && label.starts_with(piece.label) && label[piece.label.size()] == ' '))
        {
            return &piece;
        }
    }

    return nullptr;
}

ScriptedStart RhythmScript(uint32_t piece, uint32_t controller, const xsflib::psf::Variables& variables,
                           uint16_t controller_archive)
{
    constexpr uint16_t kCurrent = 29;
    constexpr uint16_t kHandOver = 26;

    ScriptedStart script = {piece, 0, {}};
    if (const auto tempo = variables.find(kRhythmTempo); tempo != variables.end())
    {
        script.steps.push_back(ScriptStep::Write(0, kRhythmTempo, tempo->second));
    }

    script.steps.push_back(ScriptStep::Start(0, controller, controller_archive));

    const auto number = variables.find(kRhythmNumber);
    const int16_t n = number == variables.end() ? -1 : number->second;
    if (n == 1 || n == 200 || n == 300 || n == 400)
    {
        script.steps.push_back(ScriptStep::Start(0, piece));
        return script;
    }

    script.steps.push_back(ScriptStep::Write(2, kCurrent, 1));
    script.steps.push_back(ScriptStep::Start(4, piece));
    script.steps.push_back(ScriptStep::Write(8, kHandOver, 1));

    return script;
}

std::vector<ntdlib::rip::Job> WithVariableStates(std::span<const WaitingPiece> pieces,
                                                 const std::vector<ntdlib::rip::Job>& selected)
{
    std::vector<ntdlib::rip::Job> jobs;
    for (const ntdlib::rip::Job& job : selected)
    {
        const auto piece =
            std::find_if(pieces.begin(), pieces.end(), [&job](const WaitingPiece& p) { return p.label == job.label; });
        if (piece == pieces.end())
        {
            jobs.push_back(job);
            continue;
        }

        for (const VariableState& state : piece->states)
        {
            ntdlib::rip::Job in_state = job;
            if (!state.name.empty())
            {
                in_state.label += " ";
                in_state.label += state.name;
            }

            for (const StateVariable& v : state.variables)
            {
                in_state.variables[v.variable] = v.value;
            }

            jobs.push_back(std::move(in_state));
        }
    }

    return jobs;
}

} // namespace hsf::rip
