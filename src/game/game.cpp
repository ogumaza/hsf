// SPDX-License-Identifier: MIT

#include "game/game.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "xsflib/ascii.h"
#include "xsflib/files.h"

#include "game/bea.h"
#include "game/sheet.h"

namespace hsf::game
{
namespace
{

namespace fs = std::filesystem;

// Where a Bezel Engine game keeps its settings and its sound effect list.
constexpr char kBoot[] = "boot.lua";
constexpr char kVoiceSetting[] = "SetVoiceCountMax(";
constexpr char kEffectList[] = "Data/SheetBin/SoundEffectList.elf";

// The presets that the games put on the aux buses (StartupEffects), and an I3DL2 reverb's type as an aux effect.
constexpr char kDefaultReverb[] = "I3DL2_Default";
constexpr std::array<const char*, 3> kMoveItEffects = {"Delay_Waltz", "I3DL2_Waltz", "Limiter_Waltz"};
constexpr uint32_t kI3dl2Reverb = 1;

// Reads the first `count` bytes of the file at `path` into `out`, or all of it when it's shorter. Returns false if they
// can't be read.
bool ReadHead(const fs::path& path, std::size_t count, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
    {
        return false;
    }

    const std::streamoff size = f.tellg();
    if (size < 0)
    {
        return false;
    }

    f.seekg(0);
    out.resize(static_cast<std::size_t>(std::min<uint64_t>(static_cast<uint64_t>(size), count)));
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));

    return static_cast<std::size_t>(f.gcount()) == out.size();
}

bool StartsWith(std::span<const uint8_t> data, const char* magic)
{
    const std::size_t length = std::strlen(magic);
    return data.size() >= length && std::memcmp(data.data(), magic, length) == 0;
}

// The voice count a Bezel Engine game's boot.lua gives the engine, from a line that calls SetVoiceCountMax with a
// number, outside a comment.
std::optional<int> VoiceCount(const std::vector<uint8_t>& text)
{
    const std::string lua(text.begin(), text.end());
    for (std::size_t start = 0; start < lua.size();)
    {
        const std::size_t end = std::min(lua.find('\n', start), lua.size());
        const std::string line = lua.substr(start, end - start);
        start = end + 1;
        const std::size_t call = line.find(kVoiceSetting);
        const std::size_t comment = line.find("--");
        if (call == std::string::npos || (comment != std::string::npos && comment < call))
        {
            continue;
        }

        int count = 0;
        std::size_t digit = call + std::strlen(kVoiceSetting);
        const std::size_t first = digit;
        for (; digit < line.size() && line[digit] >= '0' && line[digit] <= '9' && count < 100000; ++digit)
        {
            count = count * 10 + (line[digit] - '0');
        }
        if (digit > first && digit < line.size() && line[digit] == ')')
        {
            return count;
        }
    }

    return std::nullopt;
}

// Adds the effects of a sound effect list to `out`. The list has WarioWare: Get It Together!'s form or WarioWare: Move
// It!'s. A list that doesn't parse adds none.
void AddEffectList(std::vector<uint8_t> bytes, GameFiles& out)
{
    Sheet sheet;
    if (Sheet::Parse(std::move(bytes), sheet))
    {
        return;
    }

    for (auto& [preset, parameters] : ReadI3dl2Presets(sheet))
    {
        out.i3dl2_presets[preset] = parameters;
    }

    RawEffectPresets raw = ReadRawEffectPresets(sheet);
    for (auto& [preset, parameters] : raw.i3dl2)
    {
        out.i3dl2_presets[preset] = parameters;
    }
    for (auto& [preset, effect] : raw.renderer)
    {
        out.renderer_presets[preset] = effect;
    }
}

// Where a file in a Bezel Engine archive is.
struct InArchive
{
    fs::path archive;
    BeaArchive::Asset asset;
};

// Reads a file in a Bezel Engine archive. Returns false if it can't.
bool ReadInArchive(const InArchive& where, std::vector<uint8_t>& data)
{
    std::vector<uint8_t> archive;
    return xsflib::ReadFile(where.archive, archive) && BeaArchive::Read(archive, where.asset, data);
}

// Adds a Bezel Engine archive's files to `out` under `prefix`, and notes where each one is.
void AddArchive(const fs::path& path, const std::string& prefix, GameFiles& out,
                std::map<std::string, InArchive>& in_archives)
{
    std::vector<uint8_t> data;
    BeaArchive bea;
    if (!xsflib::ReadFile(path, data) || BeaArchive::Parse(data, bea))
    {
        return;
    }

    for (const BeaArchive::Asset& asset : bea.Assets())
    {
        const std::string name = prefix + asset.name;
        out.files.push_back(name);
        in_archives[name] = {path, asset};
        std::vector<uint8_t> head;
        if (BeaArchive::ReadHead(data, asset, 4, head) && StartsWith(head, "FSAR"))
        {
            out.sound_archives.push_back(name);
        }

        std::vector<uint8_t> sheet_bytes;
        if (asset.name == kEffectList && BeaArchive::Read(data, asset, sheet_bytes))
        {
            AddEffectList(std::move(sheet_bytes), out);
        }
    }
}

// A file below a folder: its path from the folder, with '/' between folders, and its path on disk.
using FolderFile = std::pair<std::string, fs::path>;

// Lists every file below `input` in `files`, in the order of their paths from it. Returns an error message if the
// folder can't be read.
std::optional<std::string> ListFolder(const fs::path& input, std::vector<FolderFile>& files)
{
    std::error_code ec;
    for (fs::recursive_directory_iterator it(input, ec), end; !ec && it != end; it.increment(ec))
    {
        // A file whose type can't be read, such as a broken link, is passed over. The walk goes on.
        std::error_code type_error;
        if (it->is_regular_file(type_error))
        {
            files.emplace_back(it->path().lexically_relative(input).generic_string(), it->path());
        }
    }
    if (ec)
    {
        return "can't read the folder: " + ec.message();
    }

    std::sort(files.begin(), files.end());

    return std::nullopt;
}

// Reads a folder: every file below it, and the files in its Bezel Engine archives.
std::optional<std::string> LoadFolder(const fs::path& input, GameFiles& out)
{
    out.name = input.filename().string();
    std::vector<FolderFile> found;
    if (std::optional<std::string> error = ListFolder(input, found))
    {
        return error;
    }

    auto in_archives = std::make_shared<std::map<std::string, InArchive>>();
    const fs::path* boot = nullptr;
    std::ptrdiff_t boot_depth = 0;
    for (const auto& [name, path] : found)
    {
        out.files.push_back(name);
        std::vector<uint8_t> head;
        ReadHead(path, BeaArchive::kHeaderSize, head);
        if (StartsWith(head, "FSAR"))
        {
            out.sound_archives.push_back(name);
        }
        else if (BeaArchive::Is(head))
        {
            AddArchive(path, name + "/", out, *in_archives);
        }
        else if (name == kEffectList || name.ends_with(std::string("/") + kEffectList))
        {
            // The list in a Bezel Engine archive that was unpacked into a folder.
            std::vector<uint8_t> sheet_bytes;
            if (xsflib::ReadFile(path, sheet_bytes))
            {
                AddEffectList(std::move(sheet_bytes), out);
            }
        }
        else if (path.filename() == kBoot)
        {
            // The game's boot.lua is the one nearest the folder's top. The folder may be the RomFS or a folder that
            // holds it.
            const std::ptrdiff_t depth = std::count(name.begin(), name.end(), '/');
            if (boot == nullptr || depth < boot_depth)
            {
                boot = &path;
                boot_depth = depth;
            }
        }
    }

    std::vector<uint8_t> text;
    if (boot != nullptr && ReadHead(*boot, 1 << 20, text))
    {
        out.voice_count = VoiceCount(text);
    }

    const fs::path root = input;
    out.read_file = [root, in_archives](const std::string& path, std::vector<uint8_t>& data)
    {
        const auto found_in = in_archives->find(path);
        if (found_in != in_archives->end())
        {
            return ReadInArchive(found_in->second, data);
        }

        std::error_code error;
        const fs::path file = root / fs::path(path);
        return fs::is_regular_file(file, error) && xsflib::ReadFile(file, data);
    };

    return std::nullopt;
}

// Reads one Bezel Engine archive's files.
std::optional<std::string> LoadBeaArchive(const fs::path& input, GameFiles& out)
{
    std::vector<uint8_t> head;
    if (!ReadHead(input, BeaArchive::kHeaderSize, head))
    {
        return std::string("can't be read");
    }
    if (!BeaArchive::Is(head))
    {
        return std::string("isn't a folder, a Bezel Engine archive (.bea) or a sound archive");
    }

    out.name = input.stem().string();
    auto in_archives = std::make_shared<std::map<std::string, InArchive>>();
    AddArchive(input, input.filename().string() + "/", out, *in_archives);
    out.read_file = [in_archives](const std::string& path, std::vector<uint8_t>& data)
    {
        const auto found_in = in_archives->find(path);
        return found_in != in_archives->end() && ReadInArchive(found_in->second, data);
    };

    return std::nullopt;
}

// The effect of the preset `name` on aux bus `bus`: an I3DL2 reverb, or one of the renderer's effects. The game has the
// preset.
BusEffect PresetEffect(const GameFiles& game, const std::string& name, uint32_t bus)
{
    BusEffect effect;
    effect.bus = bus;
    effect.name = name;
    const auto reverb = game.i3dl2_presets.find(name);
    if (reverb != game.i3dl2_presets.end())
    {
        effect.type = kI3dl2Reverb;
        effect.parameters.assign(reverb->second.begin(), reverb->second.end());
    }
    else
    {
        const RendererPreset& preset = game.renderer_presets.at(name);
        effect.type = preset.type;
        effect.parameters.assign(preset.parameters.begin(), preset.parameters.end());
    }

    return effect;
}

} // namespace

std::vector<BusEffect> StartupEffects(const GameFiles& game)
{
    std::vector<BusEffect> effects;
    const auto has = [&game](const char* name)
    {
        return game.i3dl2_presets.contains(name) || game.renderer_presets.contains(name);
    };
    if (std::all_of(kMoveItEffects.begin(), kMoveItEffects.end(), has))
    {
        for (uint32_t bus = 0; bus < kMoveItEffects.size(); ++bus)
        {
            effects.push_back(PresetEffect(game, kMoveItEffects[bus], bus));
        }
    }
    else if (game.i3dl2_presets.contains(kDefaultReverb))
    {
        effects.push_back(PresetEffect(game, kDefaultReverb, 0));
    }

    return effects;
}

std::optional<std::string> EncryptedKind(const fs::path& path)
{
    std::vector<uint8_t> head;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || !ReadHead(path, 0x110, head))
    {
        return std::nullopt;
    }

    if (StartsWith(head, "PFS0"))
    {
        return std::string("an NSP");
    }
    if (head.size() >= 0x104 && std::memcmp(&head[0x100], "HEAD", 4) == 0)
    {
        return std::string("an XCI");
    }

    const std::string extension = xsflib::AsciiLower(path.extension().string());
    if (extension == ".nca" || extension == ".nsz" || extension == ".xcz")
    {
        return std::string("an ") + (extension == ".nca" ? "NCA" : extension == ".nsz" ? "NSZ" : "XCZ");
    }

    return std::nullopt;
}

std::optional<std::string> LoadGame(const fs::path& input, GameFiles& out)
{
    out = GameFiles{};
    std::error_code ec;
    if (fs::is_directory(input, ec))
    {
        return LoadFolder(input, out);
    }

    return LoadBeaArchive(input, out);
}

} // namespace hsf::game
