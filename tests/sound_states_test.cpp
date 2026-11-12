// SPDX-License-Identifier: MIT

// Tests the jobs of a sound's states by the archive's rules (rip/sound_states.h), those of a piece that waits for its
// variables (rip/variable_states.h), and a rhythm piece's script.

#include "rip/sound_states.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/rip/archive.h"
#include "ntdlib/testing/length_cases.h"
#include "xsflib/testing/check.h"

#include "hsf/format.h"
#include "rip/variable_states.h"

using namespace ntdlib::testing;
using namespace xsflib::testing;

namespace
{

// A layered piece's job takes its first state's volumes. The jobs of its later states follow it, with the state's name
// after the piece's label. A sound of bus_b_later starts with bus B empty, and plays later with each reverb there. The
// other jobs stay as they are. An archive that no rule names has no rules. WithBusBReverb puts a reverb on bus B in
// place of any effect there, and keeps the effects in the order of their buses. A preset of the renderer's reverb
// becomes an effect of type 4.
void TestSoundStates()
{
    ntdlib::sar::ArchiveRules rules;
    rules.layered = {{"SEQ_B", {{"", 0x0006}, {"later", 0x0001}, {"all", 0}}}};
    rules.bus_b_presets = {
        {"I3DL2_Small",
         {-100.0f, -200.0f, 1.0f, 0.5f, -300.0f, 0.01f, -400.0f, 0.02f, 100.0f, 100.0f, 5000.0f, 0.25f}}};
    rules.bus_b = {{"SEQ_B", "I3DL2_Small"}};
    rules.bus_b_later = {"SEQ_C"};
    const std::vector<ntdlib::rip::Job> selected = {{0, "SEQ_A", std::nullopt, 1.0f, ntdlib::rip::kFullTrackVolumes},
                                                    {1, "SEQ_B", std::nullopt, 1.0f, ntdlib::rip::kFullTrackVolumes},
                                                    {2, "SEQ_B2", std::nullopt, 1.0f, ntdlib::rip::kFullTrackVolumes},
                                                    {3, "SEQ_C", std::nullopt, 1.0f, ntdlib::rip::kFullTrackVolumes}};
    const std::vector<ntdlib::rip::Job> jobs = hsf::rip::WithStates(rules, selected);
    const ntdlib::sar::ReverbPreset* small = &rules.bus_b_presets[0];

    ntdlib::rip::TrackVolumes start = ntdlib::rip::kFullTrackVolumes;
    start[1] = 0.0f;
    start[2] = 0.0f;
    ntdlib::rip::TrackVolumes later = ntdlib::rip::kFullTrackVolumes;
    later[0] = 0.0f;
    XSFLIB_CHECK(ntdlib::rip::TrackVolumesOf(0x0006) == start &&
                 ntdlib::rip::TrackVolumesOf(0) == ntdlib::rip::kFullTrackVolumes);
    XSFLIB_REQUIRE(jobs.size() == 7);
    Check(jobs[0].label == "SEQ_A" && jobs[0].track_volumes == ntdlib::rip::kFullTrackVolumes &&
              !jobs[0].bus_b_reverb && jobs[1].index == 1 && jobs[1].label == "SEQ_B" &&
              jobs[1].track_volumes == start && jobs[1].bus_b_reverb == small && jobs[2].index == 1 &&
              jobs[2].label == "SEQ_B later" && jobs[2].track_volumes == later && jobs[2].bus_b_reverb == small &&
              jobs[3].index == 1 && jobs[3].label == "SEQ_B all" &&
              jobs[3].track_volumes == ntdlib::rip::kFullTrackVolumes && jobs[4].label == "SEQ_B2" &&
              !jobs[4].bus_b_reverb,
          "a layered piece's states follow it with its reverb, and the other jobs stay as they are");
    Check(jobs[5].index == 3 && jobs[5].label == "SEQ_C" && !jobs[5].bus_b_reverb && jobs[6].index == 3 &&
              jobs[6].label == "SEQ_C Small reverb" && jobs[6].bus_b_reverb == small &&
              jobs[6].track_volumes == ntdlib::rip::kFullTrackVolumes,
          "a sound of bus_b_later starts with bus B empty, and plays later with each reverb");
    Check(hsf::rip::WithStates({}, selected).size() == selected.size(), "with no rules, the jobs stay as they are");
    Check(ntdlib::sar::RulesOf(length_cases::Archive(false)).Empty(), "an archive that no rule names has no rules");

    hsf::ArchiveDescriptor settings;
    hsf::AuxEffect a;
    a.bus = 0;
    a.type = hsf::AuxEffect::kI3dl2Reverb;
    a.parameters.assign(12, 0.0f);
    hsf::AuxEffect b = a;
    b.bus = 1;
    b.type = hsf::AuxEffect::kDelay;
    b.parameters.assign(8, 0.0f);
    hsf::AuxEffect c = a;
    c.bus = 2;
    settings.effects = {c, b, a};
    const hsf::ArchiveDescriptor with = hsf::rip::WithBusBReverb(settings, small);
    Check(with.effects.size() == 3 && with.effects[0].bus == 0 && with.effects[1].bus == 1 &&
              with.effects[1].type == hsf::AuxEffect::kI3dl2Reverb && with.effects[1].parameters.size() == 12 &&
              with.effects[1].parameters[11] == 0.25f && with.effects[2].bus == 2,
          "the reverb takes bus B's place, in the order of the buses");
    const ntdlib::sar::ReverbPreset room = {
        "EFFECT_ROOM",
        {2.0f, 0.4f, 40000.0f, 4.0f, 0.5f, 2000000.0f, 0.7f, 0.1f, 0.6f, 1.0f, 0.25f, 2.0f},
        ntdlib::sar::ReverbType::kReverb};
    const hsf::ArchiveDescriptor renderer = hsf::rip::WithBusBReverb(settings, &room);
    Check(renderer.effects.size() == 3 && renderer.effects[1].bus == 1 &&
              renderer.effects[1].type == hsf::AuxEffect::kReverb &&
              std::equal(room.parameters.begin(), room.parameters.end(), renderer.effects[1].parameters.begin(),
                         renderer.effects[1].parameters.end()) &&
              room.DryGain() == 0.25f && !renderer.Check(),
          "a preset of the renderer's reverb becomes a reverb of type 4, with its dry gain in its eleventh value");
    const hsf::ArchiveDescriptor without = hsf::rip::WithBusBReverb(settings, nullptr);
    Check(
        without.effects.size() == 3 && without.effects[0].bus == 2 && without.effects[1].type == hsf::AuxEffect::kDelay,
        "with no reverb, the settings stay as they are");
}

// A waiting piece's job takes its first state's variables. The jobs of its later states follow it, with the state's
// name after the piece's label. The other jobs stay as they are, and keep their track volumes. An archive that no table
// holds has no waiting pieces.
void TestVariableStates()
{
    const std::vector<hsf::rip::WaitingPiece> pieces = {
        {"SEQ_B", {{"", {{28, 5}, {27, 120}}}, {"fast", {{27, 180}}}}, "", ""}};
    ntdlib::rip::TrackVolumes half = ntdlib::rip::kFullTrackVolumes;
    half[0] = 0.5f;
    const std::vector<ntdlib::rip::Job> selected = {
        {0, "SEQ_A", std::nullopt, 1.0f, half, {}},
        {1, "SEQ_B", std::nullopt, 1.0f, ntdlib::rip::kFullTrackVolumes, {}}};
    const std::vector<ntdlib::rip::Job> jobs = hsf::rip::WithVariableStates(pieces, selected);

    const xsflib::psf::Variables start = {{27, 120}, {28, 5}};
    const xsflib::psf::Variables fast = {{27, 180}};
    Check(jobs.size() == 3 && jobs[0].label == "SEQ_A" && jobs[0].track_volumes == half && jobs[0].variables.empty() &&
              jobs[1].index == 1 && jobs[1].label == "SEQ_B" && jobs[1].variables == start && jobs[2].index == 1 &&
              jobs[2].label == "SEQ_B fast" && jobs[2].variables == fast,
          "a waiting piece's states follow it, and the other jobs stay as they are");
    Check(hsf::rip::WaitingPieces(length_cases::Archive(false)).empty(),
          "an archive that no table holds has no waiting pieces");

    // A job's label finds its piece, with or without a state's name after it.
    Check(hsf::rip::WaitingPieceOf(pieces, "SEQ_B") == &pieces[0] &&
              hsf::rip::WaitingPieceOf(pieces, "SEQ_B fast") == &pieces[0] &&
              !hsf::rip::WaitingPieceOf(pieces, "SEQ_BB") && !hsf::rip::WaitingPieceOf(pieces, "SEQ_A"),
          "a job's label finds its waiting piece");
}

// A rhythm piece's script starts the controller with the tempo at frame 0. A first piece (number 1, 200, 300 or 400)
// starts at frame 2. A later piece's script writes the first piece's 1 to variable 29 at frame 2, starts the piece at
// frame 4 and writes 1 to variable 26 at frame 8. The lead-in is left at 0.
void TestRhythmScript()
{
    using hsf::ScriptStep;
    constexpr uint32_t kPiece = 0x01000007;
    constexpr uint32_t kController = 0x01000002;
    const hsf::ScriptedStart later = hsf::rip::RhythmScript(kPiece, kController, {{27, 180}, {28, 18014}});
    const hsf::ScriptedStart first = hsf::rip::RhythmScript(kPiece, kController, {{27, 120}, {28, 200}});
    const std::vector<ScriptStep> later_steps = {ScriptStep::Write(0, 27, 180), ScriptStep::Start(0, kController),
                                                 ScriptStep::Write(2, 29, 1), ScriptStep::Start(4, kPiece),
                                                 ScriptStep::Write(8, 26, 1)};
    const std::vector<ScriptStep> first_steps = {ScriptStep::Write(0, 27, 120), ScriptStep::Start(0, kController),
                                                 ScriptStep::Start(0, kPiece)};
    Check(later.sound == kPiece && later.lead_in == 0 && later.steps == later_steps,
          "a later piece's script hands over as the piece before it does");
    Check(first.sound == kPiece && first.lead_in == 0 && first.steps == first_steps,
          "a first piece's script starts the piece after the controller");
}

} // namespace

int main()
{
    TestSoundStates();
    TestVariableStates();
    TestRhythmScript();

    return Summary("sound_states");
}
