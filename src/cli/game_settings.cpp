// SPDX-License-Identifier: MIT

// The settings that hsfrip gives a game's sound archives, and its list of a game (see game_settings.h).

#include "cli/game_settings.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/rip/archive_mode.h"
#include "ntdlib/sar/formats.h"

#include "game/game.h"
#include "hsf/format.h"
#include "hsf/sar.h"
#include "rip/procedures.h"

namespace hsf::cli
{

namespace
{

// What another console's sound archive is, from the file's first bytes `head`: "a 3DS sound archive", "a Wii sound
// archive" or "a Wii U sound archive (big-endian FSAR)". Nothing for a Switch sound archive or any other file, a DS
// sound archive included. The readers read a Wii U archive. Its sequences have big-endian arguments, though, and
// nn::atk reads them little-endian.
std::optional<std::string> ForeignArchive(std::span<const uint8_t> head)
{
    const std::string_view text(reinterpret_cast<const char*>(head.data()), head.size());
    switch (ntdlib::sar::SoundArchiveKindOf(text))
    {
    case ntdlib::sar::SoundArchiveKind::kCsar:
        return "a 3DS sound archive";
    case ntdlib::sar::SoundArchiveKind::kRsar:
        return "a Wii sound archive";
    case ntdlib::sar::SoundArchiveKind::kFsarBig:
        return "a Wii U sound archive (big-endian FSAR)";
    default:
        return std::nullopt;
    }
}

} // namespace

bool RefuseForeignArchive(const std::string& path, std::span<const uint8_t> head)
{
    const std::optional<std::string> foreign = ForeignArchive(head);
    if (foreign)
    {
        std::fprintf(stderr, "error: %s is %s; HSF rips the Switch's sound archives (little-endian FSAR, .bfsar)\n",
                     path.c_str(), foreign->c_str());
    }

    return foreign.has_value();
}

bool RefuseMissingArchive(const std::vector<std::string>& archives, const Options& o)
{
    const bool missing =
        !o.only_archive.empty() && std::find(archives.begin(), archives.end(), o.only_archive) == archives.end();
    if (missing)
    {
        std::fprintf(stderr, "error: the game has no sound archive %s (--list shows them)\n", o.only_archive.c_str());
    }

    return missing;
}

std::string DescribeArchive(std::vector<uint8_t> bytes)
{
    if (const std::optional<std::string> foreign = ForeignArchive(bytes))
    {
        return *foreign + ". HSF doesn't rip those";
    }

    return ntdlib::rip::DescribeArchive(std::move(bytes), hsf::kSarOptions);
}

hsf::ArchiveDescriptor OptionSettings(const Options& o)
{
    hsf::ArchiveDescriptor settings;
    if (o.voices > 0)
    {
        settings.voice_count = static_cast<uint32_t>(o.voices);
    }

    return settings;
}

bool VoicesFromBoot(const hsf::game::GameFiles& game, const Options& o)
{
    return o.voices == 0 && hsf::game::BootVoices(game).has_value();
}

std::vector<hsf::game::BusEffect> GameEffects(const hsf::game::GameFiles& game, const Options& o)
{
    return o.effects ? hsf::game::StartupEffects(game) : std::vector<hsf::game::BusEffect>{};
}

std::string EffectsText(const std::vector<hsf::game::BusEffect>& effects, const std::string& none)
{
    std::string text;
    for (const hsf::game::BusEffect& e : effects)
    {
        text += (text.empty() ? "" : ", ") + e.name + " on aux bus " + static_cast<char>('A' + e.bus);
    }

    return text.empty() ? none : text;
}

hsf::ArchiveDescriptor GameSettings(const hsf::game::GameFiles& game, const Options& o)
{
    hsf::ArchiveDescriptor settings = OptionSettings(o);
    if (VoicesFromBoot(game, o))
    {
        settings.voice_count = *hsf::game::BootVoices(game);
    }

    settings.effects = hsf::game::AuxEffects(GameEffects(game, o));

    if (std::optional<hsf::UserProcedureHandler> procedures = hsf::rip::GameProcedures(game.files, game.read_file))
    {
        settings.flags |= hsf::ArchiveDescriptor::kFlagProcedures;
        settings.procedures = std::move(*procedures);
    }

    return settings;
}

std::string ProceduresText(const hsf::ArchiveDescriptor& settings)
{
    if ((settings.flags & hsf::ArchiveDescriptor::kFlagProcedures) == 0)
    {
        return std::string();
    }

    return "; user procedures: " + hsf::rip::HandlerName(settings.procedures.handler);
}

void ListGame(const std::string& input, const hsf::game::GameFiles& game, const hsf::ArchiveDescriptor& settings,
              const Options& o)
{
    const std::vector<std::string>& archives = game.sound_archives;
    std::printf("%s: %zu files, %zu sound archive%s\n", input.c_str(), game.files.size(), archives.size(),
                archives.size() == 1 ? "" : "s");
    ntdlib::rip::ListArchives(archives, game.read_file, DescribeArchive);
    std::printf("voices: %u%s; effects: %s%s\n", settings.voice_count, VoicesFromBoot(game, o) ? " (boot.lua)" : "",
                EffectsText(GameEffects(game, o), "none").c_str(), ProceduresText(settings).c_str());
    if (game.sound_levels.Count() > 0)
    {
        std::printf("the game's sound sheets give levels for %zu sounds\n", game.sound_levels.Count());
    }
}

} // namespace hsf::cli
