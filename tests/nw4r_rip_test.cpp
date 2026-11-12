// SPDX-License-Identifier: MIT

// Tests a Wii sound archive's parts (rip/nw4r_rip.h): its sequences, a loop's length on Skyward Sword HD's layer,
// and the copy of the archive that a library holds (ntdlib's rsar/cut.h).

#include "rip/nw4r_rip.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "ntdlib/rsar/cut.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/testing/rsar_files.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/testing/check.h"

#include "rip/rip.h"

using namespace ntdlib::testing;
using namespace xsflib::testing;

namespace
{

using ntdlib::testing::rsar_files::MakeAudibleArchive;

// A Wii sound archive's parts. The sequence plays a note of 24 ticks over and over, at 96 ticks a second. Its track
// sets the notes' envelope first: a full sustain at once and the fastest release.
void TestNw4r()
{
    const std::vector<uint8_t> bytes =
        MakeAudibleArchive({0xD1, 0x7F, 0xD2, 0x7F, 0xD3, 0x7F, 60, 127, 24, 0x89, 0x00, 0x00, 6, 0xFF}, 1);
    Check(hsf::rip::IsRsar(bytes) && !hsf::rip::IsRsar(sar_files::Archive(false, {})),
          "a Wii sound archive is an RSAR");
    Check(hsf::rip::FindRsarFiles({"Sound/A.brsar", "Sound/b.bfsar", "Sound/demo/C.BRSAR"}) ==
              std::vector<std::string>{"Sound/A.brsar", "Sound/demo/C.BRSAR"},
          "the game's .brsar files");

    const auto archive = ntdlib::rsar::SoundArchive::Load(bytes, ntdlib::rvl::snd::kRsarOptions);
    const std::vector<hsf::rip::Nw4rJob> jobs = hsf::rip::SelectNw4rSounds(archive, std::regex(".*"), false);
    Check(jobs.size() == 1 && jobs[0].index == 0 && jobs[0].label == "SEQ_TEST" &&
              ntdlib::rsar::MissingData(archive, 0).empty(),
          "the archive's sequence, with its data");
    Check(hsf::rip::SelectNw4rSounds(archive, std::regex(".*"), true).empty(), "a sequence without BGM isn't music");

    // Two loops of 250 ms, then the fade.
    const std::optional<hsf::rip::Timing> timing = hsf::rip::AnalyzeNw4rLength(archive, {}, 0);
    Check(timing && timing->length_ms >= 490 && timing->length_ms <= 510 && timing->fade_ms == 10000,
          "two loops of 24 ticks on the layer, " + std::to_string(timing ? timing->length_ms : -1) + " ms");

    // The copy holds the sequence's files as the archive does, and plays them the same.
    const std::vector<uint8_t> kept = ntdlib::rsar::KeepSequenceData(archive, {0}, ntdlib::rvl::snd::kRsarOptions);
    const auto cut = ntdlib::rsar::SoundArchive::Load(kept, ntdlib::rvl::snd::kRsarOptions);
    const auto same = [&archive, &cut](uint32_t file_id)
    {
        const auto a = archive.File(file_id);
        const auto b = cut.File(file_id);
        return a && b && std::ranges::equal(a->file, b->file) && std::ranges::equal(a->wave_data, b->wave_data);
    };
    Check(same(archive.Sounds()[0].file_id) && same(archive.Banks()[archive.Sounds()[0].bank].file_id),
          "the library's copy of the archive holds the sequence's files");
    const std::optional<hsf::rip::Timing> again = hsf::rip::AnalyzeNw4rLength(cut, {}, 0);
    Check(again && timing && again->length_ms == timing->length_ms, "the copy plays as the archive does");
}

} // namespace

int main()
{
    TestNw4r();

    return Summary("nw4r_rip");
}
