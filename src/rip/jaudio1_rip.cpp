// SPDX-License-Identifier: MIT

// hsfrip's parts for Pikmin 2 (see jaudio1_rip.h).

#include "rip/jaudio1_rip.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/jaudio/aaf.h"
#include "xsflib/ascii.h"
#include "xsflib/binary.h"
#include "xsflib/rip/length.h"
#include "xsflib/sha1.h"

#include "hsf/format.h"
#include "hsf/jaudio1.h"
#include "jaudio/output.h"
#include "jaudio1/channel.h"
#include "jaudio1/engine.h"

namespace hsf::rip
{
namespace
{

namespace nj = ntdlib::jaudio;

// The SHA-1 of the start-up archive of Pikmin 2's Switch release.
constexpr std::string_view kStartupSha1 = "963b4c8093c2e507f74296b7d2df6aed0665d88b";

// The game's list of its music and its track map files, by their paths in the folder user. The track maps are in the
// order of Jaudio1GameText::track_maps.
constexpr std::string_view kMusicList = "Totaka/BgmList.txt";
constexpr std::array<std::string_view, 4> kTrackMapFiles = {"Totaka/trackMap_Cond_T.txt", "Totaka/trackMap_Seq_T.txt",
                                                            "Wakai/trackMap_Cond_W.txt", "Wakai/trackMap_Seq_W.txt"};
constexpr std::size_t kCondT = 0;
constexpr std::size_t kSeqT = 1;
constexpr std::size_t kCondW = 2;
constexpr std::size_t kSeqW = 3;

// The game reads at most this many maps of a track map file (BgmTrackMapFile::readTrackMap).
constexpr std::size_t kMostTrackMaps = 32;

// The pieces' classes (jaudio1::PieceSettings::Kind).
constexpr uint8_t kPlain = 0;
constexpr uint8_t kDirected = 1;
constexpr uint8_t kJump = 2;
constexpr uint8_t kConducted = JaudioPiece::kConducted;

// The levels that a piece's child tracks keep: all of them with the sound information of the scenes' main music (flag
// 0x1f00), none with that of the bosses' and the additional pieces' music (flag 0).
constexpr uint8_t kAllLevels = 0x1f;
constexpr uint8_t kNoLevels = 0;

// What the game's code sets up for a piece. `muted` holds the tracks that the piece's directors mute, or nothing for
// a piece whose directors take the track maps. A piece of class 3 plays with `conductor`.
struct PieceCode
{
    std::string_view file;
    uint8_t kind = kPlain;
    uint8_t kept_levels = kAllLevels;
    uint16_t beat_interval = 60;
    uint16_t start_point = 0;
    std::optional<uint16_t> muted = uint16_t{0};
    std::string_view conductor;
};

// The pieces of the game's music whose settings don't come from a conductor's name, by their files' names. A piece
// that the table leaves out plays once with each conductor whose name gives it (ConductedPiece).
//
// PikSceneMgr::initMainBgm makes the scenes' main music. The overworld's pieces are JumpBgmSeq objects
// (SceneMgr::newMainBgm) with directors that take the track maps. yakushima and last get a beat interval of 30. The
// middle bosses start m_boss at their appearance, at part 9 (EnemyMidBoss::BossBgm_Appear), and the final boss starts
// l_boss at part 1. Their directors (DirectorMgr_Battle) take tracks 10 and 11, and 14 and 15. The two players' battle
// plays battle_t as a DirectedBgm whose directors (DirectorMgr_2PBattle) take tracks 8 to 15. hiscore, kuro_post and
// n_tutorial_1stday take the sound information of the additional pieces. The others are BgmSeq objects with the main
// music's sound information.
//
// The game never plays 2pbattle, a_zawinul01, c_result, the six ff_ pieces, id_test, result and u_result. 2pbattle
// makes the requests of battle_t, and gets its object. a_zawinul01 makes the requests of the cave music. It has no
// conductor of its own, and plays with the conductor of a_iron. id_test makes the requests of a DirectedBgm, and gets
// one without directors. The others play as BgmSeq objects.
constexpr PieceCode Plain(std::string_view file, uint8_t kept_levels = kAllLevels)
{
    PieceCode piece;
    piece.file = file;
    piece.kept_levels = kept_levels;

    return piece;
}

constexpr PieceCode Overworld(std::string_view file, uint16_t beat_interval)
{
    PieceCode piece;
    piece.file = file;
    piece.kind = kJump;
    piece.beat_interval = beat_interval;
    piece.muted = std::nullopt;

    return piece;
}

constexpr PieceCode Boss(std::string_view file, uint16_t start_point, uint16_t muted)
{
    PieceCode piece;
    piece.file = file;
    piece.kind = kJump;
    piece.kept_levels = kNoLevels;
    piece.start_point = start_point;
    piece.muted = muted;

    return piece;
}

constexpr PieceCode Battle(std::string_view file)
{
    PieceCode piece;
    piece.file = file;
    piece.kind = kDirected;
    piece.muted = 0xff00;

    return piece;
}

constexpr PieceCode Undirected(std::string_view file)
{
    PieceCode piece;
    piece.file = file;
    piece.kind = kDirected;

    return piece;
}

constexpr PieceCode Borrowing(std::string_view file, std::string_view conductor)
{
    PieceCode piece;
    piece.file = file;
    piece.kind = kConducted;
    piece.muted = std::nullopt;
    piece.conductor = conductor;

    return piece;
}

constexpr std::array<PieceCode, 29> kPieces = {
    Battle("2pbattle.bms"),
    Borrowing("a_zawinul01.bms", "iron.cnd"),
    Battle("battle_t.bms"),
    Plain("book.bms"),
    Plain("comp_result.bms"),
    Plain("c_menu.bms"),
    Plain("c_result.bms"),
    Plain("ff_annihi.bms"),
    Plain("ff_doped.bms"),
    Plain("ff_doping.bms"),
    Plain("ff_keyget.bms"),
    Plain("ff_playerdown.bms"),
    Plain("ff_treasureget.bms"),
    Overworld("forest.bms", 60),
    Plain("f_result.bms"),
    Plain("hiscore.bms", kNoLevels),
    Undirected("id_test.bms"),
    Plain("kuro_post.bms", kNoLevels),
    Plain("kuro_pre.bms"),
    Overworld("last.bms", 30),
    Boss("l_boss.bms", 1, 0xc000),
    Boss("m_boss.bms", 9, 0x0c00),
    Overworld("n_tutorial.bms", 60),
    Plain("n_tutorial_1stday.bms", kNoLevels),
    Plain("result.bms"),
    Plain("u_result.bms"),
    Plain("worldmap.bms"),
    Plain("worldmap_intro.bms"),
    Overworld("yakushima.bms", 30),
};

bool StartsWith(std::string_view text, std::string_view start)
{
    return text.substr(0, start.size()) == start;
}

std::string WithoutExtension(std::string_view name)
{
    return std::string(name.substr(0, name.rfind('.')));
}

// The piece that a conductor plays (ConductorList::getSeqAndWaveFromConductor): the families' pieces by the start of
// the conductor's name, and new_NN for new_NN_M. The game names the piece of cavekeyget.cnd itself. caveboss.cnd and
// iron.cnd, which the game never plays, play the pieces of their names: caveboss and a_iron.
std::optional<std::string> ConductedPiece(std::string_view conductor)
{
    constexpr std::array<std::string_view, 7> kFamilies = {"cavesoil",  "cavemetal",   "caveconc", "cavetile",
                                                           "caveglass", "cavetsumiki", "caverelax"};
    for (const std::string_view family : kFamilies)
    {
        if (StartsWith(conductor, family))
        {
            return std::string(family) + ".bms";
        }
    }

    if (StartsWith(conductor, "new") && conductor.size() >= 6)
    {
        return std::string(conductor.substr(0, 6)) + ".bms";
    }

    if (conductor == "cavekeyget.cnd" || conductor == "caveboss.cnd")
    {
        return WithoutExtension(conductor) + ".bms";
    }

    if (conductor == "iron.cnd")
    {
        return std::string("a_iron.bms");
    }

    return std::nullopt;
}

// The track map of a piece that its directors take (DirectorMgr_Scene::initTrackMap): the file, and the name of the
// map in it. caveglass and caverelax have maps by their names in trackMap_Seq_T.txt. caveconc's and new_'s conductors
// have maps by their names in trackMap_Cond_T.txt, and cavesoil's and cavemetal's in trackMap_Cond_W.txt. Other
// pieces have maps by their names in trackMap_Seq_W.txt.
std::pair<std::size_t, std::string> TrackMapOf(std::string_view piece, std::string_view conductor)
{
    if (piece == "caveglass.bms" || piece == "caverelax.bms")
    {
        return {kSeqT, std::string(piece)};
    }

    if (piece == "caveconc.bms" || StartsWith(piece, "new_"))
    {
        return {kCondT, std::string(conductor)};
    }

    if (piece == "cavesoil.bms" || piece == "cavemetal.bms")
    {
        return {kCondW, std::string(conductor)};
    }

    return {kSeqW, std::string(piece)};
}

// The tokens of a text file of the game as its stream's text mode reads them: the words between white space and
// braces, without the comments. A comment starts with # and runs to the end of its line.
std::vector<std::string> TextTokens(std::span<const uint8_t> text)
{
    std::vector<std::string> tokens;
    std::string token;
    bool comment = false;
    const auto end_token = [&tokens, &token]
    {
        if (!token.empty())
        {
            tokens.push_back(std::move(token));
            token.clear();
        }
    };

    for (const uint8_t byte : text)
    {
        const char c = static_cast<char>(byte);
        if (comment)
        {
            comment = c != '\n' && c != '\r';
            continue;
        }

        if (c == '#')
        {
            end_token();
            comment = true;
        }
        else if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '{' || c == '}' || byte == 0)
        {
            end_token();
        }
        else
        {
            token.push_back(c);
        }
    }

    end_token();

    return tokens;
}

// A number as the game's stream reads it: the token's leading digits, or 0, in a byte.
uint8_t LeadingNumber(std::string_view token)
{
    uint32_t value = 0;
    for (const char c : token)
    {
        if (c < '0' || c > '9')
        {
            break;
        }

        value = (value * 10 + static_cast<uint32_t>(c - '0')) & 0xffff;
    }

    return static_cast<uint8_t>(value);
}

// The key under which the name file names a sound: an effect's ID without bits 10 and 11, which say how long the game
// plays it.
uint32_t NameKey(uint32_t id)
{
    return (id & nj::kAafTypeMask) == nj::kAafEffect ? id & ~(nj::kAafToEnd | nj::kAafToEnd >> 1) : id;
}

// A path in a folder, or the name alone for no folder.
std::string InFolder(const std::string& folder, std::string_view name)
{
    return folder.empty() ? std::string(name) : folder + "/" + std::string(name);
}

bool EndsWithLower(std::string_view text, std::string_view end)
{
    return text.size() >= end.size() && xsflib::AsciiLower(std::string(text.substr(text.size() - end.size()))) == end;
}

// The model of JAudio1 for the length analysis, as JAudio2's is (rip.cpp's JaudioLengthModel), with JAudio2's rules.
// The FX lines ring while their delays hold a sample louder than 8 (jaudio1::Engine::FxRinging). Without `loops`, no
// loop counts: a piece that a conductor plays never repeats.
class Jaudio1LengthModel
{
public:
    Jaudio1LengthModel(jaudio1::Engine& engine, bool loops) : engine_(engine), loops_(loops)
    {
    }

    bool RunFrame()
    {
        engine_.RunSubframe();
        output_.clear();
        out_.Push(engine_.Output(), output_);

        return true;
    }

    bool Sounded() const
    {
        return xsflib::rip::Audible(output_);
    }

    bool Ringing() const
    {
        return std::any_of(output_.begin(), output_.end(), [](int16_t sample) { return sample != 0; }) ||
               engine_.FxRinging();
    }

    std::size_t VoiceCount()
    {
        return jaudio1::Voices::kDspChannels;
    }

    xsflib::rip::VoicePlace Voice(std::size_t index)
    {
        const jaudio1::Engine::WavePlace place = engine_.DspChannelPlace(static_cast<uint32_t>(index));
        return {place.playing, place.loops, place.position};
    }

    bool Active() const
    {
        return engine_.Playing();
    }

    bool Settled() const
    {
        return engine_.Held() && engine_.Waiting();
    }

    bool Stuck() const
    {
        return false;
    }

    bool Moving() const
    {
        return engine_.Moving();
    }

    uint64_t ChangedAt() const
    {
        return 0;
    }

    bool Busy() const
    {
        return engine_.Busy();
    }

    bool ChannelsChanging() const
    {
        return !engine_.Held();
    }

    int ActiveChannels() const
    {
        return engine_.ActiveChannels();
    }

    template <typename Hook>
    void OnLoop(Hook hook)
    {
        if (!loops_)
        {
            return;
        }

        engine_.SetLoopHook(
            [this, hook](uint32_t track, uint32_t target, bool played)
            {
                if (track == MainTrack() && (played || engine_.Held()))
                {
                    hook(static_cast<int>(track), target);
                }
            });
    }

private:
    // The main track, as JAudio2's model picks it. A track that waits for ever doesn't count.
    uint32_t MainTrack() const
    {
        uint32_t first = jaudio1::Track::kNone;
        for (const uint32_t track : engine_.TreeOrder())
        {
            if (!engine_.PlayedNotes(track) || engine_.WaitsForEver(track))
            {
                continue;
            }

            if (!engine_.QuietPass(track))
            {
                return track;
            }

            if (first == jaudio1::Track::kNone)
            {
                first = track;
            }
        }

        return first;
    }

    jaudio1::Engine& engine_;
    bool loops_;
    jaudio::AudioOut out_;
    std::vector<int16_t> output_;
};

} // namespace

JaudioSettings Jaudio1Settings()
{
    JaudioSettings settings;
    settings.library = JaudioSettings::kPikmin2Library;
    settings.dsp_level = jaudio1::Settings{}.dsp_level;
    settings.music_volume = 1.0f;
    settings.effect_volume = 1.0f;

    return settings;
}

std::optional<Jaudio1Paths> FindJaudio1Files(const std::vector<std::string>& files, const ReadGameFile& read)
{
    for (const std::string& path : files)
    {
        const std::size_t slash = path.rfind('/');
        const std::string folder = slash == std::string::npos ? std::string() : path.substr(0, slash);
        const std::size_t parent = folder.rfind('/');
        const std::string folder_name = parent == std::string::npos ? folder : folder.substr(parent + 1);
        if (xsflib::AsciiLower(folder_name) != "audiores" || !EndsWithLower(path, ".aaf"))
        {
            continue;
        }

        std::vector<uint8_t> data;
        if (!read(path, data) || xsflib::Sha1::Hex(xsflib::Sha1::Of(data)) != kStartupSha1)
        {
            continue;
        }

        Jaudio1Paths paths;
        paths.startup = path;
        paths.sequences = InFolder(folder, "Seqs/Seq.arc");
        paths.waves = InFolder(folder, "Banks");
        paths.conductors = InFolder(folder, "Conductor.arc");
        paths.names = path.substr(0, path.size() - 4) + ".asn";
        paths.user = InFolder(parent == std::string::npos ? std::string() : folder.substr(0, parent), "user");

        return paths;
    }

    return std::nullopt;
}

std::map<std::string, uint8_t> ReadMusicVolumes(std::span<const uint8_t> text)
{
    std::map<std::string, uint8_t> volumes;
    const std::vector<std::string> tokens = TextTokens(text);
    for (std::size_t i = 0; i + 1 < tokens.size() && tokens[i] != "endoffile"; i += 2)
    {
        volumes.try_emplace(tokens[i], LeadingNumber(tokens[i + 1]));
    }

    return volumes;
}

std::map<std::string, TrackCounts> ReadTrackMaps(std::span<const uint8_t> text)
{
    // A map takes 31 tokens: its name, the 6 counts, and 24 flags.
    constexpr std::size_t kTokens = 31;
    std::map<std::string, TrackCounts> maps;
    const std::vector<std::string> tokens = TextTokens(text);
    for (std::size_t i = 0, read = 0; read < kMostTrackMaps && i < tokens.size() && tokens[i] != "endoffile";
         i += kTokens, ++read)
    {
        if (tokens.size() - i < kTokens)
        {
            throw xsflib::FormatError("a track map ends early");
        }

        TrackCounts counts{};
        for (std::size_t k = 0; k < counts.size(); ++k)
        {
            counts[k] = LeadingNumber(tokens[i + 1 + k]);
            if (counts[k] >= 16)
            {
                throw xsflib::FormatError("a track map's count is past 15");
            }
        }

        for (std::size_t k = 1 + counts.size(); k < kTokens; ++k)
        {
            if (LeadingNumber(tokens[i + k]) > 1)
            {
                throw xsflib::FormatError("a track map's flag is past 1");
            }
        }

        maps.try_emplace(tokens[i], counts);
    }

    return maps;
}

uint16_t MutedTracks(const TrackCounts& counts)
{
    uint32_t layers = 0;
    for (std::size_t i = 1; i < counts.size(); ++i)
    {
        layers += counts[i];
    }

    const uint32_t mask = layers >= 32 ? ~0u : (1u << layers) - 1;
    return static_cast<uint16_t>((counts[0] >= 32 ? 0 : mask << counts[0]) & 0xffff);
}

std::optional<std::string> ReadJaudio1GameText(const Jaudio1Paths& paths, const ReadGameFile& read,
                                               Jaudio1GameText& text)
{
    std::vector<uint8_t> data;
    const std::string list = InFolder(paths.user, kMusicList);
    if (!read(list, data))
    {
        return "can't read " + list;
    }

    text.volumes = ReadMusicVolumes(data);

    for (std::size_t i = 0; i < kTrackMapFiles.size(); ++i)
    {
        const std::string path = InFolder(paths.user, kTrackMapFiles[i]);
        if (!read(path, data))
        {
            return "can't read " + path;
        }

        try
        {
            text.track_maps[i] = ReadTrackMaps(data);
        }
        catch (const xsflib::FormatError& e)
        {
            return path + ": " + e.what();
        }
    }

    // The name file's sounds have little-endian IDs. The first name of a key counts.
    if (read(paths.names, data))
    {
        try
        {
            for (const nj::AsnSound& sound : nj::ReadAsnSounds(data, false))
            {
                text.names.try_emplace(NameKey(sound.id), sound.name);
            }
        }
        catch (const xsflib::FormatError& e)
        {
            return paths.names + ": " + e.what();
        }
    }

    return std::nullopt;
}

Jaudio1Selection SelectJaudio1Sounds(const Jaudio1Files& files, const Jaudio1GameText& text, const std::regex& only,
                                     bool bgm)
{
    Jaudio1Selection selection;
    JaudioLibrary2& library = selection.library;

    // The effect sequence plays at its volume in the game's list too (SeqBase::init).
    for (const jaudio1::SoundFiles::SequenceFile& file : files.sound.Sequences())
    {
        if (file.name == "se.bms")
        {
            library.effect_sequence = file.id;
            const auto volume = text.volumes.find(file.name);
            library.effect_sequence_volume = volume != text.volumes.end() ? volume->second : uint8_t{127};
        }
    }

    const auto add_piece = [&](const std::string& label, JaudioPiece piece)
    {
        const auto place = static_cast<uint32_t>(library.pieces.size());
        library.pieces.push_back(std::move(piece));
        JaudioJob job;
        job.index = static_cast<uint32_t>(selection.jobs.size());
        job.label = label;
        job.id = JaudioLibrary2::kPieceSection | place;
        selection.jobs.push_back(std::move(job));
    };

    const auto muted_from_maps = [&](const std::string& piece, const std::string& conductor) -> uint16_t
    {
        const auto [file, name] = TrackMapOf(piece, conductor);
        const auto map = text.track_maps[file].find(name);
        if (map == text.track_maps[file].end())
        {
            selection.warnings.push_back(name + " has no track map; its rip plays every track");
            return 0;
        }

        return MutedTracks(map->second);
    };

    const auto volume_of = [&](const std::string& piece) -> uint8_t
    {
        const auto volume = text.volumes.find(piece);
        if (volume == text.volumes.end())
        {
            selection.warnings.push_back(piece + " isn't in the game's list of its music; its rip plays at 50");
            return 50;
        }

        return std::min<uint8_t>(volume->second, 127);
    };

    for (const jaudio1::SoundFiles::SequenceFile& file : files.sound.Sequences())
    {
        const auto code = std::find_if(kPieces.begin(), kPieces.end(),
                                       [&file](const PieceCode& piece) { return piece.file == file.name; });
        if (code != kPieces.end())
        {
            if (!std::regex_search(WithoutExtension(file.name), only))
            {
                continue;
            }

            JaudioPiece piece;
            piece.file = file.id;
            piece.kind = code->kind;
            piece.volume = volume_of(file.name);
            piece.kept_levels = code->kept_levels;
            piece.beat_interval = code->beat_interval;
            piece.start_point = code->start_point;
            piece.conductor = code->conductor;
            piece.muted_tracks = code->muted ? *code->muted : muted_from_maps(file.name, piece.conductor);
            if (piece.kind == kConducted && !files.conductors.contains(piece.conductor))
            {
                selection.warnings.push_back(file.name + "'s conductor " + piece.conductor +
                                             " isn't in the conductor archive; the piece is left out");
                continue;
            }

            add_piece(WithoutExtension(file.name), piece);
            continue;
        }

        // A piece of the cave music: one rip for each conductor whose name gives the piece, named after the
        // conductor. A file that no conductor plays, such as the effect sequence, plays no music.
        for (const auto& [conductor, data] : files.conductors)
        {
            if (ConductedPiece(conductor) != file.name || !std::regex_search(WithoutExtension(conductor), only))
            {
                continue;
            }

            JaudioPiece piece;
            piece.file = file.id;
            piece.kind = kConducted;
            piece.volume = volume_of(file.name);
            piece.kept_levels = kAllLevels;
            piece.muted_tracks = muted_from_maps(file.name, conductor);
            piece.conductor = conductor;
            add_piece(WithoutExtension(conductor), piece);
        }
    }

    if (bgm)
    {
        return selection;
    }

    // The effects, by category and index. The game keeps an effect until it ends (bit 11). An effect plays on a slot
    // track of its category in the first effect scene. A category without one plays no effect.
    const std::vector<std::vector<uint8_t>>& scenes = files.sound.EffectScenes();
    for (uint32_t category = 0; category < jaudio1::SoundFiles::kEffectCategories; ++category)
    {
        const bool slot = !scenes.empty() && category < scenes[0].size() && scenes[0][category] != 0;
        if (!slot && files.sound.SoundCount(category) != 0)
        {
            selection.warnings.push_back("the effects of category " + std::to_string(category) +
                                         " have no slot track in the first effect scene; they're left out");
            continue;
        }

        for (uint32_t index = 0; index < files.sound.SoundCount(category); ++index)
        {
            const uint32_t id = category << 12 | index | nj::kAafToEnd;
            const auto name = text.names.find(NameKey(id));
            char label[32];
            std::snprintf(label, sizeof(label), "se%u-%04u", static_cast<unsigned>(category),
                          static_cast<unsigned>(index));
            const std::string text_label = name != text.names.end() && !name->second.empty() ? name->second : label;
            if (!std::regex_search(text_label, only))
            {
                continue;
            }

            JaudioJob job;
            job.index = static_cast<uint32_t>(selection.jobs.size());
            job.label = text_label;
            job.id = id;
            selection.jobs.push_back(std::move(job));
        }
    }

    return selection;
}

Jaudio1LengthAnalyzer::Jaudio1LengthAnalyzer(const Jaudio1Files& files, ArchiveDescriptor settings)
    : files_(files), settings_(std::move(settings))
{
}

JaudioTiming Jaudio1LengthAnalyzer::Analyze(uint32_t sound, std::optional<uint32_t> draws) const
{
    JaudioTiming result;
    const JaudioLibrary2& library = settings_.jaudio.library2;
    const auto measure = [this, sound, draws, &library, &result]() -> std::optional<Timing>
    {
        jaudio1::Engine engine(files_.sound, Jaudio1EngineSettings(settings_.jaudio));
        if (StartJaudio1Sound(engine, files_, library, sound, draws))
        {
            return std::nullopt;
        }

        const bool conducted = (sound & 0xc0000000u) == JaudioLibrary2::kPieceSection &&
                               library.pieces[sound & 0xffff].kind == JaudioPiece::kConducted;
        Jaudio1LengthModel model(engine, !conducted);
        const std::optional<Timing> timing = xsflib::rip::MeasureLength(model, JaudioLengthRules());
        result.missing_bank_notes = engine.MissingBankNotes();
        result.sequence_draws = engine.SequenceDraws();

        return timing;
    };
    result.timing = xsflib::rip::TryMeasure(measure);

    return result;
}

} // namespace hsf::rip
