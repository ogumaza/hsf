// SPDX-License-Identifier: MIT

// The HSF file format code (src/hsf/format.*): the PSF container, tags, times, program chunks, the archive descriptor,
// and _lib loading.

#include "hsf/format.h"

#include <zlib.h>

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "check.h"
#include "common/bytes.h"

namespace
{

using hsf::ArchiveDescriptor;
using hsf::FileReader;
using hsf::LoadedSet;
using hsf::ProgramBuilder;
using hsf::ProgramChunks;
using hsf::PsfFile;
using hsf::Tags;
using hsf::WritePsf;

constexpr uint8_t kVersion = hsf::kPsfVersionHsf;

// A reader of the files in `fs`, by their paths made normal.
FileReader MapReader(const std::map<std::string, std::vector<uint8_t>>& fs)
{
    return [&fs](const std::string& path, std::vector<uint8_t>& out)
    {
        const auto it = fs.find(std::filesystem::path(path).lexically_normal().generic_string());
        if (it == fs.end())
        {
            return false;
        }

        out = it->second;

        return true;
    };
}

// The error LoadSet gives for `path`, or an empty string if the set loads.
std::string LoadError(const std::string& path, const FileReader& reader)
{
    LoadedSet set;
    const auto err = hsf::LoadSet(path, reader, set);
    return err ? *err : std::string();
}

void TestTimes()
{
    HSF_CHECK(hsf::ParseTime("") == -1);
    HSF_CHECK(hsf::ParseTime("abc") == -1);
    HSF_CHECK(hsf::ParseTime("5") == 5000);
    HSF_CHECK(hsf::ParseTime("1.5") == 1500);
    HSF_CHECK(hsf::ParseTime("1,25") == 1250);
    HSF_CHECK(hsf::ParseTime("2:03") == 123000);
    HSF_CHECK(hsf::ParseTime("2:03.5") == 123500);
    HSF_CHECK(hsf::ParseTime("1:00:00") == 3600000);
    HSF_CHECK(hsf::ParseTime(" 0:10.000 ") == 10000);
    HSF_CHECK(hsf::ParseTime("1:2:3:4") == -1);
    HSF_CHECK(hsf::ParseTime("-1") == -1);
    HSF_CHECK(hsf::ParseTime("nan") == -1);
    HSF_CHECK(hsf::ParseTime("inf") == -1);
    HSF_CHECK(hsf::ParseTime("1e300") == -1);
    HSF_CHECK(hsf::ParseTime("99999999999999999999:00") == -1);

    HSF_CHECK(hsf::FormatTime(0) == "0:00.000");
    HSF_CHECK(hsf::FormatTime(65998) == "1:05.998");
    HSF_CHECK(hsf::FormatTime(600000) == "10:00.000");

    for (const long long ms : {0LL, 1LL, 999LL, 59999LL, 61001LL, 3599999LL})
    {
        HSF_CHECK(hsf::ParseTime(hsf::FormatTime(ms)) == ms);
    }
}

// Numbers in tags: an optional sign, digits with '.' or ',' before decimals, and an optional exponent, with the values
// that std::strtod gives in the C locale. Text after the number is left unread.
void TestDecimals()
{
    for (const char* text : {"0", "0.5", "1,25", "-1.5", "+2", ".5", "5.", "123.456", "0.000123", "2.718281828459045",
                             "1e3", "1.5E-3", "1e22", "1e-22", "9007199254740992", "0.1", "0.3", "62.226"})
    {
        std::string c_text = text;
        std::replace(c_text.begin(), c_text.end(), ',', '.');

        const auto number = hsf::ReadDecimal(text);

        Check(number && number->length == std::strlen(text) && number->value == std::strtod(c_text.c_str(), nullptr),
              std::string(text) + " reads as std::strtod reads it");
    }

    const auto with_unit = hsf::ReadDecimal("0.5dB");
    const auto bare_exponent = hsf::ReadDecimal("1e+x");
    const auto hex = hsf::ReadDecimal("0x10");
    const auto huge = hsf::ReadDecimal("1e999");
    const auto tiny = hsf::ReadDecimal("1e-999");

    HSF_CHECK(with_unit && with_unit->value == 0.5 && with_unit->length == 3);
    HSF_CHECK(bare_exponent && bare_exponent->value == 1 && bare_exponent->length == 1);
    HSF_CHECK(hex && hex->value == 0 && hex->length == 1);
    HSF_CHECK(huge && std::isinf(huge->value));
    HSF_CHECK(tiny && tiny->value == 0);
    for (const char* text : {"", ".", "-", "+.", "e5", " 1", "nan", "inf", "x1"})
    {
        Check(!hsf::ReadDecimal(text), std::string("\"") + text + "\" isn't a number");
    }
}

// Under a locale that writes decimals with a comma, std::strtod stops at '.'. Tags still read the same. The check runs
// only where such a locale is installed.
void TestDecimalsUnderCommaLocale()
{
    const char* comma_locale = nullptr;
    for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8", "de-DE", "German_Germany.1252"})
    {
        if (std::setlocale(LC_NUMERIC, name) && std::strtod("0.5", nullptr) == 0)
        {
            comma_locale = name;
            break;
        }
    }

    if (!comma_locale)
    {
        std::setlocale(LC_NUMERIC, "C");
        std::printf("no locale with decimal commas is installed: tags weren't read under one\n");
        return;
    }

    const auto volume = hsf::ReadDecimal("0.5");
    const long long length = hsf::ParseTime("1:02.226");
    std::setlocale(LC_NUMERIC, "C");

    Check(volume && volume->value == 0.5, std::string("a volume of 0.5 reads as 0.5 under ") + comma_locale);
    Check(length == 62226, std::string("a length of 1:02.226 reads as 62,226 ms under ") + comma_locale);
}

void TestPsfRoundTrip()
{
    const std::vector<uint8_t> kReserved = {1, 2, 3, 4, 5};
    const Tags kTags = {{"title", "Test"}, {"comment", "line one\nline two"}, {"_lib", "x.hsflib"}};
    std::vector<uint8_t> program(100000);
    for (std::size_t i = 0; i < program.size(); i++)
    {
        program[i] = static_cast<uint8_t>(i * 7 + (i >> 8));
    }

    const auto file = WritePsf(kVersion, kReserved, program, kTags);
    PsfFile psf;
    const auto err = hsf::ParsePsf(file, psf);

    HSF_CHECK(file.size() > 16 && std::memcmp(file.data(), "PSF", 3) == 0 && file[3] == 0x29);
    HSF_CHECK(!err);
    HSF_CHECK(psf.version == kVersion);
    HSF_CHECK(psf.reserved == kReserved);
    HSF_CHECK(psf.program == program);
    HSF_CHECK(psf.tags == kTags);
    HSF_CHECK(hsf::TagAreaOffset(file) == 16 + kReserved.size() + hsf::LoadLe32(&file[8]));

    // Corruption of the compressed program is detected by the CRC.
    auto bad = file;
    bad[16 + kReserved.size() + 10] ^= 0x55;

    HSF_CHECK(hsf::ParsePsf(bad, psf) == "program CRC mismatch");

    // Truncation.
    auto cut = file;
    cut.resize(20);

    HSF_CHECK(hsf::ParsePsf(cut, psf) == "truncated PSF file");
    HSF_CHECK(hsf::ParsePsf(std::vector<uint8_t>{'P', 'S'}, psf) == "not a PSF file");

    // A program whose zlib stream stops short, with a size and CRC that match what's left of it.
    const uint32_t half = hsf::LoadLe32(&file[8]) / 2;
    std::vector<uint8_t> short_stream(file.begin(),
                                      file.begin() + 16 + static_cast<std::ptrdiff_t>(kReserved.size()) + half);
    hsf::StoreLe32(&short_stream[8], half);
    hsf::StoreLe32(&short_stream[12], static_cast<uint32_t>(crc32(0, &short_stream[16 + kReserved.size()], half)));

    HSF_CHECK(hsf::ParsePsf(short_stream, psf) == "truncated program data");
}

void TestTagParsing()
{
    const std::string kHeader = std::string("PSF") + static_cast<char>(kVersion) + std::string(12, '\0');
    const std::string tagged = kHeader +
                               "[TAG]Title = Hello \nTITLE2=x\ncomment=a\ncomment=b\nartist=me\ncomment=c\n"
                               "no equals sign\n=empty name\nlength=1:00\r\n";
    std::vector<uint8_t> data(tagged.begin(), tagged.end());
    PsfFile psf;

    HSF_CHECK(!hsf::ParsePsf(data, psf));
    HSF_CHECK(psf.tags["title"] == "Hello");
    HSF_CHECK(psf.tags["title2"] == "x");
    HSF_CHECK(psf.tags["comment"] == "c"); // a and b join, and the later c replaces them
    HSF_CHECK(psf.tags["artist"] == "me");
    HSF_CHECK(psf.tags["length"] == "1:00");
    HSF_CHECK(psf.tags.count("") == 0);

    // Consecutive lines with the same name join.
    const std::string repeated = kHeader + "[TAG]comment=a\ncomment=b\n";
    std::vector<uint8_t> data2(repeated.begin(), repeated.end());

    HSF_CHECK(!hsf::ParsePsf(data2, psf));
    HSF_CHECK(psf.tags["comment"] == "a\nb");
}

// An I3DL2 reverb on aux bus `bus`, with parameters 1 to 12.
hsf::AuxEffect Reverb(uint32_t bus)
{
    hsf::AuxEffect effect;
    effect.bus = bus;
    for (int i = 1; i <= 12; ++i)
    {
        effect.parameters.push_back(static_cast<float>(i) * 0.5f);
    }

    return effect;
}

void TestArchiveDescriptor()
{
    ArchiveDescriptor d;
    d.version = 2;
    d.flags = 1;
    d.output_mode = 2;
    d.voice_count = 72;
    d.sdk_version = 100401;
    d.effects = {Reverb(0), Reverb(2)};
    d.effects[1].parameters[11] = -0.0f;
    d.archive_path = "Archive/Sound.bea/Audio/b.bfsar";
    d.group_paths = {"Audio/groups/GROUP_A.bfgrp", "Audio/groups/GROUP_BB.bfgrp"};

    const auto raw = d.Serialize();
    const auto back = ArchiveDescriptor::Parse(raw);

    HSF_CHECK(raw.size() % 4 == 0);
    HSF_CHECK(std::memcmp(raw.data(), "HSFA", 4) == 0);
    HSF_CHECK(back && back->version == d.version && back->flags == d.flags && back->output_mode == d.output_mode &&
              back->voice_count == d.voice_count && back->sdk_version == d.sdk_version &&
              back->archive_path == d.archive_path && back->group_paths == d.group_paths);
    HSF_CHECK(back && back->effects.size() == 2 && back->effects[0].bus == 0 && back->effects[1].bus == 2 &&
              back->effects[0].type == hsf::AuxEffect::kI3dl2Reverb &&
              back->effects[0].parameters == d.effects[0].parameters && std::signbit(back->effects[1].parameters[11]));
    HSF_CHECK(!ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.begin() + 40)).has_value());
    HSF_CHECK(!ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.begin() + 100)).has_value());

    // A player only accepts version 1, with no flags, in stereo, with 1 to 96 voices, and I3DL2 reverbs of 12 finite
    // parameters on the aux buses 0 to 2.
    HSF_CHECK(d.Check().has_value());

    ArchiveDescriptor v1 = d;
    v1.version = 1;
    v1.flags = 0;
    v1.output_mode = hsf::kOutputStereo;

    HSF_CHECK(!v1.Check());
    HSF_CHECK(!ArchiveDescriptor{}.Check());

    for (const uint32_t voices : {0u, 97u})
    {
        ArchiveDescriptor odd = v1;
        odd.voice_count = voices;
        HSF_CHECK(odd.Check().has_value());
    }

    ArchiveDescriptor bus = v1;
    bus.effects[0].bus = 3;
    ArchiveDescriptor type = v1;
    type.effects[0].type = 2;
    ArchiveDescriptor count = v1;
    count.effects[0].parameters.pop_back();
    ArchiveDescriptor infinite = v1;
    infinite.effects[1].parameters[3] = std::numeric_limits<float>::infinity();

    HSF_CHECK(bus.Check().has_value());
    HSF_CHECK(type.Check().has_value());
    HSF_CHECK(count.Check().has_value());
    HSF_CHECK(infinite.Check().has_value());
}

void TestChunks()
{
    ProgramBuilder b;
    b.AddFile("content/sound/a.bfsar", std::vector<uint8_t>{9, 8, 7, 6, 5});
    b.AddSound(0x01000005);
    b.AddFile("Audio/other.bin", std::vector<uint8_t>{1});

    ProgramChunks chunks;
    const auto err = hsf::ParseProgram(b.Data(), chunks);

    HSF_CHECK(b.Data().size() % 4 == 0);
    HSF_CHECK(!err);
    HSF_CHECK(chunks.files.size() == 2 && chunks.sound == 0x01000005u);
    HSF_CHECK(chunks.files[0].path == "content/sound/a.bfsar" &&
              chunks.files[0].data == (std::vector<uint8_t>{9, 8, 7, 6, 5}));
    HSF_CHECK(chunks.files[1].path == "Audio/other.bin" && chunks.files[1].data == std::vector<uint8_t>{1});

    // Unknown chunks are skipped.
    std::vector<uint8_t> prog = {'X', 'X', 'X', 'X', 4, 0, 0, 0, 1, 2, 3, 4};
    const std::vector<uint8_t> data = b.Data();
    prog.insert(prog.end(), data.begin(), data.end());
    ProgramChunks skipped;

    HSF_CHECK(!hsf::ParseProgram(prog, skipped));
    HSF_CHECK(skipped.files.size() == 2 && skipped.sound.has_value());

    // A truncated chunk is an error, even when its size is so large that adding it to the position wraps around on a
    // 32-bit host. A FILE chunk whose path runs past its end is an error too, and so is an SND chunk without its word.
    const std::vector<uint8_t> trunc = {'F', 'I', 'L', 'E', 100, 0, 0, 0, 1, 2};
    const std::vector<uint8_t> huge = {'F', 'I', 'L', 'E', 0xf8, 0xff, 0xff, 0xff, 1, 2, 3, 4};
    const std::vector<uint8_t> long_path = {'F', 'I', 'L', 'E', 4, 0, 0, 0, 9, 0, 'a', 'b'};
    const std::vector<uint8_t> short_sound = {'S', 'N', 'D', ' ', 2, 0, 0, 0, 1, 2, 0, 0};
    ProgramChunks bad;

    HSF_CHECK(hsf::ParseProgram(trunc, bad).has_value());
    HSF_CHECK(hsf::ParseProgram(huge, bad).has_value());
    HSF_CHECK(hsf::ParseProgram(long_path, bad).has_value());
    HSF_CHECK(hsf::ParseProgram(short_sound, bad).has_value());
}

void TestLoadSet()
{
    // dir/lib.hsflib holds the descriptor and the base files. dir/sub/x.minihsf loads it with _lib=../lib.hsflib, and
    // loads extra.minihsf from the same directory with _lib2.
    std::map<std::string, std::vector<uint8_t>> fs;
    const ArchiveDescriptor archive;
    {
        ProgramBuilder b;
        b.AddFile("f", std::vector<uint8_t>{1});
        b.AddSound(0x01000001);
        fs["dir/lib.hsflib"] = WritePsf(kVersion, archive.Serialize(), b.Data(), {{"game", "G"}});
    }
    {
        ProgramBuilder b;
        b.AddFile("f", std::vector<uint8_t>{2});
        b.AddSound(0x01000002);
        fs["dir/sub/x.minihsf"] =
            WritePsf(kVersion, {}, b.Data(),
                     {{"_lib", "../lib.hsflib"}, {"_lib2", "extra.minihsf"}, {"title", "X"}, {"length", "1:00"}});
    }
    {
        ProgramBuilder b;
        b.AddSound(0x01000003);
        fs["dir/sub/extra.minihsf"] = WritePsf(kVersion, {}, b.Data(), {{"title", "extra"}});
    }
    const FileReader reader = MapReader(fs);

    LoadedSet set;
    const auto err = hsf::LoadSet("dir/sub/x.minihsf", reader, set);

    if (!Check(!err && set.sources.size() == 3, "x.minihsf loads: " + (err ? *err : std::string("wrong result"))))
    {
        return;
    }
    HSF_CHECK(set.sources[0] == "dir/sub/../lib.hsflib");
    HSF_CHECK(set.sources[1] == "dir/sub/x.minihsf");
    HSF_CHECK(set.sources[2] == "dir/sub/extra.minihsf");
    HSF_CHECK(set.files.size() == 2 && set.File("f") && *set.File("f") == std::vector<uint8_t>{2});
    HSF_CHECK(!set.File("g"));
    HSF_CHECK(set.archive.has_value());
    HSF_CHECK(set.sound == 0x01000003u);
    HSF_CHECK(set.tags["title"] == "X" && set.tags["length"] == "1:00" && set.tags.count("game") == 0);

    // An empty _lib2 names no library, like an empty _lib, and _lib3 still loads.
    fs["dir/sub/v.minihsf"] =
        WritePsf(kVersion, {}, {}, {{"_lib", "../lib.hsflib"}, {"_lib2", ""}, {"_lib3", "extra.minihsf"}});
    LoadedSet gap;

    HSF_CHECK(!hsf::LoadSet("dir/sub/v.minihsf", reader, gap));
    HSF_CHECK(gap.sources.size() == 3 && gap.sources[2] == "dir/sub/extra.minihsf");

    // A missing library, a wrong PSF version, a set without a descriptor and one without a sound are errors.
    fs["dir/y.minihsf"] = WritePsf(kVersion, {}, {}, {{"_lib", "missing.hsflib"}});
    fs["dir/z.miniwsf"] = WritePsf(0x27, {}, {}, {{"_lib", "lib.hsflib"}});
    fs["dir/w.minihsf"] = WritePsf(kVersion, {}, {}, {{"title", "w"}});
    fs["dir/nosound.hsflib"] = WritePsf(kVersion, archive.Serialize(), {}, {});

    HSF_CHECK(LoadError("dir/y.minihsf", reader) == "can't read dir/missing.hsflib");
    HSF_CHECK(LoadError("dir/z.miniwsf", reader).find("not an HSF file") != std::string::npos);
    HSF_CHECK(LoadError("dir/w.minihsf", reader).starts_with("no archive descriptor"));
    HSF_CHECK(LoadError("dir/nosound.hsflib", reader).starts_with("no sound selected"));

    // So is a descriptor a version 1 player can't play.
    ArchiveDescriptor v2;
    v2.version = 2;
    fs["dir/v2.hsflib"] = WritePsf(kVersion, v2.Serialize(), {}, {});

    HSF_CHECK(LoadError("dir/v2.hsflib", reader).find("version 2") != std::string::npos);

    // A _lib cycle ends with an error instead of recursing forever.
    fs["dir/c1.minihsf"] = WritePsf(kVersion, {}, {}, {{"_lib", "c2.minihsf"}});
    fs["dir/c2.minihsf"] = WritePsf(kVersion, {}, {}, {{"_lib", "c1.minihsf"}});

    HSF_CHECK(LoadError("dir/c1.minihsf", reader) == "_lib nesting too deep");

    // So does a set that names more files than a set can have. Each of these names the next one ten times. Loading them
    // all would take 10^10 loads.
    for (int level = 0; level < 10; level++)
    {
        Tags tags;
        for (int n = 2; n <= 11; n++)
        {
            tags["_lib" + std::to_string(n)] = "f" + std::to_string(level + 1) + ".minihsf";
        }
        fs["dir/f" + std::to_string(level) + ".minihsf"] = WritePsf(kVersion, {}, {}, tags);
    }
    fs["dir/f10.minihsf"] = WritePsf(kVersion, {}, {}, {});

    HSF_CHECK(LoadError("dir/f0.minihsf", reader) == "more than 64 files in the set");
}

void TestLoadSetInArchive()
{
    // foobar2000's path for a file in an archive: unpack://zip|<length>|<the archive's path>|<its path in the archive>.
    const std::string kArchive = "unpack://zip|23|file://C:\\music\\set.zip|";
    std::map<std::string, std::vector<uint8_t>> fs;
    ProgramBuilder b;
    b.AddSound(0x01000000);
    fs[kArchive + "set.hsflib"] = WritePsf(kVersion, ArchiveDescriptor{}.Serialize(), b.Data(), {});
    fs[kArchive + "x.minihsf"] = WritePsf(kVersion, {}, {}, {{"_lib", "set.hsflib"}});
    const FileReader reader = [&fs](const std::string& path, std::vector<uint8_t>& out)
    {
        const auto it = fs.find(path);
        if (it == fs.end())
        {
            return false;
        }

        out = it->second;

        return true;
    };

    LoadedSet set;
    const auto err = hsf::LoadSet(kArchive + "x.minihsf", reader, set, "/\\|");
    const std::string without = LoadError(kArchive + "x.minihsf", reader);

    // With | as a separator, the library is found in the archive, and without it, beside the archive.
    HSF_CHECK(!err && set.sources.size() == 2 && set.sources[0] == kArchive + "set.hsflib");
    HSF_CHECK(without == "can't read unpack://zip|23|file://C:\\music\\set.hsflib");
}

// A PSF file as one call of zlib's compress2 makes it. WritePsf compresses a program in pieces. The test checks that
// WritePsf gives the same bytes.
std::vector<uint8_t> OneCallPsf(const std::vector<uint8_t>& reserved, const std::vector<uint8_t>& program,
                                const Tags& tags)
{
    std::vector<uint8_t> compressed;
    uLongf size = 0;
    if (!program.empty())
    {
        size = compressBound(static_cast<uLong>(program.size()));
        compressed.resize(size);
        compress2(compressed.data(), &size, program.data(), static_cast<uLong>(program.size()), Z_BEST_COMPRESSION);
        compressed.resize(size);
    }

    std::vector<uint8_t> out(16);
    std::memcpy(out.data(), "PSF", 3);
    out[3] = kVersion;
    hsf::StoreLe32(&out[4], static_cast<uint32_t>(reserved.size()));
    hsf::StoreLe32(&out[8], static_cast<uint32_t>(compressed.size()));
    hsf::StoreLe32(
        &out[12], compressed.empty() ? 0 : static_cast<uint32_t>(crc32(0, compressed.data(), static_cast<uInt>(size))));
    out.insert(out.end(), reserved.begin(), reserved.end());
    out.insert(out.end(), compressed.begin(), compressed.end());
    const std::string text = hsf::FormatTagArea(tags);
    out.insert(out.end(), text.begin(), text.end());

    return out;
}

void TestProgramPieces()
{
    // Files of every length modulo four, copied and referred to, and large ones that compress well and badly.
    std::vector<std::vector<uint8_t>> files;
    std::mt19937 rng(5);
    for (std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4},
                             std::size_t{5}, std::size_t{3} << 20})
    {
        std::vector<uint8_t> file(size);
        for (uint8_t& byte : file)
        {
            byte = static_cast<uint8_t>(rng() % 4);
        }
        files.push_back(std::move(file));
    }
    std::vector<uint8_t> noise(1 << 20);
    for (uint8_t& byte : noise)
    {
        byte = static_cast<uint8_t>(rng());
    }
    files.push_back(std::move(noise));

    ProgramBuilder copied;
    ProgramBuilder referred;
    for (std::size_t i = 0; i < files.size(); ++i)
    {
        const std::string path = "content/" + std::string(i, 'f');
        copied.AddFile(path, files[i]);
        referred.AddFileRef(path, files[i]);
        if (i == 3)
        {
            copied.AddSound(0x01000003);
            referred.AddSound(0x01000003);
        }
    }

    const std::vector<uint8_t> program = copied.Data();
    HSF_CHECK(referred.Data() == program);

    // A referred file is a piece of the program, not a copy.
    bool referred_in_place = false;
    for (const std::span<const uint8_t> piece : referred.Pieces())
    {
        referred_in_place = referred_in_place || piece.data() == files.back().data();
    }
    HSF_CHECK(referred_in_place);

    const std::vector<uint8_t> kReserved = {7, 7};
    const Tags kTags = {{"title", "pieces"}};
    const std::vector<uint8_t> file = WritePsf(kVersion, kReserved, program, kTags);
    HSF_CHECK(file == OneCallPsf(kReserved, program, kTags));
    HSF_CHECK(copied.WritePsf(kVersion, kReserved, kTags) == file);
    HSF_CHECK(referred.WritePsf(kVersion, kReserved, kTags) == file);
    HSF_CHECK(WritePsf(kVersion, {}, {}, {}) == OneCallPsf({}, {}, {}));
    HSF_CHECK(ProgramBuilder{}.WritePsf(kVersion, {}, kTags) == OneCallPsf({}, {}, kTags));

    ProgramChunks chunks;
    HSF_CHECK(!hsf::ParsePsfChunks(file, chunks));
    HSF_CHECK(chunks.files.size() == files.size() && chunks.sound == 0x01000003u);
    for (std::size_t i = 0; i < files.size() && i < chunks.files.size(); ++i)
    {
        HSF_CHECK(chunks.files[i].path == "content/" + std::string(i, 'f') && chunks.files[i].data == files[i]);
    }
}

// The outcome of reading a program: its chunks, or the error.
struct ChunksRead
{
    std::string error;
    ProgramChunks chunks;
};

bool SameChunks(const ChunksRead& a, const ChunksRead& b)
{
    if (a.error != b.error || a.chunks.sound != b.chunks.sound || a.chunks.files.size() != b.chunks.files.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < a.chunks.files.size(); ++i)
    {
        if (a.chunks.files[i].path != b.chunks.files[i].path || a.chunks.files[i].data != b.chunks.files[i].data)
        {
            return false;
        }
    }

    return true;
}

// A PSF file's chunks read both ways: ParsePsf and then ParseProgram, and ParsePsfChunks. ParsePsfChunks reads the
// chunks straight from the zlib stream. The two ways must agree on every file, error included.
void CheckStreamedChunks(const std::vector<uint8_t>& file, const std::string& what)
{
    ChunksRead whole;
    PsfFile psf;
    if (const auto err = hsf::ParsePsf(file, psf))
    {
        whole.error = *err;
    }
    else if (const auto err2 = hsf::ParseProgram(psf.program, whole.chunks))
    {
        whole.error = *err2;
    }

    ChunksRead streamed;
    if (const auto err = hsf::ParsePsfChunks(file, streamed.chunks))
    {
        streamed.error = *err;
    }

    Check(SameChunks(whole, streamed),
          what + ": the streamed chunks match (\"" + whole.error + "\", \"" + streamed.error + "\")");
}

// A random program: FILE, SND and unknown chunks of random sizes, sometimes cut short or followed by stray bytes.
std::vector<uint8_t> RandomProgram(std::mt19937& rng)
{
    ProgramBuilder b;
    const int chunks = static_cast<int>(rng() % 6);
    for (int i = 0; i < chunks; ++i)
    {
        std::vector<uint8_t> data(rng() % 3 == 0 ? rng() % 200000 : rng() % 40);
        for (uint8_t& byte : data)
        {
            byte = static_cast<uint8_t>(rng() % 7);
        }

        if (rng() % 4 == 0)
        {
            b.AddSound(static_cast<uint32_t>(rng()));
        }
        else
        {
            b.AddFile(std::string(rng() % 12, 'p'), data);
        }
    }

    std::vector<uint8_t> program = b.Data();
    if (rng() % 3 == 0)
    {
        program.insert(program.begin(), {'X', 'Y', 'Z', 'W', 5, 0, 0, 0, 1, 2, 3, 4, 5, 0, 0, 0});
    }
    if (rng() % 3 == 0 && !program.empty())
    {
        program.resize(rng() % program.size());
    }
    if (rng() % 4 == 0)
    {
        program.insert(program.end(), rng() % 10, 0xEE);
    }

    return program;
}

void TestStreamedChunks()
{
    ProgramBuilder b;
    b.AddFile("content/sound/a.bfsar", std::vector<uint8_t>(3 << 20, 0x5A));
    b.AddSound(0x01000005);
    b.AddFile("Audio/other.bin", std::vector<uint8_t>{1, 2, 3});
    const std::vector<uint8_t> program = b.Data();
    CheckStreamedChunks(WritePsf(kVersion, {9}, program, {}), "a set's chunks");
    CheckStreamedChunks(WritePsf(kVersion, {}, {}, {}), "no program");

    // The chunk errors, a program that stops in a chunk's padding, and one with stray bytes after its last chunk.
    const std::vector<std::vector<uint8_t>> kPrograms = {
        {'F', 'I', 'L', 'E', 100, 0, 0, 0, 1, 2},
        {'F', 'I', 'L', 'E', 0xf8, 0xff, 0xff, 0xff, 1, 2, 3, 4},
        {'F', 'I', 'L', 'E', 4, 0, 0, 0, 9, 0, 'a', 'b'},
        {'F', 'I', 'L', 'E', 1, 0, 0, 0, 9, 0, 0, 0},
        {'S', 'N', 'D', ' ', 2, 0, 0, 0, 1, 2, 0, 0},
        {'S', 'N', 'D', ' ', 5, 0, 0, 0, 1, 2, 3, 4, 5},
        {'S', 'N', 'D', ' ', 4, 0, 0, 0, 1, 2, 3, 4, 7, 7, 7},
    };
    for (std::size_t i = 0; i < kPrograms.size(); ++i)
    {
        CheckStreamedChunks(WritePsf(kVersion, {}, kPrograms[i], {}), "program " + std::to_string(i));
    }

    // The container's errors.
    const std::vector<uint8_t> file = WritePsf(kVersion, {}, program, {});
    std::vector<uint8_t> bad = file;
    bad[40] ^= 0x55;
    CheckStreamedChunks(bad, "a damaged program");
    CheckStreamedChunks({file.begin(), file.begin() + 20}, "a cut file");
    CheckStreamedChunks({'P', 'S'}, "not a PSF file");
    const uint32_t half = hsf::LoadLe32(&file[8]) / 2;
    std::vector<uint8_t> short_stream(file.begin(), file.begin() + 16 + half);
    hsf::StoreLe32(&short_stream[8], half);
    hsf::StoreLe32(&short_stream[12], static_cast<uint32_t>(crc32(0, &short_stream[16], half)));
    CheckStreamedChunks(short_stream, "a stream cut short");

    std::mt19937 rng(28);
    for (int round = 0; round < 300 && failures == 0; ++round)
    {
        CheckStreamedChunks(WritePsf(kVersion, {}, RandomProgram(rng), {}), "random program " + std::to_string(round));
    }
}

} // namespace

int main()
{
    TestTimes();
    TestDecimals();
    TestDecimalsUnderCommaLocale();
    TestPsfRoundTrip();
    TestTagParsing();
    TestArchiveDescriptor();
    TestChunks();
    TestStreamedChunks();
    TestProgramPieces();
    TestLoadSet();
    TestLoadSetInArchive();

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    return 0;
}
