// SPDX-License-Identifier: MIT

// Tests the readers of a Switch game's files (src/game). The test builds a Bezel Engine archive with stored and
// compressed assets, a data sheet with a sound effect list, and a boot.lua, and puts them in a folder with a loose
// sound archive. It checks what LoadGame finds in the folder and in the archive alone: the files, the sound archives,
// the voice count and the I3DL2 reverbs, and that each file reads back as it was written. The test also loads a title's
// folder whose archive was unpacked. That folder holds three boot.lua files at two depths. A list in WarioWare: Move
// It!'s form gives an I3DL2 reverb, a delay and a light limiter. Sound sheets give their sounds' levels. Damaged
// archives and sheets must be refused, and the encrypted forms a game comes in must be recognised. The SARC archives
// beside a sound archive must be found among a game's files.
//
// usage: game_test

#include "game/game.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rip/archive.h"
#include "xsflib/bytes.h"
#include "xsflib/files.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/temp_folder.h"
#include <zstd.h>

#include "game/bea.h"
#include "game/sheet.h"

using namespace xsflib::testing;

namespace
{

namespace fs = std::filesystem;

using hsf::game::BeaArchive;
using hsf::game::GameFiles;
using hsf::game::I3dl2Preset;
using hsf::game::Sheet;

struct AssetSpec
{
    std::string name;
    std::vector<uint8_t> data;
    bool compress = false;
};

// A record of the sound effect list: its name, its type (1 for an I3DL2 reverb) and its parameters in the game's units.
struct EffectSpec
{
    std::string name;
    uint32_t type = 1;
    I3dl2Preset parameters{};
};

// The parameters of the first reverb in the list, in the game's units: times in milliseconds, and the high-frequency
// decay ratio and the dry gain in percent.
constexpr I3dl2Preset kGameUnits = {-1000.0f, -100.0f, 1490.0f, 83.0f, -2602.0f, 7.0f,
                                    200.0f,   11.0f,   100.0f,  90.0f, 5000.0f,  70.0f};

void Pad(std::vector<uint8_t>& out, std::size_t alignment)
{
    while (out.size() % alignment != 0)
    {
        out.push_back(0);
    }
}

std::vector<uint8_t> Bytes(const std::string& text)
{
    return std::vector<uint8_t>(text.begin(), text.end());
}

// A sound archive's first bytes, enough for the readers to distinguish it from other files.
std::vector<uint8_t> SoundArchive(uint8_t fill)
{
    std::vector<uint8_t> data = Bytes("FSAR");
    data.insert(data.end(), 60, fill);

    return data;
}

std::vector<uint8_t> Compress(const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> out(ZSTD_compressBound(data.size()));
    const std::size_t size = ZSTD_compress(out.data(), out.size(), data.data(), data.size(), 3);
    XSFLIB_CHECK(!ZSTD_isError(size));
    out.resize(ZSTD_isError(size) ? 0 : size);

    return out;
}

// The two layouts of an archive's asset table: the offsets of the assets' headers (WarioWare: Get It Together! and
// WarioWare: Move It!), or the headers themselves, 0x48 bytes each (Super Mario Party Jamboree).
enum class Layout
{
    kOffsets,
    kInline,
};

// A little-endian Bezel Engine archive: the SCNE header and the asset table, then each asset's name and data. In the
// layout with offsets, each asset's ASST header comes before its name.
std::vector<uint8_t> Bea(const std::vector<AssetSpec>& assets, Layout layout = Layout::kOffsets)
{
    const bool in_table = layout == Layout::kInline;
    const std::size_t header = in_table ? 0x48 : 0x30;
    std::vector<uint8_t> out(0x30);
    out[0] = 'S';
    out[1] = 'C';
    out[2] = 'N';
    out[3] = 'E';
    out[0xA] = in_table ? 6 : 0; // bytes 8 to 11 as Jamboree's archives have them; the reader ignores them
    out[0xC] = 0xFF;
    out[0xD] = 0xFE;
    xsflib::StoreLe32(&out[0x20], static_cast<uint32_t>(assets.size()));
    xsflib::StoreLe64(&out[0x28], out.size());
    const std::size_t table = out.size();
    out.resize(table + (in_table ? header : 8) * assets.size());

    for (std::size_t i = 0; i < assets.size(); ++i)
    {
        std::size_t entry = table + header * i;
        if (!in_table)
        {
            entry = out.size();
            xsflib::StoreLe64(&out[table + 8 * i], entry);
            out.resize(entry + header);
        }
        out[entry] = 'A';
        out[entry + 1] = 'S';
        out[entry + 2] = 'S';
        out[entry + 3] = 'T';

        const std::size_t name = out.size();
        out.resize(name + 2);
        xsflib::StoreLe16(&out[name], static_cast<uint16_t>(assets[i].name.size()));
        out.insert(out.end(), assets[i].name.begin(), assets[i].name.end());
        Pad(out, 8);

        const std::vector<uint8_t> stored = assets[i].compress ? Compress(assets[i].data) : assets[i].data;
        out[entry + 0x10] = assets[i].compress ? 2 : 0;
        xsflib::StoreLe32(&out[entry + 0x14], static_cast<uint32_t>(stored.size()));
        if (in_table)
        {
            // The size takes 32 bits, and a tag follows it. A hash at +0x28 goes unread.
            xsflib::StoreLe32(&out[entry + 0x18], static_cast<uint32_t>(assets[i].data.size()));
            std::copy_n("_ADDAUD", 7, &out[entry + 0x1C]);
            std::fill_n(&out[entry + 0x28], 16, uint8_t{0xA5});
            xsflib::StoreLe64(&out[entry + 0x38], out.size());
            xsflib::StoreLe64(&out[entry + 0x40], name);
        }
        else
        {
            xsflib::StoreLe64(&out[entry + 0x18], assets[i].data.size());
            xsflib::StoreLe64(&out[entry + 0x20], out.size());
            xsflib::StoreLe64(&out[entry + 0x28], name);
        }
        out.insert(out.end(), stored.begin(), stored.end());
        Pad(out, 8);
    }

    return out;
}

// A record of a data sheet: its name, and its 32-bit words by their offsets in the record.
struct RecordSpec
{
    std::string name;
    std::vector<std::pair<std::size_t, uint32_t>> words;
};

// A data sheet with a list called `list`, of records of `size` bytes: a little-endian AArch64 ELF relocatable object.
// Section 1 holds the records and their count, section 2 the names, sections 3 and 4 the symbols and their names, and
// section 5 a relocation for each record's pointer to its name, against section 2's symbol. `first_record`, if set, is
// another symbol that names the first record.
std::vector<uint8_t> SheetOf(const std::string& list, std::size_t size, const std::vector<RecordSpec>& specs,
                             const std::string& first_record = "")
{
    std::vector<uint8_t> records(size * specs.size() + 4);
    std::vector<uint8_t> names;
    std::vector<uint8_t> relocations;
    for (std::size_t i = 0; i < specs.size(); ++i)
    {
        for (const auto& [offset, word] : specs[i].words)
        {
            xsflib::StoreLe32(&records[size * i + offset], word);
        }

        const std::size_t relocation = relocations.size();
        relocations.resize(relocation + 24);
        xsflib::StoreLe64(&relocations[relocation], size * i);
        xsflib::StoreLe64(&relocations[relocation + 8], (uint64_t{1} << 32) | 257); // symbol 1, R_AARCH64_ABS64
        xsflib::StoreLe64(&relocations[relocation + 16], names.size());
        names.insert(names.end(), specs[i].name.begin(), specs[i].name.end());
        names.push_back(0);
    }
    xsflib::StoreLe32(&records[size * specs.size()], static_cast<uint32_t>(specs.size()));

    // The symbols: none, section 2's symbol, then the list and its count in section 1.
    std::vector<uint8_t> strings = Bytes(std::string(1, '\0') + list + '\0' + list + "Count" + '\0');
    std::vector<uint8_t> symbols(4 * 24);
    symbols[24 + 4] = 3; // STT_SECTION
    xsflib::StoreLe16(&symbols[24 + 6], 2);
    xsflib::StoreLe32(&symbols[48], 1);
    xsflib::StoreLe16(&symbols[48 + 6], 1);
    xsflib::StoreLe64(&symbols[48 + 16], size * specs.size());
    xsflib::StoreLe32(&symbols[72], static_cast<uint32_t>(list.size() + 2));
    xsflib::StoreLe16(&symbols[72 + 6], 1);
    xsflib::StoreLe64(&symbols[72 + 8], size * specs.size());
    xsflib::StoreLe64(&symbols[72 + 16], 4);
    if (!first_record.empty())
    {
        symbols.resize(5 * 24);
        xsflib::StoreLe32(&symbols[96], static_cast<uint32_t>(strings.size()));
        xsflib::StoreLe16(&symbols[96 + 6], 1);
        xsflib::StoreLe64(&symbols[96 + 16], size);
        strings.insert(strings.end(), first_record.begin(), first_record.end());
        strings.push_back(0);
    }

    // The header, the sections' data, then the section headers.
    std::vector<uint8_t> out(0x40);
    out[0] = 0x7F;
    out[1] = 'E';
    out[2] = 'L';
    out[3] = 'F';
    out[4] = 2;
    out[5] = 1;
    out[6] = 1;
    xsflib::StoreLe16(&out[0x10], 1);
    xsflib::StoreLe16(&out[0x12], 183);
    xsflib::StoreLe32(&out[0x14], 1);
    xsflib::StoreLe16(&out[0x34], 0x40);
    xsflib::StoreLe16(&out[0x3A], 0x40);
    xsflib::StoreLe16(&out[0x3C], 6);

    struct SectionSpec
    {
        uint32_t type;
        const std::vector<uint8_t>* data;
        uint32_t link;
        uint32_t info;
    };
    const SectionSpec sections[] = {
        {1, &records, 0, 0}, {1, &names, 0, 0}, {2, &symbols, 4, 2}, {3, &strings, 0, 0}, {4, &relocations, 3, 1}};
    std::vector<uint8_t> headers(0x40);
    for (const SectionSpec& section : sections)
    {
        Pad(out, 8);
        const std::size_t header = headers.size();
        headers.resize(header + 0x40);
        xsflib::StoreLe32(&headers[header + 4], section.type);
        xsflib::StoreLe64(&headers[header + 0x18], out.size());
        xsflib::StoreLe64(&headers[header + 0x20], section.data->size());
        xsflib::StoreLe32(&headers[header + 0x28], section.link);
        xsflib::StoreLe32(&headers[header + 0x2C], section.info);
        out.insert(out.end(), section.data->begin(), section.data->end());
    }
    Pad(out, 8);
    xsflib::StoreLe64(&out[0x28], out.size());
    out.insert(out.end(), headers.begin(), headers.end());

    return out;
}

// A sound effect list of WarioWare: Get It Together!'s form: records of 0x88 bytes, with the type at +0x8 and an I3DL2
// reverb's 12 parameters from +0x38.
std::vector<uint8_t> EffectSheet(const std::vector<EffectSpec>& effects)
{
    std::vector<RecordSpec> records;
    for (const EffectSpec& effect : effects)
    {
        RecordSpec record{effect.name, {{8, effect.type}}};
        for (std::size_t k = 0; k < effect.parameters.size(); ++k)
        {
            record.words.emplace_back(0x38 + 4 * k, std::bit_cast<uint32_t>(effect.parameters[k]));
        }
        records.push_back(record);
    }

    return SheetOf("s_SoundEffectList", 0x88, records);
}

// A record of a list in WarioWare: Move It!'s form: its name, its type and its parameters in the game's units.
struct RawEffectSpec
{
    std::string name;
    uint32_t type = 1;
    std::vector<float> parameters;
};

// A sound effect list of Move It!'s form: records of 0xB8 bytes, with the type at +0x8. An I3DL2 reverb's 12 parameters
// start at +0x38, a delay's 7 at +0x68, and those of any other type at +0x84.
std::vector<uint8_t> RawEffectSheet(const std::vector<RawEffectSpec>& effects)
{
    std::vector<RecordSpec> records;
    for (const RawEffectSpec& effect : effects)
    {
        const std::size_t start = effect.type == 1 ? 0x38 : effect.type == 2 ? 0x68 : 0x84;
        RecordSpec record{effect.name, {{8, effect.type}}};
        for (std::size_t k = 0; k < effect.parameters.size(); ++k)
        {
            record.words.emplace_back(start + 4 * k, std::bit_cast<uint32_t>(effect.parameters[k]));
        }
        records.push_back(record);
    }

    return SheetOf("s_RawEffectDataList", 0xB8, records);
}

// Writes `data` to the file at `path` after making the folders that hold it.
void WriteFile(const fs::path& path, const std::vector<uint8_t>& data)
{
    fs::create_directories(path.parent_path());
    XSFLIB_CHECK(!xsflib::WriteFile(path, data));
}

// What the game's files hold, by their paths in the game.
const std::vector<uint8_t> kStored = SoundArchive(0x11);
const std::vector<uint8_t> kPacked = SoundArchive(0x22);
const std::vector<uint8_t> kLoose = SoundArchive(0x33);
const std::vector<uint8_t> kText = Bytes("not a sound archive\n");

// The game's sound effect list: the default reverb, a hall and a delay. The two reverbs are I3DL2 presets.
std::vector<uint8_t> GameEffectList()
{
    EffectSpec hall{"Hall", 1, kGameUnits};
    hall.parameters[2] = 3920.0f;
    const EffectSpec kDelay{"Delay", 2, kGameUnits};

    return EffectSheet({{"I3DL2_Default", 1, kGameUnits}, hall, kDelay});
}

std::vector<AssetSpec> GameAssets()
{
    return {{"Audio/Stored.bfsar", kStored, false},
            {"Audio/Packed.bfsar", kPacked, true},
            {"Data/SheetBin/SoundEffectList.elf", GameEffectList(), true},
            {"Audio/Readme.txt", kText, false}};
}

void CheckPresets(const GameFiles& game)
{
    // The game's times are in milliseconds, and its ratio and dry gain in percent.
    I3dl2Preset expected = kGameUnits;
    expected[2] = kGameUnits[2] / 1000.0f;
    expected[3] = kGameUnits[3] / 100.0f;
    expected[5] = kGameUnits[5] / 1000.0f;
    expected[7] = kGameUnits[7] / 1000.0f;
    expected[11] = kGameUnits[11] / 100.0f;
    I3dl2Preset hall = expected;
    hall[2] = 3920.0f / 1000.0f;

    XSFLIB_CHECK(game.i3dl2_presets.size() == 2);
    XSFLIB_CHECK(game.i3dl2_presets.contains("I3DL2_Default") && game.i3dl2_presets.at("I3DL2_Default") == expected);
    XSFLIB_CHECK(game.i3dl2_presets.contains("Hall") && game.i3dl2_presets.at("Hall") == hall);
}

// Reads `path` from the game's files, and checks that it holds `expected`.
void CheckRead(const GameFiles& game, const std::string& path, const std::vector<uint8_t>& expected)
{
    std::vector<uint8_t> data;
    Check(game.read_file && game.read_file(path, data) && data == expected, path + " reads back as written");
}

void TestFolder(const fs::path& dir)
{
    WriteFile(dir / "game" / "Archive" / "Music.nx.bea", Bea(GameAssets()));
    WriteFile(dir / "game" / "Audio" / "Loose.bfsar", kLoose);
    WriteFile(dir / "game" / "Notes.txt", kText);
    WriteFile(dir / "game" / "boot.lua", Bytes("-- engine:SetVoiceCountMax(10)\nlocal engine = BezelEngine\n"
                                               "engine:SetVoiceCountMax(count)\nengine:SetVoiceCountMax(72)\n"));

    GameFiles game;
    const std::optional<std::string> error = hsf::game::LoadGame(dir / "game", game);

    XSFLIB_CHECK(!error);
    XSFLIB_CHECK(game.name == "game");
    const std::vector<std::string> kFiles = {"Archive/Music.nx.bea",
                                             "Archive/Music.nx.bea/Audio/Stored.bfsar",
                                             "Archive/Music.nx.bea/Audio/Packed.bfsar",
                                             "Archive/Music.nx.bea/Data/SheetBin/SoundEffectList.elf",
                                             "Archive/Music.nx.bea/Audio/Readme.txt",
                                             "Audio/Loose.bfsar",
                                             "Notes.txt",
                                             "boot.lua"};
    XSFLIB_CHECK(game.files == kFiles);
    const std::vector<std::string> kSoundArchives = {"Archive/Music.nx.bea/Audio/Stored.bfsar",
                                                     "Archive/Music.nx.bea/Audio/Packed.bfsar", "Audio/Loose.bfsar"};
    XSFLIB_CHECK(game.sound_archives == kSoundArchives);
    XSFLIB_CHECK(game.voice_count == 72);
    CheckPresets(game);
    CheckRead(game, "Archive/Music.nx.bea/Audio/Stored.bfsar", kStored);
    CheckRead(game, "Archive/Music.nx.bea/Audio/Packed.bfsar", kPacked);
    CheckRead(game, "Archive/Music.nx.bea/Audio/Readme.txt", kText);
    CheckRead(game, "Audio/Loose.bfsar", kLoose);
    std::vector<uint8_t> data;
    XSFLIB_CHECK(!game.read_file("Missing.bfsar", data));
    XSFLIB_CHECK(!game.read_file("Archive/Music.nx.bea/Missing.bfsar", data));
}

// A game whose folder and files have names that aren't ASCII. The game's name and its files' paths are UTF-8, and each
// file reads back through its path. On Windows, the names don't go through the code page.
void TestUtf8Folder(const fs::path& dir)
{
    const std::string title = "\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0"; // three katakana in UTF-8
    const std::string audio = "Audio \xc3\xa9";                       // "Audio" and an e with an acute accent
    WriteFile(dir / xsflib::PathFromUtf8(title) / xsflib::PathFromUtf8(audio) / "Loose.bfsar", kLoose);

    GameFiles game;
    const std::optional<std::string> error = hsf::game::LoadGame(dir / xsflib::PathFromUtf8(title), game);

    XSFLIB_CHECK(!error);
    XSFLIB_CHECK(game.name == title);
    XSFLIB_CHECK(game.files == std::vector<std::string>{audio + "/Loose.bfsar"});
    XSFLIB_CHECK(game.sound_archives == std::vector<std::string>{audio + "/Loose.bfsar"});
    CheckRead(game, audio + "/Loose.bfsar", kLoose);
}

void TestUnpackedFolder(const fs::path& dir)
{
    // A title's folder holds the RomFS. The RomFS holds the Bezel Engine archive unpacked into a folder, the game's
    // boot.lua at the top, two deeper decoys named boot.lua and the sound effect list as a loose file. The walk reads
    // one decoy before the game's boot.lua and the other after it.
    const fs::path romfs = dir / "title" / "romfs";
    WriteFile(romfs / "boot.lua", Bytes("engine:SetVoiceCountMax(72)\n"));
    WriteFile(romfs / "Scripts" / "boot.lua", Bytes("engine:SetVoiceCountMax(10)\n"));
    WriteFile(romfs / "data" / "boot.lua", Bytes("engine:SetVoiceCountMax(20)\n"));
    WriteFile(romfs / "Archive" / "Music.nx.bea" / "Data" / "SheetBin" / "SoundEffectList.elf", GameEffectList());
    WriteFile(romfs / "Archive" / "Music.nx.bea" / "Audio" / "Stored.bfsar", kStored);

    GameFiles game;
    const std::optional<std::string> error = hsf::game::LoadGame(dir / "title", game);

    XSFLIB_CHECK(!error);
    XSFLIB_CHECK(game.voice_count == 72);
    CheckPresets(game);
    const std::vector<std::string> kSoundArchives = {"romfs/Archive/Music.nx.bea/Audio/Stored.bfsar"};
    XSFLIB_CHECK(game.sound_archives == kSoundArchives);
}

void TestArchive(const fs::path& dir)
{
    WriteFile(dir / "Music.nx.bea", Bea(GameAssets()));

    GameFiles game;
    const std::optional<std::string> error = hsf::game::LoadGame(dir / "Music.nx.bea", game);

    XSFLIB_CHECK(!error);
    XSFLIB_CHECK(game.name == "Music.nx");
    XSFLIB_CHECK(game.files.size() == 4 && game.files[0] == "Music.nx.bea/Audio/Stored.bfsar");
    const std::vector<std::string> kSoundArchives = {"Music.nx.bea/Audio/Stored.bfsar",
                                                     "Music.nx.bea/Audio/Packed.bfsar"};
    XSFLIB_CHECK(game.sound_archives == kSoundArchives);
    XSFLIB_CHECK(!game.voice_count);
    CheckPresets(game);
    CheckRead(game, "Music.nx.bea/Audio/Packed.bfsar", kPacked);
    std::vector<uint8_t> data;
    XSFLIB_CHECK(!game.read_file("Audio/Packed.bfsar", data));

    // A file that's neither a folder nor an archive, and a path that doesn't exist, are refused.
    WriteFile(dir / "Notes.txt", kText);
    XSFLIB_CHECK(hsf::game::LoadGame(dir / "Notes.txt", game).has_value());
    XSFLIB_CHECK(hsf::game::LoadGame(dir / "Missing.bea", game).has_value());
}

// Super Mario Party Jamboree's layout: the asset table holds the assets' headers. A damaged one is refused.
void TestInlineArchive(const fs::path& dir)
{
    const std::vector<uint8_t> bea = Bea(GameAssets(), Layout::kInline);
    WriteFile(dir / "Music.nx.bea", bea);

    GameFiles game;
    const std::optional<std::string> error = hsf::game::LoadGame(dir / "Music.nx.bea", game);

    XSFLIB_CHECK(!error);
    XSFLIB_CHECK(game.files.size() == 4 && game.files[0] == "Music.nx.bea/Audio/Stored.bfsar");
    const std::vector<std::string> kSoundArchives = {"Music.nx.bea/Audio/Stored.bfsar",
                                                     "Music.nx.bea/Audio/Packed.bfsar"};
    XSFLIB_CHECK(game.sound_archives == kSoundArchives);
    CheckPresets(game);
    CheckRead(game, "Music.nx.bea/Audio/Stored.bfsar", kStored);
    CheckRead(game, "Music.nx.bea/Audio/Packed.bfsar", kPacked);
    CheckRead(game, "Music.nx.bea/Audio/Readme.txt", kText);

    // A table that runs past the end, a second header without its ASST, and data that runs past the end.
    BeaArchive parsed;
    std::vector<uint8_t> damaged = bea;
    xsflib::StoreLe32(&damaged[0x20], 0x10000);
    XSFLIB_CHECK(BeaArchive::Parse(damaged, parsed).has_value());
    damaged = bea;
    damaged[0x30 + 0x48] = 'X';
    XSFLIB_CHECK(BeaArchive::Parse(damaged, parsed).has_value());
    damaged = bea;
    xsflib::StoreLe64(&damaged[0x30 + 0x38], bea.size());
    XSFLIB_CHECK(BeaArchive::Parse(damaged, parsed).has_value());
}

// A list of Move It!'s form: an I3DL2 reverb, a delay, a light limiter, and a record of type 4 that the reader leaves
// out. The game divides the times by 1000 (ms to s). It gives nn::atk a delay's longest delay and each time of a light
// limiter in whole milliseconds, and a delay's delay in whole microseconds, on 2 channels.
void TestRawEffectList(const fs::path& dir)
{
    const std::vector<RawEffectSpec> kEffects = {
        {"I3DL2_Waltz",
         1,
         {-1000.0f, -1000.0f, 2000.0f, 0.8f, -700.0f, 5.0f, -500.0f, 40.0f, 50.0f, 3.0f, 6000.0f, 0.0f}},
        {"Delay_Waltz", 2, {1999.9f, 250.7004f, 0.5f, 0.2f, 0.0f, 0.4f, 0.3f}},
        {"Limiter_Waltz", 3, {1.0f, 0.02f, 50.9f, 0.5f, 0.3f, 1.0f, 1.0f}},
        {"Other", 4, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f}}};
    WriteFile(dir / "romfs" / "Data" / "SheetBin" / "SoundEffectList.elf", RawEffectSheet(kEffects));

    GameFiles game;
    const std::optional<std::string> error = hsf::game::LoadGame(dir / "romfs", game);

    const I3dl2Preset kReverb = {-1000.0f, -1000.0f, 2.0f,  0.8f, -700.0f, 0.005f,
                                 -500.0f,  0.04f,    50.0f, 3.0f, 6000.0f, 0.0f};
    const hsf::game::RendererPreset kDelay{2, {1999000.0f, 250700.0f, 0.5f, 0.2f, 0.0f, 0.4f, 0.3f, 1.0f}};
    const hsf::game::RendererPreset kLimiter{3, {1000.0f, 0.0f, 50000.0f, 0.0f, 0.3f, 1.0f, 1.0f, 1.0f}};
    XSFLIB_CHECK(!error);
    XSFLIB_CHECK(game.i3dl2_presets.size() == 1 && game.i3dl2_presets.at("I3DL2_Waltz") == kReverb);
    XSFLIB_CHECK(game.renderer_presets.size() == 2 && game.renderer_presets.at("Delay_Waltz") == kDelay &&
                 game.renderer_presets.at("Limiter_Waltz") == kLimiter);
}

// A sound sheet of WarioWare: Get It Together!'s form, with one record of 0x98 bytes: `sound` at `level`.
std::vector<uint8_t> SoundSheet(const std::string& sound, float level)
{
    const uint32_t kUnset = std::bit_cast<uint32_t>(-999999.0f);
    const RecordSpec record{sound, {{0x18, std::bit_cast<uint32_t>(level)}, {0x4C, kUnset}, {0x50, kUnset}}};

    return SheetOf("s_VoiceDataTable", 0x98, {record}, "s_DefaultVoiceData");
}

// A game's sound sheets give its sounds' levels, from a Bezel Engine archive and from one unpacked into a folder. A
// voice sheet, a sheet in another folder and a file that isn't a sheet add nothing.
void TestSoundSheets(const fs::path& dir)
{
    const fs::path archive = fs::path("Archive") / "Data~SoundData.nx.bea";
    const std::vector<AssetSpec> assets = {
        {"Data/SheetBin/SoundVoice_BGM.elf", SoundSheet("BGM_Title", 0.71f), true},
        {"Data/SheetBin/SoundVoice_SE.elf", SoundSheet("SE_Click", 2.0f), false},
        {"Data/SheetBin/SoundVoice_Damaged.elf", kText, false},
        {"Data/SheetBin/SoundVoice_Voice_enUS.elf", SoundSheet("BGM_Title", 0.25f), false},
        {"Data/SheetBinX/SoundVoice_Elsewhere.elf", SoundSheet("BGM_Other", 0.25f), false}};
    WriteFile(dir / "packed" / archive, Bea(assets));
    for (const AssetSpec& asset : assets)
    {
        WriteFile(dir / "unpacked" / archive / asset.name, asset.data);
    }

    const auto check = [](const fs::path& input)
    {
        GameFiles game;
        const std::optional<std::string> error = hsf::game::LoadGame(input, game);
        XSFLIB_CHECK(!error);
        Check(game.sound_levels.Count() == 2, input.filename().string() + ": two sounds have levels");
        Check(game.sound_levels.LevelOf("BGM_Title") == 0.71f, input.filename().string() + ": a sound's level");
        Check(game.sound_levels.LevelOf("SE_Click") == 2.0f, input.filename().string() + ": another sheet's level");
        Check(game.sound_levels.LevelOf("BGM_Other") == 1.0f,
              input.filename().string() + ": a sheet elsewhere adds none");
    };
    check(dir / "packed");
    check(dir / "packed" / archive);
    check(dir / "unpacked");
}

void TestDamagedArchives()
{
    const std::vector<uint8_t> good = Bea(GameAssets());
    BeaArchive bea;
    XSFLIB_REQUIRE(!BeaArchive::Parse(good, bea) && bea.Assets().size() == 4);

    // The first bytes of a compressed asset, without the rest.
    std::vector<uint8_t> head;
    XSFLIB_CHECK(BeaArchive::ReadHead(good, bea.Assets()[1], 4, head) && head == Bytes("FSAR"));

    // A big-endian byte order mark, a table past the end, an asset without its ASST header, an asset whose data runs
    // past the end, and an unknown compression.
    std::vector<uint8_t> damaged = good;
    damaged[0xC] = 0xFE;
    damaged[0xD] = 0xFF;
    XSFLIB_CHECK(!BeaArchive::Is(damaged) && BeaArchive::Parse(damaged, bea).has_value());
    damaged = good;
    xsflib::StoreLe32(&damaged[0x20], 0x10000000);
    XSFLIB_CHECK(BeaArchive::Parse(damaged, bea).has_value());
    damaged = good;
    damaged[xsflib::LoadLe64(&good[0x30])] = 'X';
    XSFLIB_CHECK(BeaArchive::Parse(damaged, bea).has_value());
    damaged = good;
    xsflib::StoreLe64(&damaged[xsflib::LoadLe64(&good[0x30]) + 0x18], good.size());
    XSFLIB_CHECK(BeaArchive::Parse(damaged, bea).has_value());
    damaged = good;
    damaged[xsflib::LoadLe64(&good[0x38]) + 0x10] = 1;
    std::vector<uint8_t> data;
    XSFLIB_CHECK(!BeaArchive::Parse(damaged, bea) && !BeaArchive::Read(damaged, bea.Assets()[1], data));
}

void TestDamagedSheets()
{
    const std::vector<uint8_t> good = EffectSheet({{"I3DL2_Default", 1, kGameUnits}});
    Sheet sheet;
    XSFLIB_CHECK(!Sheet::Parse(good, sheet));
    XSFLIB_CHECK(sheet.Symbol("s_SoundEffectListCount").size() == 4 && sheet.Symbol("s_Missing").empty());
    XSFLIB_CHECK(sheet.StringAt("s_SoundEffectList", 0) == "I3DL2_Default");
    XSFLIB_CHECK(!sheet.StringAt("s_SoundEffectList", 8));

    // Not an ELF object, a 32-bit or big-endian one, one for another machine, and section headers past the end.
    std::vector<uint8_t> damaged = good;
    damaged[1] = 'X';
    XSFLIB_CHECK(Sheet::Parse(damaged, sheet).has_value());
    damaged = good;
    damaged[4] = 1;
    XSFLIB_CHECK(Sheet::Parse(damaged, sheet).has_value());
    damaged = good;
    damaged[5] = 2;
    XSFLIB_CHECK(Sheet::Parse(damaged, sheet).has_value());
    damaged = good;
    xsflib::StoreLe16(&damaged[0x12], 62);
    XSFLIB_CHECK(Sheet::Parse(damaged, sheet).has_value());
    damaged = good;
    damaged.resize(damaged.size() - 1);
    XSFLIB_CHECK(Sheet::Parse(damaged, sheet).has_value());

    // A record with no pointer to its name is left out.
    damaged = good;
    const std::size_t relocations = xsflib::LoadLe64(&good[xsflib::LoadLe64(&good[0x28]) + 5 * 0x40 + 0x18]);
    xsflib::StoreLe64(&damaged[relocations], 8);
    XSFLIB_CHECK(!Sheet::Parse(damaged, sheet) && hsf::game::ReadI3dl2Presets(sheet).empty());
}

void TestEncrypted(const fs::path& dir)
{
    std::vector<uint8_t> nsp = Bytes("PFS0");
    nsp.resize(0x200);
    WriteFile(dir / "game.nsp", nsp);
    std::vector<uint8_t> xci(0x200);
    xci[0x100] = 'H';
    xci[0x101] = 'E';
    xci[0x102] = 'A';
    xci[0x103] = 'D';
    WriteFile(dir / "game.bin", xci);
    WriteFile(dir / "content.NCA", kText);
    WriteFile(dir / "Notes.txt", kText);

    XSFLIB_CHECK(hsf::game::EncryptedKind(dir / "game.nsp") == "an NSP");
    XSFLIB_CHECK(hsf::game::EncryptedKind(dir / "game.bin") == "an XCI");
    XSFLIB_CHECK(hsf::game::EncryptedKind(dir / "content.NCA") == "an NCA");
    XSFLIB_CHECK(!hsf::game::EncryptedKind(dir / "Notes.txt"));
    XSFLIB_CHECK(!hsf::game::EncryptedKind(dir));
    XSFLIB_CHECK(!hsf::game::EncryptedKind(dir / "Missing.nsp"));
}

// ntdlib::rip::SarcsBeside lists the SARC archives among a game's files in the sound archive's folder only: not those
// in the folders below it, in another folder whose name starts with the same text, or with another extension. A sound
// archive in no folder takes those at the top, and one in a Bezel Engine archive those in its folder there.
void TestSarcsBeside()
{
    hsf::game::GameFiles game;
    game.files = {"SoundData/SeData.bfsar", "SoundData/b.sarc",  "SoundData/a.sarc", "SoundData/sub/c.sarc",
                  "SoundData/e.sarc.zs",    "SoundDataX/f.sarc", "top.sarc",         "Archive/A.bea/Sound/x.sarc",
                  "Archive/A.bea/y.sarc"};

    const std::vector<std::string> beside = {"SoundData/a.sarc", "SoundData/b.sarc"};
    XSFLIB_CHECK(ntdlib::rip::SarcsBeside(game.files, "SoundData/SeData.bfsar") == beside);
    XSFLIB_CHECK(ntdlib::rip::SarcsBeside(game.files, "Sound.bfsar") == std::vector<std::string>{"top.sarc"});
    XSFLIB_CHECK(ntdlib::rip::SarcsBeside(game.files, "Archive/A.bea/Sound/s.bfsar") ==
                 std::vector<std::string>{"Archive/A.bea/Sound/x.sarc"});
    XSFLIB_CHECK(ntdlib::rip::SarcsBeside(game.files, "Other/s.bfsar").empty());
}

} // namespace

int main()
{
    const xsflib::testing::TempFolder temp("hsf_game_test");
    const fs::path& dir = temp.Path();

    TestFolder(dir / "folder");
    TestUtf8Folder(dir / "utf8");
    TestUnpackedFolder(dir / "unpacked");
    TestArchive(dir / "archive");
    TestInlineArchive(dir / "inline");
    TestRawEffectList(dir / "raw");
    TestSoundSheets(dir / "sheets");
    TestDamagedArchives();
    TestDamagedSheets();
    TestEncrypted(dir / "encrypted");
    TestSarcsBeside();

    return Summary("game");
}
