// SPDX-License-Identifier: MIT

#include "game/game.h"

#include <algorithm>
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

bool ReadFile(const fs::path& path, std::vector<uint8_t>& data, std::size_t limit = SIZE_MAX)
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
    data.resize(static_cast<std::size_t>(std::min<uint64_t>(static_cast<uint64_t>(size), limit)));
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));

    return static_cast<std::size_t>(f.gcount()) == data.size();
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

// Adds the I3DL2 reverbs of a sound effect list to `out`. A list that doesn't parse adds none.
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
}

// Where a file in a Bezel Engine archive is.
struct InArchive
{
    fs::path archive;
    BeaArchive::Asset asset;
};

// Adds a Bezel Engine archive's files to `out` under `prefix`, and notes where each one is.
void AddArchive(const fs::path& path, const std::string& prefix, GameFiles& out,
                std::map<std::string, InArchive>& in_archives)
{
    std::vector<uint8_t> data;
    BeaArchive bea;
    if (!ReadFile(path, data) || BeaArchive::Parse(data, bea))
    {
        return;
    }

    for (const BeaArchive::Asset& asset : bea.Assets())
    {
        const std::string name = prefix + asset.name;
        out.files.push_back(name);
        in_archives[name] = {path, asset};
        std::vector<uint8_t> head;
        if (bea.ReadHead(data, asset, 4, head) && StartsWith(head, "FSAR"))
        {
            out.sound_archives.push_back(name);
        }

        std::vector<uint8_t> sheet_bytes;
        if (asset.name == kEffectList && bea.Read(data, asset, sheet_bytes))
        {
            AddEffectList(std::move(sheet_bytes), out);
        }
    }
}

} // namespace

std::optional<std::string> EncryptedKind(const fs::path& path)
{
    std::vector<uint8_t> head;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || !ReadFile(path, head, 0x110))
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

    std::string extension = path.extension().string();
    for (char& c : extension)
    {
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
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
    auto in_archives = std::make_shared<std::map<std::string, InArchive>>();
    if (fs::is_directory(input, ec))
    {
        // Every file below the folder, and the files in its Bezel Engine archives.
        out.name = input.filename().string();
        std::vector<std::pair<std::string, fs::path>> found;
        for (fs::recursive_directory_iterator it(input, ec), end; !ec && it != end; it.increment(ec))
        {
            // A file whose type can't be read, such as a broken link, is passed over. The walk goes on.
            std::error_code type_error;
            if (it->is_regular_file(type_error))
            {
                found.emplace_back(it->path().lexically_relative(input).generic_string(), it->path());
            }
        }
        if (ec)
        {
            return "can't read the folder: " + ec.message();
        }

        std::sort(found.begin(), found.end());
        const fs::path* boot = nullptr;
        std::ptrdiff_t boot_depth = 0;
        for (const auto& [name, path] : found)
        {
            out.files.push_back(name);
            std::vector<uint8_t> head;
            ReadFile(path, head, 0x30);
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
                if (ReadFile(path, sheet_bytes))
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
        if (boot != nullptr && ReadFile(*boot, text, 1 << 20))
        {
            out.voice_count = VoiceCount(text);
        }

        const fs::path root = input;
        out.read_file = [root, in_archives](const std::string& path, std::vector<uint8_t>& data)
        {
            const auto found_in = in_archives->find(path);
            if (found_in == in_archives->end())
            {
                std::error_code error;
                const fs::path file = root / fs::path(path);
                return fs::is_regular_file(file, error) && ReadFile(file, data);
            }

            std::vector<uint8_t> archive;
            BeaArchive bea;
            return ReadFile(found_in->second.archive, archive) && bea.Read(archive, found_in->second.asset, data);
        };

        return std::nullopt;
    }

    // One Bezel Engine archive.
    std::vector<uint8_t> head;
    if (!ReadFile(input, head, 0x30))
    {
        return std::string("can't be read");
    }
    if (!BeaArchive::Is(head))
    {
        return std::string("isn't a folder, a Bezel Engine archive (.bea) or a sound archive");
    }

    out.name = input.stem().string();
    const std::string prefix = input.filename().string() + "/";
    AddArchive(input, prefix, out, *in_archives);
    out.read_file = [in_archives](const std::string& path, std::vector<uint8_t>& data)
    {
        const auto found_in = in_archives->find(path);
        std::vector<uint8_t> archive;
        BeaArchive bea;
        return found_in != in_archives->end() && ReadFile(found_in->second.archive, archive) &&
               bea.Read(archive, found_in->second.asset, data);
    };

    return std::nullopt;
}

} // namespace hsf::game
