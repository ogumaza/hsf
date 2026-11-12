// SPDX-License-Identifier: MIT

#include "rip/procedures.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

#include "ntdlib/nx/bex/user_procedures.h"
#include "xsflib/sha1.h"

namespace hsf::rip
{
namespace
{

// A game's project archive, by its size and SHA-1, and the handler of the game's user procedures.
struct ProjectArchive
{
    std::size_t size;
    std::string_view sha1;
    uint32_t handler;
};

constexpr std::array<ProjectArchive, 2> kProjects = {{
    {64338704, "a4ede9f67dab3a17300a7a22d97b58f95552bfde", UserProcedureHandler::kJamboree},
    {67534664, "386235195e7b5354173822a0a5419b4cacb1cffe", UserProcedureHandler::kSuperstars},
}};

constexpr std::string_view kProjectPath = "_Resident/AddonAudioProject.fspj";

} // namespace

std::optional<UserProcedureHandler> GameProcedures(const std::vector<std::string>& files, const ReadGameFile& read)
{
    std::optional<UserProcedureHandler> found;
    for (const std::string& path : files)
    {
        std::vector<uint8_t> data;
        if (!NamesFile(path, kProjectPath) || !read(path, data))
        {
            continue;
        }

        const std::string sha1 = xsflib::Sha1::Hex(xsflib::Sha1::Of(data));
        const auto same = [&data, &sha1](const ProjectArchive& project)
        {
            return project.size == data.size() && project.sha1 == sha1;
        };
        if (const auto project = std::find_if(kProjects.begin(), kProjects.end(), same); project != kProjects.end())
        {
            found.emplace();
            found->handler = project->handler;
            break;
        }
    }

    if (!found || found->handler != UserProcedureHandler::kJamboree)
    {
        return found;
    }

    for (uint32_t number = 0; number < UserProcedureTable::kTables; ++number)
    {
        const std::string name =
            std::string("audio/userproc/conv/") + (number < 10 ? "0" : "") + std::to_string(number) + ".msgpack";
        const auto names_table = [&name](const std::string& path)
        {
            return NamesFile(path, name);
        };
        const auto table = std::find_if(files.begin(), files.end(), names_table);
        std::vector<uint8_t> data;
        if (table == files.end() || !read(*table, data))
        {
            continue;
        }

        UserProcedureTable read_table;
        read_table.number = number;
        read_table.values = ntdlib::nx::bex::ReadProcedureTable(data);
        found->tables.push_back(read_table);
    }

    return found;
}

std::string HandlerName(uint32_t handler)
{
    switch (handler)
    {
    case UserProcedureHandler::kJamboree:
        return "Super Mario Party Jamboree's";
    case UserProcedureHandler::kSuperstars:
        return "Mario Party Superstars'";
    default:
        return "none";
    }
}

} // namespace hsf::rip
