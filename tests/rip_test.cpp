// SPDX-License-Identifier: MIT

// Tests the ripper's length analysis on ntdlib's length cases (ntdlib/testing/length_cases.h): sequences that each take
// one of its paths, in a little-endian archive. A counted loop that runs past the limit follows. Then the test checks
// that the ripper takes a big-endian archive, and only that, for a Wii U archive.

#include "rip/rip.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/testing/length_cases.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/testing/check.h"

#include "hsf/sar.h"

using namespace ntdlib::testing;
using namespace xsflib::testing;

namespace
{

// The length that the ripper gives the case `name`, or -1.
long long LengthOf(const hsf::rip::LengthAnalyzer& analyzer, const std::string& name)
{
    for (std::size_t i = 0; i < length_cases::Cases().size(); ++i)
    {
        if (name == length_cases::Cases()[i].name)
        {
            const std::optional<hsf::rip::Timing> timing = analyzer.Analyze(static_cast<uint32_t>(i));
            return timing ? timing->length_ms : -1;
        }
    }

    return -1;
}

// A little-endian archive with one sequence: a loop of 64 passes, each C4 for 250 ms and a rest, 10 s in all. The
// passes take 640 s, past the limit of 600 s. Program 0 plays a wave of 1 s that ends, as in the length cases.
std::vector<uint8_t> CountedLoopArchive()
{
    length_cases::Code code(false);
    code.LoopStart(64).Note(60, 127, 24).Wait(936).LoopEnd().End();

    std::vector<uint8_t> samples(2 * 32000);
    for (std::size_t i = 0; i < samples.size(); i += 2)
    {
        samples[i + 1] = 0x10;
    }
    sar_files::WaveSpec wave;
    wave.encoding = 1;
    wave.loop_end = 32000;
    wave.channels = {samples};

    sar_files::Region region;
    region.wave = 0;
    region.original_key = 60;
    region.adshr = {127, 127, 127, 0, 127};
    sar_files::VelocityTable velocities;
    velocities.entries = {{0, region}};
    sar_files::KeyTable keys;
    keys.entries = {{0, velocities}};
    sar_files::BankSpec bank;
    bank.waves = {{0x05000000, 0}};
    bank.instruments = {keys};

    sar_files::ArchiveSpec spec;
    spec.banks = {{"BANK", 0}};
    spec.wave_archives = {{"WARC", 1}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{sar_files::Bank(false, bank), "", false},
                  {sar_files::WaveArchive(false, {sar_files::Wave(false, wave)}), "", false},
                  {sar_files::Sequence(false, code.Bytes(), {{"Start", 0}}), "", false}};
    sar_files::SoundSpec sound;
    sound.name = "SEQ_COUNTED_LOOP";
    sound.volume = 127;
    sound.allocate_track_flags = 1;
    sound.banks = {0};
    sound.file = 2;
    spec.sounds = {sound};

    return sar_files::Archive(false, spec);
}

} // namespace

int main()
{
    const hsf::rip::LengthAnalyzer analyzer(
        hsf::fsar::SoundArchive::Load(length_cases::Archive(false), hsf::kSarOptions));
    length_cases::CheckCases([&analyzer](uint32_t index) { return analyzer.Analyze(index); });

    // In HSF, SEQ_END ends at the same frame as SEQ_REST_LOOP and SEQ_LONG_REST. A different rule ends each one. A
    // model whose output runs a frame behind its engine can end SEQ_END sooner.
    const long long end = LengthOf(analyzer, "SEQ_END");
    Check(end > 0 && end == LengthOf(analyzer, "SEQ_REST_LOOP"), "SEQ_END ends at the same frame as SEQ_REST_LOOP");

    // The counted loop reaches the limit. The analysis then takes it as endless: two passes of 10 s, then the fade.
    const hsf::rip::LengthAnalyzer counted(hsf::fsar::SoundArchive::Load(CountedLoopArchive(), hsf::kSarOptions));
    const std::optional<hsf::rip::Timing> timing = counted.Analyze(0);
    Check(timing && timing->length_ms >= 19995 && timing->length_ms <= 20015 && timing->fade_ms == 10000,
          "a counted loop that runs past the limit loops after two passes, " +
              std::to_string(timing ? timing->length_ms : -1) + " ms");

    // The Wii U's archives are big-endian. The ripper refuses them, and takes the Switch's archives.
    Check(hsf::rip::IsWiiUArchive(sar_files::Archive(true, {})), "a big-endian archive is the Wii U's");
    Check(!hsf::rip::IsWiiUArchive(length_cases::Archive(false)), "a little-endian archive isn't the Wii U's");
    Check(!hsf::rip::IsWiiUArchive(std::vector<uint8_t>{'F', 'S', 'A', 'R'}), "four bytes aren't an archive");

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    return 0;
}
