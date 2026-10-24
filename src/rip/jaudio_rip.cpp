// SPDX-License-Identifier: MIT

// hsfrip's parts for games that play their sound with JAudio (see jaudio_rip.h).

#include "rip/jaudio_rip.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/jaudio/archive.h"
#include "ntdlib/jaudio/rarc.h"
#include "xsflib/ascii.h"
#include "xsflib/binary.h"
#include "xsflib/sha1.h"
#include "xsflib/text.h"

#include "hsf/format.h"

namespace hsf::rip
{
namespace
{

namespace nj = ntdlib::jaudio;

// The sections of the sound table: sound effects, and the music that the game's sequence manager plays.
constexpr uint8_t kMusicSection = 1;

// The track states as the game's table holds them, two to a byte, the even track in the high nibble.
JaudioTrackStates States(uint32_t sound, const std::array<uint8_t, 8>& packed)
{
    JaudioTrackStates states;
    states.sound = sound;
    for (std::size_t i = 0; i < packed.size(); i++)
    {
        states.states[2 * i] = static_cast<uint8_t>(packed[i] >> 4);
        states.states[2 * i + 1] = static_cast<uint8_t>(packed[i] & 15);
    }

    return states;
}

// The track states of each piece of music in `states`, by the piece's index in group 0 of section 1.
void AddTrackStates(JaudioSettings& s, std::span<const std::pair<uint32_t, std::array<uint8_t, 8>>> states)
{
    for (const auto& [index, packed] : states)
    {
        s.track_states.push_back(States(uint32_t{kMusicSection} << 24 | index, packed));
    }
}

// Super Mario Galaxy's code in the Switch release's `main`.
JaudioGame Galaxy()
{
    JaudioGame game;
    game.name = "Super Mario Galaxy";
    game.startup_sha1 = "b78389973b94ad6039745dbb3540840b5330251a";

    JaudioSettings& s = game.settings;

    // AudParams' output gain, 0.9 (0x6598f0), through JASDriver::setDSPLevel: 4 times it in 4.12.
    s.dsp_level = 0x3999;

    // AudParams' volumes of the sequence manager and the effect manager (0x6598fc and 0x659900), and the categories'
    // volumes of its first volume set (scCtgVolume, 0x659908). AudSystem::initVolumeSetting sets them.
    s.music_volume = 1.2f;
    s.effect_volume = 1.2f;
    s.category_volumes = {1.2f, 1.2f, 1.2f, 1.3f, 1.2f, 1.2f, 1.2f, 1.2f,
                          1.2f, 1.2f, 1.2f, 1.2f, 1.0f, 1.5f, 1.0f, 1.0f};

    // AudEffector::initFxLine's four lines (0x93f710), each with its two buses looked up in JASDsp's table of sends
    // (0x6511b8). Each line delays by 64 sub-frames.
    const std::array<int16_t, 8> filter = {0, 0x1000, 0, 0, 0, 0, 0x3fff, 0};
    s.fx_lines[0] = {1, 64, {0x0d00, 0x0d60}, {0x7000, 0}, filter};
    s.fx_lines[1] = {1, 64, {0x0d00, 0x0d60}, {0, 0x7000}, filter};
    s.fx_lines[2] = {1, 64, {0x0d00, 0x0f40}, {0x7000, 0x7000}, filter};
    s.fx_lines[3] = {1, 64, {0x0ca0, 0x0d60}, {0x7000, 0x7000}, filter};

    // AudBgmSetting's first state of each piece of music that has a setting (cBgmSettingInfo at 0x659bc4, cMuteState
    // at 0x659f0c). AudSingleBgm::start gives a piece that state as it starts. Each name says which tracks play. Each
    // state mutes track 0 and fades out the tracks that don't play. kNoTracksMuted1 also mutes track 1 while its
    // volume is 0. The pieces that play no track play beside streams, and the game fades their tracks in later.
    constexpr std::array<uint8_t, 8> kNoTracks = {0x20, 0, 0, 0, 0, 0, 0, 0};
    constexpr std::array<uint8_t, 8> kTracks1To13 = {0x21, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x00};
    constexpr std::array<uint8_t, 8> kTracks1To10 = {0x21, 0x11, 0x11, 0x11, 0x11, 0x10, 0x00, 0x00};
    constexpr std::array<uint8_t, 8> kNoTracksMuted1 = {0x28, 0, 0, 0, 0, 0, 0, 0};
    const std::array<std::pair<uint32_t, std::array<uint8_t, 8>>, 18> kStates = {{
        {5, kNoTracks},
        {8, kTracks1To13},
        {15, kNoTracks},
        {21, kNoTracks},
        {26, kNoTracks},
        {37, kNoTracks},
        {40, kNoTracks},
        {56, kNoTracksMuted1},
        {59, kNoTracks},
        {60, kNoTracks},
        {69, kNoTracks},
        {70, kNoTracks},
        {75, kTracks1To10},
        {79, kNoTracks},
        {83, kNoTracks},
        {84, kNoTracks},
        {96, kNoTracks},
        {99, kTracks1To10},
    }};
    AddTrackStates(s, kStates);

    // The sounds of group 1 of section 1. AudMultiBgm plays each as a piece of music of group 0 and a stream
    // (cMultiBgmSet).
    game.replaced_groups = {1 << 8 | 1};

    return game;
}

// Super Mario Galaxy 2's code in the Switch release's `main`.
JaudioGame Galaxy2()
{
    JaudioGame game;
    game.name = "Super Mario Galaxy 2";
    game.startup_sha1 = "fb262f1705a6253603055f36002d42dea6b51c87";

    JaudioSettings& s = game.settings;

    // AudParams: the output gain 0.9 (0x745a14), the volumes of the sequence manager and the effect manager
    // (0x745a20 and 0x745a24), and the first volume set of the categories (0x746f5c). All match the first game's.
    s.dsp_level = 0x3999;
    s.music_volume = 1.2f;
    s.effect_volume = 1.2f;
    s.category_volumes = {1.2f, 1.2f, 1.2f, 1.3f, 1.2f, 1.2f, 1.2f, 1.2f,
                          1.2f, 1.2f, 1.2f, 1.2f, 1.0f, 1.5f, 1.0f, 1.0f};

    // AudEffector::initFxLine's four lines (0x1290bc), from the first of its two sets (0xa448a0), each with its two
    // buses looked up in JASDsp's table of sends (0x73fa9c). AudEffector::changeFxLineSet (0x129228) gives a stage the
    // second set when the stage is one of a list (0xb80e0).
    const std::array<int16_t, 8> filter = {0x0625, -0x0745, 0x0625, -0x0626, -0x0508, 0x1808, 0x28cd, -0x0f18};
    s.fx_lines[0] = {1, 69, {0x0d00, 0x0d60}, {0x7fff, 0x30a1}, filter};
    s.fx_lines[1] = {1, 69, {0x0d00, 0x0d60}, {0x30a1, 0x7fff}, filter};
    s.fx_lines[2] = {1, 100, {0x0d00, 0x0f40}, {0x7fff, 0x7fff}, filter};
    s.fx_lines[3] = {1, 85, {0x0ca0, 0x0d60}, {0x7fff, 0x7fff}, filter};

    // AudBgmSetting's first state of each piece of music that has a setting. A switch on the sound's ID (0x1252d0)
    // finds the piece's setting, and the mute states are at 0x745d9c, 11 to a setting. AudSingleBgm::start (0x1219e8)
    // gives a piece the first state as it starts. The names say which tracks play. The first game's list names its
    // states the same way.
    constexpr std::array<uint8_t, 8> kNoTracks = {0x20, 0, 0, 0, 0, 0, 0, 0};
    constexpr std::array<uint8_t, 8> kTracks4And7And8 = {0x20, 0x00, 0x10, 0x01, 0x10, 0x00, 0x00, 0x00};
    constexpr std::array<uint8_t, 8> kTracks1To5 = {0x21, 0x11, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00};
    const std::array<std::pair<uint32_t, std::array<uint8_t, 8>>, 41> kStates = {{
        {0x08, {0x21, 0x11, 0x11, 0x11, 0x10, 0x10, 0x00, 0x00}},
        {0x09, {0x21, 0x11, 0x11, 0x11, 0x11, 0x10, 0x00, 0x00}},
        {0x0a, {0x21, 0x11, 0x11, 0x00, 0x11, 0x10, 0x00, 0x11}},
        {0x17, {0x21, 0x11, 0x10, 0x01, 0x10, 0x00, 0x00, 0x08}},
        {0x18, kNoTracks},
        {0x1d, kNoTracks},
        {0x1e, {0x21, 0x11, 0x11, 0x11, 0x11, 0x00, 0x00, 0x08}},
        {0x21, kNoTracks},
        {0x23, kTracks4And7And8},
        {0x25, kNoTracks},
        {0x29, kTracks4And7And8},
        {0x2c, {0x21, 0x11, 0x11, 0x11, 0x11, 0x11, 0x10, 0x00}},
        {0x2d, {0x21, 0x10, 0x00, 0x01, 0x00, 0x11, 0x10, 0x00}},
        {0x31, kNoTracks},
        {0x32, kNoTracks},
        {0x35, kNoTracks},
        {0x39, kTracks1To5},
        {0x3a, kTracks4And7And8},
        {0x3c, kNoTracks},
        {0x45, kNoTracks},
        {0x46, kNoTracks},
        {0x47, kNoTracks},
        {0x53, kNoTracks},
        {0x54, kNoTracks},
        {0x58, kNoTracks},
        {0x5a, kNoTracks},
        {0x5b, kNoTracks},
        {0x5d, kNoTracks},
        {0x60, kNoTracks},
        {0x61, kNoTracks},
        {0x62, kNoTracks},
        {0x65, kNoTracks},
        {0x67, kNoTracks},
        {0x70, kTracks1To5},
        {0x72, kNoTracks},
        {0x73, kNoTracks},
        {0x74, kNoTracks},
        {0x76, kNoTracks},
        {0x77, kNoTracks},
        {0x78, kNoTracks},
        {0x7b, kNoTracks},
    }};
    AddTrackStates(s, kStates);

    // As in the first game, AudBgmMgr::start (0x121140) plays a sound with bit 16 of its ID set beside a stream.
    game.replaced_groups = {1 << 8 | 1};

    // The volumes of the music in a table of the game's files (0x128cf0).
    game.bgm_params = "Info/BgmParam.arc";

    return game;
}

const std::vector<JaudioGame>& Games()
{
    static const std::vector<JaudioGame> kGames = {Galaxy(), Galaxy2()};
    return kGames;
}

// The hash of a JMap table's column names: each byte added to 31 times the hash so far.
uint32_t JmapHash(std::string_view name)
{
    uint32_t hash = 0;
    for (const char c : name)
    {
        hash = hash * 31 + static_cast<uint8_t>(c);
    }

    return hash;
}

bool EndsWith(std::string_view text, std::string_view end)
{
    return text.size() >= end.size() && xsflib::AsciiLower(std::string(text.substr(text.size() - end.size()))) == end;
}

} // namespace

bool IsStartupArchive(std::span<const uint8_t> data)
{
    std::vector<uint8_t> decoded;
    std::span<const uint8_t> bytes = data;
    if (nj::IsYaz0(data))
    {
        try
        {
            bytes = nj::DecodeIfYaz0(data, decoded);
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    const std::string_view head(reinterpret_cast<const char*>(bytes.data()), std::min<std::size_t>(bytes.size(), 4));
    return head == "AA_<" || head == "<_AA";
}

std::optional<JaudioPaths> FindJaudioFiles(const std::vector<std::string>& files, const ReadGameFile& read)
{
    for (const std::string& path : files)
    {
        const std::size_t slash = path.rfind('/');
        const std::string folder = slash == std::string::npos ? std::string() : path.substr(0, slash);
        const std::size_t parent = folder.rfind('/');
        const std::string folder_name = parent == std::string::npos ? folder : folder.substr(parent + 1);
        if (xsflib::AsciiLower(folder_name) != "audiores" || !(EndsWith(path, ".szs") || EndsWith(path, ".baa")))
        {
            continue;
        }

        std::vector<uint8_t> data;
        if (!read(path, data) || !IsStartupArchive(data))
        {
            continue;
        }

        JaudioPaths paths;
        paths.startup = path;
        paths.waves = folder + "/Waves";
        const std::string seqs = xsflib::AsciiLower(folder + "/Seqs/");
        for (const std::string& file : files)
        {
            if (xsflib::AsciiLower(file).starts_with(seqs) && EndsWith(file, ".arc") &&
                file.find('/', seqs.size()) == std::string::npos)
            {
                paths.sequences = file;
                break;
            }
        }

        return paths;
    }

    return std::nullopt;
}

const JaudioGame* FindJaudioGame(std::span<const uint8_t> startup)
{
    const std::string sha1 = xsflib::Sha1::Hex(xsflib::Sha1::Of(startup));
    for (const JaudioGame& game : Games())
    {
        if (game.startup_sha1 == sha1)
        {
            return &game;
        }
    }

    return nullptr;
}

std::vector<JaudioSoundVolume> ReadBgmVolumes(std::span<const uint8_t> table_archive, const nj::SoundArchive& archive)
{
    std::vector<uint8_t> decoded;
    const std::span<const uint8_t> rarc = nj::DecodeIfYaz0(table_archive, decoded);
    const std::vector<nj::ArchiveFile> files = nj::ReadRarc(rarc);
    const auto table = std::find_if(files.begin(), files.end(),
                                    [](const nj::ArchiveFile& file) { return file.name == "BgmParam.bcsv"; });
    if (table == files.end())
    {
        throw xsflib::FormatError("the archive has no table BgmParam.bcsv");
    }

    // The table's header: the counts of rows and columns, where the rows start and each row's size. The columns follow
    // in 12 bytes each: the name's hash, a mask, the value's offset in the row, a shift and the type. The strings
    // follow the rows.
    const xsflib::Reader r(table->data, rarc.size() >= 4 && rarc[0] == 'R');
    const uint32_t rows = r.U32(0);
    const uint32_t columns = r.U32(4);
    const uint32_t first_row = r.U32(8);
    const uint32_t row_size = r.U32(12);
    struct Column
    {
        uint32_t mask = 0;
        uint16_t offset = 0;
        uint8_t shift = 0;
        uint8_t type = 0;
    };
    std::optional<Column> name_column;
    std::optional<Column> volume_column;
    for (uint32_t i = 0; i < columns; i++)
    {
        const std::size_t at = 16 + std::size_t{12} * i;
        const Column column{r.U32(at + 4), r.U16(at + 8), r.U8(at + 10), r.U8(at + 11)};
        if (r.U32(at) == JmapHash("SoundIdName"))
        {
            name_column = column;
        }
        else if (r.U32(at) == JmapHash("Volume"))
        {
            volume_column = column;
        }
    }

    // The game reads the name as a string (type 6) and the volume as an integer (type 0).
    constexpr uint8_t kInteger = 0;
    constexpr uint8_t kString = 6;
    if (!name_column || name_column->type != kString || !volume_column || volume_column->type != kInteger)
    {
        throw xsflib::FormatError("the table BgmParam.bcsv lacks the columns SoundIdName and Volume");
    }

    std::map<std::string, const nj::SoundEntry*> by_name;
    for (const nj::SoundEntry& sound : archive.Sounds())
    {
        if (!sound.name.empty())
        {
            by_name.emplace(sound.name, &sound);
        }
    }

    const uint64_t strings = first_row + uint64_t{rows} * row_size;
    std::vector<JaudioSoundVolume> volumes;
    for (uint32_t i = 0; i < rows; i++)
    {
        const uint64_t row = first_row + uint64_t{i} * row_size;
        const uint64_t name_at = strings + r.U32(static_cast<std::size_t>(row + name_column->offset));
        if (name_at >= r.Size())
        {
            throw xsflib::FormatError("a row of BgmParam.bcsv names a string past the table's end");
        }
        const std::string name = r.CString(static_cast<std::size_t>(name_at), r.Size());
        const uint32_t word = r.U32(static_cast<std::size_t>(row + volume_column->offset));
        const uint32_t volume = (word & volume_column->mask) >> (volume_column->shift & 31);

        const auto sound = by_name.find(name);
        if (sound != by_name.end() && (sound->second->type & 0x40) != 0)
        {
            volumes.push_back({sound->second->Id(), volume & 0xff});
        }
    }

    return volumes;
}

std::string JaudioLabel(const nj::SoundEntry& sound)
{
    return sound.name.empty() ? "sound " + xsflib::Hex(sound.Id(), 8) : sound.name;
}

std::vector<JaudioJob> SelectJaudioSounds(const jaudio::SoundFiles& files, const JaudioGame* game,
                                          const std::regex& only, bool bgm)
{
    std::vector<JaudioJob> jobs;
    const std::vector<nj::SoundEntry>& sounds = files.Archive().Sounds();
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        const nj::SoundEntry& sound = sounds[i];
        const uint32_t group = uint32_t{sound.section} << 8 | sound.group;
        if (game != nullptr &&
            std::find(game->replaced_groups.begin(), game->replaced_groups.end(), group) != game->replaced_groups.end())
        {
            continue;
        }

        if ((bgm && sound.section != kMusicSection) || files.SequenceOf(sound).sequence == nullptr)
        {
            continue;
        }

        const std::string label = JaudioLabel(sound);
        if (!std::regex_search(label, only))
        {
            continue;
        }

        JaudioJob job;
        job.index = static_cast<uint32_t>(i);
        job.label = label;
        job.id = sound.Id();
        jobs.push_back(std::move(job));
    }

    return jobs;
}

std::vector<uint8_t> BuildJaudioLibrary(const FileChunk& startup, const std::optional<FileChunk>& sequences,
                                        const std::vector<FileChunk>& waves, ArchiveDescriptor descriptor,
                                        const xsflib::psf::Tags& tags)
{
    ProgramBuilder program;
    program.AddFileRef(startup.path, startup.data);
    if (sequences)
    {
        program.AddFileRef(sequences->path, sequences->data);
    }
    for (const FileChunk& wave : waves)
    {
        program.AddFileRef(wave.path, wave.data);
    }

    return program.WritePsf(kPsfVersionHsf, descriptor.Serialize(), tags);
}

std::vector<uint8_t> BuildJaudioMini(uint32_t id, const std::string& lib_name, xsflib::psf::Tags tags)
{
    ProgramBuilder program;
    program.AddSound(id);
    tags["_lib"] = lib_name;

    return program.WritePsf(kPsfVersionHsf, {}, tags);
}

} // namespace hsf::rip
