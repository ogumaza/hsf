// SPDX-License-Identifier: MIT

#include "jaudio1/engine.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/jaudio/aaf.h"
#include "ntdlib/jaudio/bank.h"
#include "ntdlib/jaudio/rarc.h"
#include "ntdlib/jaudio/wave_system.h"
#include "xsflib/binary.h"

#include "jaudio/dsp.h"
#include "jaudio/tables.h"
#include "jaudio1/channel.h"
#include "jaudio1/conductor.h"
#include "jaudio1/instruments.h"
#include "jaudio1/oscillator.h"
#include "jaudio1/track.h"

namespace hsf::jaudio1
{
namespace
{

namespace nj = ntdlib::jaudio;

// The DSP's memory below the waves: the 64 channels' blocks, the FX lines' settings, the outputs of a frame's 7
// sub-frames, and the FX lines' buffers.
constexpr uint32_t kBlocks = 0x0000;
constexpr uint32_t kFxSettings = 0x6000;
constexpr uint32_t kFxLineBytes = 0x20;
constexpr uint32_t kLeft = 0x6100;
constexpr uint32_t kRight = 0x6800;
constexpr uint32_t kFxBuffers = 0x8000;
constexpr uint32_t kFxBufferBytes = SoundFiles::kMaxFxSubframes * 0xa0;
static_assert(kFxBuffers + 4 * kFxBufferBytes <= SoundFiles::kWaves);

// The loudest sample of an FX line's delay that counts as silence: the length analysis's level of -72 dB. A line's
// feedback rounds its products, and can keep quieter samples going round for ever.
constexpr int kFxSilence = 8;

// JASDsp's table of sends (0x944b96 in the release's main). An FX scene names each of a line's two buses by an index
// into it.
constexpr std::array<uint16_t, 12> kSends = {0x0d00, 0x0d60, 0x0dc0, 0x0e20, 0x0e80, 0x0ee0,
                                             0x0ca0, 0x0f40, 0x0fa0, 0x0b00, 0x09a0, 0x0000};

// JAI's number for the root track of a sequence (JAIGlobalParameter's seqTrackMax). JAI numbers a child of the root
// track by its slot.
constexpr uint32_t kRootNumber = 32;
constexpr uint32_t kChildNumbers = 16;

// The levels that a port command sets (the flags of JASPortArgs).
constexpr uint8_t kPortVolume = 0x01;
constexpr uint8_t kPortPitch = 0x02;
constexpr uint8_t kPortPan = 0x04;
constexpr uint8_t kPortFxmix = 0x08;
constexpr uint8_t kPortDolby = 0x10;
constexpr uint8_t kPortTempo = 0x80;

// The levels that outerInit switches on. A piece's kept levels use the same bits.
constexpr std::array<uint16_t, 5> kOuterLevels = {OuterParam::kVolume, OuterParam::kPitch, OuterParam::kFxmix,
                                                  OuterParam::kPan, OuterParam::kDolby};

// BgmSeq::setConfigVolume gives line 3 of a piece's volume.
constexpr float kMusicLevel = 0.8f;

// With this bit in the sound table's format for the sequences, a piece's volume over 127 gives line 1 of its volume
// (JAIBasic::setSeExtParameter).
constexpr uint8_t kFormatVolume = 4;

// The sound table (JAInter::SoundTable): the formats and the version in its first 4 bytes, the categories' counts and
// first places from byte 6, and the sounds from kSoundInfos. Categories 0 to 15 hold effects
// (SoundFiles::kEffectCategories), 16 the sequences and 17 the streams.
constexpr std::size_t kSoundInfos = 0x50;
constexpr uint32_t kSoundCategories = 18;

// JAI's work for a piece runs once in each of the game's frames. The model takes 30 frames a second.
constexpr double kGameFrameRate = 30.0;

// The registers of a track that hold the game's handle of the track's object (PSSystem::setObject). The model writes
// 0 for the handle.
constexpr uint32_t kObjectRegister = 18;

// The requests of the tracks to the game's code (PSGame's seqCpuSync) that change the piece.
constexpr uint32_t kSyncLevels = 0x0;
constexpr uint32_t kSyncSeLevels = 0x1;
constexpr uint32_t kSyncRootUpdate = 0x1e00;
constexpr uint32_t kSyncChildUpdate = 0x1f00;
constexpr uint32_t kSyncBeat = 0x2e00;
constexpr uint32_t kSyncStartPoint = 0x4000;
constexpr uint32_t kSyncJump = 0x4001;
constexpr uint32_t kSyncRootInit = 0xe00;
constexpr uint32_t kSyncChildInit = 0xf00;

// A request that the conductor has answered. No track's request has this value: requests are 16 bits.
constexpr uint32_t kSyncConducted = 0x10000;
constexpr uint16_t kNoJump = 0xffff;

// The requests of the horns' tunes (PSGame::Rappa, one for each captain): the horn's place in its tune, and the wait
// after each note. The game's start makes the horns, at place 0. The wait comes from the tilt of the stick: 3 ticks
// (Rappa::cBaseWaitTime) at full tilt. The horn's place is the track's port 11 at the last wait.
constexpr uint32_t kSyncHornPlace = 0x3000;
constexpr uint32_t kSyncHornWait = 0x3001;
constexpr uint32_t kSyncSecondHornPlace = 0x3002;
constexpr uint32_t kSyncSecondHornWait = 0x3003;
constexpr uint32_t kHornPlacePort = 11;
constexpr uint16_t kHornWait = 3;

// The request for the story's flag of the paid debt (Game::playData). The model plays the story's start: the debt
// unpaid.
constexpr uint32_t kSyncDebtPaid = 0x5000;

// The requests that reach the cave music's conductor (PSAutoBgm) from a piece without one.
constexpr std::array<uint32_t, 8> kSyncsOfOtherCode = {0x300, 0x600, 0x800, 0x900, 0xa00, 0xb00, 0xc00, 0xd00};

// TrackOnDirectorBase::onPlayInit: a muted layer's pause status. Its notes stop as it mutes, and none start.
constexpr uint8_t kMutedLayerStatus = 0x60;

// Whether the start-up archive is little-endian: its first word is a block's type in that order.
bool LittleEndianAaf(std::span<const uint8_t> aaf)
{
    if (aaf.size() < 4)
    {
        return false;
    }

    const uint32_t first = xsflib::LoadLe32(aaf.data());
    return first >= 1 && first <= 8;
}

// The start-up archive's blocks that the engine reads: the sound table (type 1), the effects' scenes (type 6) and the
// FX scenes (type 7). Each type word is followed by three words for a block: its offset, its size and a value. The
// lists of banks (type 2) and wave systems (type 3) give blocks up to a 0 word. A little-endian archive widens the
// offsets of the lists' entries and of a block of type 5 to 64 bits.
struct AafBlocks
{
    std::span<const uint8_t> sound_table;
    std::span<const uint8_t> effect_scenes;
    std::span<const uint8_t> fx_scenes;
};

AafBlocks FindAafBlocks(std::span<const uint8_t> aaf)
{
    const bool little = LittleEndianAaf(aaf);
    const xsflib::Reader r(aaf, !little);
    const std::size_t entry = little ? 16 : 12;
    AafBlocks blocks;
    for (std::size_t at = 0;;)
    {
        const uint32_t type = r.U32(at);
        at += 4;
        if (type == 0)
        {
            return blocks;
        }

        if (type == 2 || type == 3)
        {
            while (r.U32(at) != 0)
            {
                at += entry;
            }
            at += 4;
        }
        else if (type <= 8)
        {
            if (type == 1)
            {
                blocks.sound_table = r.Sub(r.U32(at), r.U32(at + 4));
            }
            else if (type == 6)
            {
                blocks.effect_scenes = r.Sub(r.U32(at), r.U32(at + 4));
            }
            else if (type == 7)
            {
                blocks.fx_scenes = r.Sub(r.U32(at), r.U32(at + 4));
            }
            at += type == 5 ? entry : 12;
        }
        else
        {
            while (r.U32(at) != 0)
            {
                at += 4;
            }
            at += 4;
        }
    }
}

// Whether an oscillator keeps its level or repeats a cycle. It keeps its level at its sustain, and from the note's
// start when it has no attack table. The game's step then gives the full scale and leaves the state as it is
// (JASOscillator::incCounter). A vibrato and a tremolo repeat a part of their tables (Oscillator::Cycling).
bool OscillatorHeld(const Oscillator& oscillator)
{
    const uint8_t state = oscillator.State().state;
    return state == Oscillator::kHold || (state == Oscillator::kStart && oscillator.Data()->attack.Null()) ||
           oscillator.Cycling();
}

// Whether a channel holds its note: it plays on a looping wave, and each of its envelopes keeps its level or repeats a
// cycle.
bool NoteHeld(const Channel& channel)
{
    if (!channel.alive || channel.status != Channel::kPlay ||
        (channel.wave_format == Channel::kWave && channel.wave.loop_flags == 0))
    {
        return false;
    }

    return std::all_of(channel.oscillators.begin(), channel.oscillators.end(),
                       [](const Oscillator& oscillator) { return !oscillator.Valid() || OscillatorHeld(oscillator); });
}

// Whether a track has opened a track that is still open. A track that closes leaves its parent's slot.
bool HasChildren(const Tracks& tracks, uint32_t index)
{
    const Track& track = tracks.TrackAt(index);
    return std::any_of(track.children.begin(), track.children.end(),
                       [](uint32_t child) { return child != Track::kNone; });
}

// The effects' settings at the listener. A rip starts an effect as the game starts one with no place in its world
// (PSSystem::SysIF::playSystemSe). PSGame starts JAI with 2 audio cameras: JAI then takes no distance, Doppler shift or
// distance send for an effect. The effects' default send (seDefaultFx) and the map's settings stay 0. JAI starts an
// effect that has no actor with a default actor. The default actor has no object. The actor's flag marks the effect as
// played with an actor. PSM::SeSound then gives the effect a send of 0.07 (CreaturePrm::cSeFxMix) and a pan of 0.5.
// The scene's send and the camera's distance volume go only to an effect without that flag, or to one with an object.
// The levels move over 5 frames as the distance changes (PSSystem's sDistanceParameterMoveTime).
constexpr float kEffectSend = 0.07f;
constexpr float kListenerPan = 0.5f;
constexpr float kSurroundCenter = 30.0f;
constexpr uint32_t kDistanceTime = 5;

// The flag of a game's effect ID that keeps the effect until it ends. The game sets it for most of its effects. It
// holds the others for as long as they play.
constexpr uint32_t kHeldEffect = 0x800;

// The levels that the effect sequence's slot tracks keep when they ask for them: the sequence's sound info (flag
// 0x1f00 in PSGame's start-up) keeps them all.
constexpr uint8_t kEffectKept = 0x1f;

// The flags of an effect's sound info that its levels read (JAInter's sound flags).
constexpr uint32_t kFlagNoDistanceVolume = 0x2;
constexpr uint32_t kFlagRandomPitch = 0x10;
constexpr uint32_t kFlagPitchSpread = 0xc0;
constexpr uint32_t kFlagNoMapSend = 0x400;

// The bits of the sound table's format for the effects.
constexpr uint8_t kFormatOffset = 1;
constexpr uint8_t kFormatPitch = 2;
constexpr uint8_t kFormatSend = 8;

// The port command's request for an interrupt of its track (JASPortArgs's flag 0x40), and the interrupt that stops an
// effect's slot track.
constexpr uint8_t kPortInterrupt = 0x40;
constexpr uint32_t kStopInterrupt = 5;

// A level that moves to a target over a number of the game's frames (JAInter::MoveParaSet). A level that takes its
// value at once keeps a move in progress: the move then carries it away again until the move's end.
struct MoveLevel
{
    float value = 1.0f;
    float target = 1.0f;
    float amount = 0.0f;
    uint32_t count = 0;

    // set.
    void Set(float level, uint32_t time)
    {
        if ((count == 0 && value == level) || (count != 0 && target == level))
        {
            return;
        }

        target = level;
        if (time == 0)
        {
            value = level;
            return;
        }
        amount = (value - target) / static_cast<float>(time + 1);
        count = time + 1;
    }

    // move.
    void Move()
    {
        if (count == 0)
        {
            return;
        }

        if (--count != 0)
        {
            value -= amount;
        }
        else
        {
            value = target;
        }
    }
};

// The lines of an effect's levels (JAInter's sound parameters). While line 7 holds -1, a level is the product or the
// sum of lines 0 to 6. Otherwise line 7 overrides them.
using Lines = std::array<MoveLevel, 8>;
constexpr std::size_t kLineDoppler = 1;
constexpr std::size_t kLineMap = 3;
constexpr std::size_t kLineDistance = 4;
constexpr std::size_t kLineOverride = 7;

Lines MakeLines(float value)
{
    Lines lines;
    for (MoveLevel& line : lines)
    {
        line.value = value;
        line.target = value;
    }
    lines[kLineOverride].value = -1.0f;
    lines[kLineOverride].target = -1.0f;

    return lines;
}

// checkPlayingSeUpdateMultiplication's product of the lines.
float LineProduct(const Lines& lines, float factor)
{
    float value = 1.0f;
    if (lines[kLineOverride].value == -1.0f)
    {
        for (std::size_t i = 0; i < kLineOverride; i++)
        {
            value *= lines[i].value;
        }
    }
    else
    {
        value = lines[kLineOverride].value;
    }

    return value * factor;
}

// checkPlayingSeUpdateAddition's sum of the lines' offsets from `center`, from 0 to 1.
float LineSum(const Lines& lines, float center)
{
    if (lines[kLineOverride].value != -1.0f)
    {
        return lines[kLineOverride].value;
    }

    float value = 0.0f;
    for (std::size_t i = 0; i < kLineOverride; i++)
    {
        value += lines[i].value - center;
    }
    value += center;
    return std::clamp(value, 0.0f, 1.0f);
}

// An effect on the effect sequence (a JAISe of PSM::SeSound's class, without an actor), from the game's call until JAI
// releases it: its sound info, its slot track and the slot's route for the ports, its state in JAInter::SeMgr, its
// levels' lines, and the levels that its slot track last took (SeMgr::TrackUpdate).
struct Effect
{
    enum State : uint8_t
    {
        kStored,
        kLoaded,
        kReady,
        kPlaying,
        kInactive,
    };

    uint32_t id = 0;
    SoundInfo info;
    uint32_t slot = 0;
    uint32_t slots = 0; // the slot tracks of the scene with the most (JAI's seTrackMax)
    uint32_t route = 0;
    State state = kStored;
    Lines volumes = MakeLines(1.0f);
    Lines pans = MakeLines(0.5f);
    Lines pitches = MakeLines(1.0f);
    Lines fxmixes = MakeLines(0.0f);
    Lines dolbys = MakeLines(kSurroundCenter / 127.0f);
    uint8_t pitch_spread = 0;     // mRandPitchModifier
    float distance_offset = 0.0f; // SeSound's mDistanceModifier
    float volume = 1.0f;
    float pitch = 1.0f;
    float fxmix = 0.0f;
    float pan = 0.5f;
    float dolby = 0.0f;
    bool stop = false; // the interrupt switch that releaseSeRegist leaves for the sequence manager

    // JAI's generator (JAInter::Const::random) and JALCalc's (getRandom_0_1). Each starts from its value at the
    // game's start.
    RandomFast jai_random;
    RandomFast jal_random;
};

// JAISe::setSeDistanceParameters with PSM::SeSound's overrides, for an effect with JAI's default actor at the listener.
// The levels move over `time` frames.
void SetDistanceLevels(Effect& e, uint32_t time)
{
    // setSeDistanceVolume. SeSound::setDistanceVolumeCommon also sets a line of the effect send.
    float volume = 1.0f;
    if ((e.info.flags & kFlagNoDistanceVolume) == 0)
    {
        e.fxmixes[kLineDoppler].Set(kEffectSend, 0);
        volume = std::clamp(1.0f - e.distance_offset, 0.0f, 1.0f);
    }
    e.volumes[kLineDistance].Set(volume, time);

    // SeSound::setSeDistancePan.
    e.pans[kLineDistance].Set(kListenerPan, time);

    // setSeDistancePitch. An effect with the random pitch takes a new pitch below 1 in each frame.
    float pitch = 1.0f;
    if ((e.info.flags & kFlagRandomPitch) != 0)
    {
        const auto step = static_cast<uint32_t>(static_cast<int32_t>(e.jai_random.Next() * 16.0f)) & 0xf;
        pitch = 1.0f - static_cast<float>(step) / 192.0f;
    }
    if ((e.info.flags & kFlagPitchSpread) != 0)
    {
        pitch += static_cast<float>(e.pitch_spread) / 192.0f;
    }
    e.pitches[kLineDistance].Set(pitch, time);

    // setSeDistanceFxmix, the map's send, and SeSound::setSeDistanceDolby.
    e.fxmixes[kLineDistance].Set(0.0f, time);
    if ((e.info.flags & kFlagNoMapSend) == 0)
    {
        e.fxmixes[kLineMap].Set(0.0f, 0);
    }
    e.dolbys[kLineDistance].Set(0.0f, time);
}

} // namespace

SoundFiles::SoundFiles(std::span<const uint8_t> aaf, std::span<const uint8_t> sequences, const WaveArchives& waves)
    : aaf_(aaf.begin(), aaf.end())
{
    if (!nj::IsAaf(aaf_))
    {
        throw xsflib::FormatError("the start-up archive isn't an AAF");
    }

    std::vector<uint8_t> decoded;
    for (const nj::ArchiveFile& file : nj::ReadRarc(nj::DecodeIfYaz0(sequences, decoded), nj::kWideRarcEntrySize))
    {
        SequenceFile& entry = sequences_.emplace_back();
        entry.id = file.id;
        entry.name = file.name;
        entry.sequence = std::make_unique<Sequence>(std::vector<uint8_t>(file.data.begin(), file.data.end()));
    }

    const AafBlocks blocks = FindAafBlocks(aaf_);
    if (blocks.sound_table.size() < kSoundInfos)
    {
        throw xsflib::FormatError("the start-up archive has no sound table");
    }
    sound_table_.assign(blocks.sound_table.begin(), blocks.sound_table.end());
    sequence_format_ = sound_table_[1];

    LoadWaves(waves);
    LoadBanks();
    LoadFxLines(blocks.fx_scenes);
    LoadEffectScenes(blocks.effect_scenes);
}

uint8_t SoundFiles::EffectFormat() const
{
    return sound_table_[0];
}

uint16_t SoundFiles::SoundCount(uint32_t category) const
{
    // JAInter::SoundTable::init: each of the 18 categories gives its count and its first sound's place in the table.
    if (category >= kSoundCategories)
    {
        return 0;
    }

    return xsflib::Reader(sound_table_, !LittleEndianAaf(aaf_)).U16(6 + 4 * category);
}

std::optional<SoundInfo> SoundFiles::FindSound(uint32_t id) const
{
    // JAInter::SoundTable::getInfoPointer. The table's sounds are 16 bytes each from kSoundInfos.
    uint32_t category = 0;
    switch (id >> 30)
    {
    case 0:
        category = (id >> 12) & 0xff;
        if (category >= kEffectCategories)
        {
            return std::nullopt;
        }
        break;

    case 2:
        category = kEffectCategories;
        break;

    case 3:
        category = kEffectCategories + 1;
        break;

    default:
        return std::nullopt;
    }

    const uint32_t index = id & 0x3ff;
    if (index >= SoundCount(category))
    {
        return std::nullopt;
    }

    const xsflib::Reader r(sound_table_, !LittleEndianAaf(aaf_));
    const std::size_t at = kSoundInfos + 16 * (static_cast<std::size_t>(r.U16(8 + 4 * category)) + index);
    if (at + 16 > r.Size())
    {
        return std::nullopt;
    }

    SoundInfo info;
    info.flags = r.U32(at);
    info.offset = r.U16(at + 6);
    info.pitch = r.F32(at + 8);
    info.volume = r.U8(at + 0xc);
    info.fxmix = r.U8(at + 0xd);

    return info;
}

const SoundFiles::SequenceFile* SoundFiles::FindSequence(uint16_t id) const
{
    for (const SequenceFile& file : sequences_)
    {
        if (file.id == id)
        {
            return &file;
        }
    }

    return nullptr;
}

void SoundFiles::LoadWaves(const WaveArchives& waves)
{
    // Each wave system's wave archives go into the DSP's memory, each once, on 32-byte boundaries. A wave that several
    // groups list has a copy in each of their archives, at another offset in each. The copies hold the same bytes. The
    // game plays the copy of the group that it loaded last (JASBasicWaveBank::incWaveTable). The model plays the copy
    // of the last group that lists the wave: ntdlib keeps that group's record of it.
    const std::vector<nj::WaveSystem> systems = nj::ReadAafWaveSystems(aaf_);
    std::map<std::string, const std::vector<uint8_t>*> archives;
    for (const nj::WaveSystem& system : systems)
    {
        for (const nj::WaveGroup& group : system.groups)
        {
            if (archives.contains(group.archive))
            {
                continue;
            }

            const std::vector<uint8_t>* bytes = waves(group.archive);
            archives[group.archive] = bytes;
            if (bytes == nullptr)
            {
                missing_waves_.push_back(group.archive);
            }
        }
    }

    uint32_t size = kWaves;
    std::map<std::string, uint32_t> addresses;
    for (const auto& [name, bytes] : archives)
    {
        if (bytes != nullptr && !bytes->empty())
        {
            addresses[name] = size;
            size = (size + static_cast<uint32_t>(bytes->size()) + 31) & ~31u;
        }
    }
    memory_ = std::make_unique<DspMemory>(size);
    for (const auto& [name, address] : addresses)
    {
        const std::vector<uint8_t>& bytes = *archives[name];
        std::copy(bytes.begin(), bytes.end(), memory_->Bytes().begin() + address);
    }

    for (const nj::WaveSystem& system : systems)
    {
        WaveBank& bank = wave_banks_[system.number];
        for (const auto& [id, info] : system.waves)
        {
            const auto address = addresses.find(info.archive);
            if (address == addresses.end())
            {
                continue;
            }

            WaveInfo wave;
            wave.format = static_cast<uint8_t>(info.format);
            wave.base_key = info.base_key;
            wave.sample_rate = info.sample_rate;
            wave.loop_flags = info.loop ? 0xffffffff : 0;
            wave.loop_start = info.loop_start;
            wave.loop_end = info.loop_end;
            wave.sample_count = info.sample_count;
            wave.last = info.loop_last;
            wave.penult = info.loop_penult;
            bank.Add(id, BankWave{wave, address->second + info.offset});
        }
    }
}

void SoundFiles::LoadBanks()
{
    // JAInter::BankWave::init reads every bank of the list, in order. A bank's place in the list is its physical
    // number. The banks' random settings step the generator as they're read.
    std::vector<std::string> warnings;
    const std::vector<nj::Bank> banks = nj::ReadAafBanks(aaf_, warnings);
    if (!warnings.empty())
    {
        throw xsflib::FormatError("the start-up archive has a damaged bank: " + warnings.front());
    }

    for (const nj::Bank& parsed : banks)
    {
        auto bank = std::make_unique<InstrumentBank>(parsed, inst_random_);
        const auto waves = wave_banks_.find(parsed.wave_system);
        bank->SetWaveBank(waves != wave_banks_.end() ? &waves->second : nullptr);
        banks_.push_back(std::move(bank));
    }
}

void SoundFiles::LoadFxLines(std::span<const uint8_t> scenes)
{
    // JAInter::Fx::init. The block of FX scenes gives the number of scenes, each line's buffer in sub-frames, and each
    // scene's offset in the block. A scene gives the four lines' settings in 0x20 bytes each (JASDsp::FxlineConfig_):
    // the flags, the indexes and levels of the two sends, the delay's length and the filter. A line whose buffer is 0
    // stays off. The others take the first scene's settings.
    if (scenes.empty())
    {
        return;
    }

    const xsflib::Reader r(scenes, !LittleEndianAaf(aaf_));
    if (r.U32(0) == 0)
    {
        return;
    }

    const uint32_t scene = r.U32(0x14);
    for (std::size_t line = 0; line < fx_lines_.size(); line++)
    {
        const uint32_t buffer = r.U32(4 + 4 * line);
        if (buffer == 0)
        {
            continue;
        }
        if (buffer > kMaxFxSubframes)
        {
            throw xsflib::FormatError("an FX line's buffer is longer than " + std::to_string(kMaxFxSubframes) +
                                      " sub-frames");
        }

        const std::size_t at = scene + line * kFxLineBytes;
        FxLine& fx = fx_lines_[line];
        fx.buffer_subframes = buffer;
        fx.flags = r.U8(at);
        fx.subframes = static_cast<uint16_t>(std::min(r.U32(at + 0xc), buffer));
        for (std::size_t send = 0; send < 2; send++)
        {
            const uint16_t index = r.U16(at + 2 + 4 * send);
            if (index >= kSends.size())
            {
                throw xsflib::FormatError("an FX line sends to a bus past the end of the table of sends");
            }
            fx.buses[send] = kSends[index];
            fx.levels[send] = r.S16(at + 4 + 4 * send);
        }
        for (std::size_t i = 0; i < fx.filter.size(); i++)
        {
            fx.filter[i] = r.S16(at + 0x10 + 2 * i);
        }
    }
}

void SoundFiles::LoadEffectScenes(std::span<const uint8_t> scenes)
{
    // JAInter::InitData's block of type 6: the number of scenes, then each scene's offset in the block (64 bits in a
    // little-endian archive). A scene gives 2 bytes for each category of effects up to the last with sounds: the
    // category's slot tracks (JAInter::SeMgr's categoryInfoTable), then the most effects that it keeps at once.
    if (scenes.empty())
    {
        return;
    }

    const bool little = LittleEndianAaf(aaf_);
    const xsflib::Reader r(scenes, !little);
    uint32_t categories = 0;
    for (uint32_t category = 0; category < kEffectCategories; category++)
    {
        if (SoundCount(category) != 0)
        {
            categories = category + 1;
        }
    }

    const uint32_t count = r.U32(0);
    const std::size_t pointer = little ? 8 : 4;
    for (uint32_t scene = 0; scene < count; scene++)
    {
        const std::size_t at = r.U32(4 + pointer * scene);
        std::vector<uint8_t>& slots = effect_scenes_.emplace_back();
        for (uint32_t category = 0; category < categories; category++)
        {
            slots.push_back(r.U8(at + 2 * category));
        }
    }
}

// The model of one piece: the channels, the tracks and the DSP, and JAI's state for the piece's sequence.
struct Engine::Model
{
    explicit Model(DspMemory& memory) : voices(kBlocks), tracks(voices), dsp(memory, jaudio::DspTables())
    {
    }

    Voices voices;
    Tracks tracks;
    jaudio::Dsp dsp;
    uint32_t root = Track::kNone;
    OuterParam root_outer;
    PieceSettings piece;

    // JAI's levels for the piece (JAInter::SeqUpdateData): the product of the volume's lines, and a port command for
    // the root track and for each child by its number. A command waits in JASPortCmd's list for the next sub-frame. A
    // command that is already in the list keeps its place and takes the new levels.
    struct PortArgs
    {
        float volume = 0.0f;
        float pitch = 0.0f;
        float fxmix = 0.0f;
        float pan = 0.0f;
        float dolby = 0.0f;
        float tempo = 0.0f;
        uint8_t flags = 0;
        bool interrupt = false; // with flag 0x40: the command requests the track's stop (the arguments' word 9)
        bool queued = false;
    };
    float volume = 1.0f;
    std::array<PortArgs, kRootNumber + 1> ports{};
    std::vector<uint32_t> port_queue;

    // The children that have asked for their levels (the mask at +4 of SeqUpdateData), and whether JAI has found the
    // piece playing (SOUNDSTATE_Playing).
    uint32_t asked = 0;
    bool started = false;

    // The sub-frames between the game's frames, and the sub-frame at which the next frame falls due.
    double frame_subframes = 0.0;
    double next_frame = 0.0;

    // The root track's tempo when its object took the track (SeqTrackRoot's standard tempo), and whether the root
    // track has asked for an odd number of beats since then (bit 0 of BeatMgr's flags).
    uint16_t standard_tempo = 120;
    bool odd_beat = false;

    // An AutoBgm piece's conductor.
    std::unique_ptr<Conductor> conductor;

    // The effect on the effect sequence, or null for a piece of music.
    std::unique_ptr<Effect> effect;

    // Each horn's place in its tune (Rappa's table index).
    std::array<uint16_t, 2> horn_places{};

    uint32_t unmodelled = 0;
    uint64_t subframe = 0;
};

Engine::Engine(const SoundFiles& files, const Settings& settings)
    : files_(files), settings_(settings), memory_(std::make_unique<DspMemory>(files.Memory().Size()))
{
    std::copy(files.Memory().Bytes().begin(), files.Memory().Bytes().end(), memory_->Bytes().begin());
}

Engine::~Engine() = default;

std::optional<std::string> Engine::Start(uint16_t id, const PieceSettings& piece)
{
    model_.reset();
    const SoundFiles::SequenceFile* file = files_.FindSequence(id);
    if (file == nullptr)
    {
        return std::string("the sequence archive has no such file");
    }

    std::unique_ptr<Conductor> conductor;
    if (piece.kind == PieceSettings::kAuto)
    {
        try
        {
            conductor = std::make_unique<Conductor>(ConductorFile::Parse(piece.conductor));
        }
        catch (const xsflib::FormatError& e)
        {
            return std::string("the conductor file can't be read: ") + e.what();
        }
    }

    // JAI's start of the piece (checkReadSeq): the volume's lines, the sequence's set-up (setSeqData), the root
    // track's levels (rootInit) and its start (startSeq). The lines multiply in their order: the piece's volume (line
    // 1), the music's level (line 3) and the music's volume of the options (line 8). The others hold 1.
    float volume = 1.0f;
    if ((files_.SequenceFormat() & kFormatVolume) != 0)
    {
        volume = volume * (static_cast<float>(piece.volume) / 127.0f);
    }
    volume = volume * kMusicLevel;
    volume = volume * settings_.music_volume;
    if (auto error = StartSequence(*file, volume))
    {
        return error;
    }

    model_->piece = piece;
    model_->conductor = std::move(conductor);

    return std::nullopt;
}

std::optional<std::string> Engine::StartEffect(uint16_t id, uint8_t volume, uint32_t sound_id)
{
    model_.reset();
    const SoundFiles::SequenceFile* file = files_.FindSequence(id);
    if (file == nullptr)
    {
        return std::string("the sequence archive has no such file");
    }

    const std::optional<SoundInfo> info = (sound_id >> 30) == 0 ? files_.FindSound(sound_id) : std::nullopt;
    if (!info)
    {
        return std::string("the sound table has no such effect");
    }

    // The effect's slot track: its category's first in effect scene 0 (SeMgr's seScene stays 0). The slot tracks
    // count on from one category to the next.
    const uint32_t category = (sound_id >> 12) & 0xff;
    const std::vector<std::vector<uint8_t>>& scenes = files_.EffectScenes();
    if (scenes.empty() || category >= scenes[0].size() || scenes[0][category] == 0)
    {
        return std::string("effect scene 0 gives the effect's category no slot track");
    }

    uint32_t slot = 0;
    for (uint32_t i = 0; i < category; i++)
    {
        slot += scenes[0][i];
    }

    uint32_t slots = 0;
    for (const std::vector<uint8_t>& scene : scenes)
    {
        uint32_t sum = 0;
        for (const uint8_t count : scene)
        {
            sum += count;
        }
        slots = std::max(slots, sum);
    }

    // JAI's lines of the effect sequence's volume: the sequence's volume in the game's list of its music when the
    // sequences' format has the volume bit (SeqBase::init writes it into the sound info), line 3 at 1
    // (SeSeq::setConfigVolume), and line 8, the effects' volume of the options.
    float sequence_volume = 1.0f;
    if ((files_.SequenceFormat() & kFormatVolume) != 0)
    {
        sequence_volume = sequence_volume * (static_cast<float>(volume) / 127.0f);
    }
    sequence_volume = sequence_volume * 1.0f;
    sequence_volume = sequence_volume * settings_.effect_volume;
    if (auto error = StartSequence(*file, sequence_volume))
    {
        return error;
    }

    // storeSeBuffer and SeSound::initParameter. The game keeps the effect until it ends. The high 4 bits of the
    // effect's flags give the most that its volume drops at random, in fifteenths.
    Model& m = *model_;
    m.piece.kept_levels = kEffectKept;
    m.effect = std::make_unique<Effect>();
    Effect& e = *m.effect;
    e.id = sound_id | kHeldEffect;
    e.info = *info;
    e.slot = slot;
    e.slots = slots;
    e.route = 0x20000000u | ((slot >> 4) & 0xf) | ((slot & 0xf) << 4);
    const uint32_t drop = info->flags >> 28;
    if (drop != 0)
    {
        e.distance_offset = std::clamp(static_cast<float>(drop) / 15.0f * e.jal_random.Next(), 0.0f, 1.0f);
    }

    return std::nullopt;
}

void Engine::DrawSequenceNumbers(uint32_t count)
{
    if (model_)
    {
        model_->tracks.DrawRandom(count);
    }
}

uint32_t Engine::SequenceDraws() const
{
    return model_ ? model_->tracks.RandomDraws() : 0;
}

std::optional<std::string> Engine::StartSequence(const SoundFiles::SequenceFile& file, float volume)
{
    // The memory below the waves starts again from zeros.
    std::fill(memory_->Bytes().begin(), memory_->Bytes().begin() + SoundFiles::kWaves, uint8_t{0});
    model_ = std::make_unique<Model>(*memory_);
    Model& m = *model_;

    // The DSP's start-up (DsetupTable, DsetVARAM), and the FX lines of the first FX scene (JASDsp::setFXLine).
    m.dsp.SetFxLayout(jaudio::Dsp::FxLayout::kJaudio1);
    m.dsp.Setup(static_cast<uint16_t>(Voices::kDspChannels), kBlocks, kFxSettings);
    m.dsp.SetVaram(0);
    for (std::size_t line = 0; line < files_.FxLines().size(); line++)
    {
        const FxLine& fx = files_.FxLines()[line];
        if (fx.buffer_subframes == 0)
        {
            continue;
        }

        const uint32_t at = kFxSettings + static_cast<uint32_t>(line) * kFxLineBytes;
        memory_->SetNative16(at, fx.flags);
        memory_->SetNative16(at + 2, fx.subframes);
        memory_->SetNative32(at + 4, kFxBuffers + static_cast<uint32_t>(line) * kFxBufferBytes);
        for (uint32_t send = 0; send < 2; send++)
        {
            memory_->SetNative16(at + 8 + 4 * send, fx.buses[send]);
            memory_->SetNative16(at + 0xa + 4 * send, static_cast<uint16_t>(fx.levels[send]));
        }
        for (uint32_t i = 0; i < fx.filter.size(); i++)
        {
            memory_->SetNative16(at + 0x10 + 2 * i, static_cast<uint16_t>(fx.filter[i]));
        }
    }

    m.voices.SetChannelLevel(settings_.channel_level);
    m.voices.SetAutoLevel(settings_.auto_level);
    for (uint32_t i = 0; i < files_.Banks().size(); i++)
    {
        m.tracks.SetBank(i, files_.Banks()[i].get());
        m.tracks.SetVirtualBank(files_.Banks()[i]->Number(), i);
    }
    m.tracks.InstGenerator() = files_.InstGenerator();
    m.tracks.SetSyncCallback([this](uint32_t track, uint32_t value) { return Sync(track, value); });
    if (loop_hook_)
    {
        m.tracks.SetLoopHook(loop_hook_);
    }

    m.volume = volume;
    m.root = m.tracks.StartSequence(*file.sequence, &m.root_outer);
    if (m.root == Track::kNone)
    {
        model_.reset();
        return std::string("no root track is free");
    }
    OuterInit(kRootNumber, 0xffff);

    // The sequence starts in one of the game's frames. JAI's next frame falls due a frame later.
    m.frame_subframes = m.voices.DacRate() / jaudio::Dsp::kSubframeSamples / kGameFrameRate;
    m.next_frame = m.frame_subframes;
    output_.assign(2 * jaudio::Dsp::kSubframeSamples, int16_t{0});

    return std::nullopt;
}

uint32_t Engine::JaiTrack(uint32_t number) const
{
    // JAInter::SystemInterface::trackToSeqp: the root track, or the root's child in the slot. The effect sequence's ID
    // has the flag 0x800: its numbers name the children of the root's children.
    const Model& m = *model_;
    if (number == kRootNumber)
    {
        return m.root;
    }

    const Track& root = m.tracks.TrackAt(m.root);
    if (!m.effect)
    {
        return root.children[number & 0xf];
    }

    const uint32_t child = root.children[(number >> 4) & 0xf];
    return child == Track::kNone ? Track::kNone : m.tracks.TrackAt(child).children[number & 0xf];
}

void Engine::OuterInit(uint32_t number, uint16_t kept)
{
    // JAInter::SystemInterface::outerInit. The track's levels switch on at once. Each level that `kept` leaves out
    // drops to 0 at once. The port command then sets the levels at the next sub-frame's start. A child also takes its
    // mute from JAI's mute bits, which stay off while the game leaves the piece alone.
    Model& m = *model_;
    const uint32_t index = JaiTrack(number);
    if (index == Track::kNone)
    {
        return;
    }

    Model::PortArgs& args = m.ports[number];
    OuterParam* outer = m.tracks.TrackAt(index).outer;
    if (number == kRootNumber)
    {
        // SeqUpdateData's levels for the sequence: the volume's product, and the defaults of the others.
        args.volume = m.volume;
        args.pitch = 1.0f;
        args.fxmix = 0.0f;
        args.pan = 0.5f;
        args.dolby = 0.0f;
        args.tempo = 1.0f;
        args.flags = 0xff;
        outer->OnSwitch(OuterParam::kTempo);
    }
    else
    {
        // JAISequence's levels for the child: 1 for the volume and the pitch, 0.5 for the pan and 0 for the others. An
        // AutoBgm piece's start gives each of the conductor's tracks its effect send.
        args.volume = 1.0f;
        args.pitch = 1.0f;
        args.fxmix = m.conductor ? m.conductor->Fxmix(number).value_or(0.0f) : 0.0f;
        args.pan = 0.5f;
        args.dolby = 0.0f;
        args.interrupt = false;
        args.flags = 0x7f;
        m.tracks.MuteTrack(index, false);
    }

    for (const uint16_t level : kOuterLevels)
    {
        outer->OnSwitch(level);
    }
    for (const uint16_t level : kOuterLevels)
    {
        if ((kept & level) == 0)
        {
            outer->SetParam(static_cast<uint8_t>(level), 0.0f);
        }
    }

    if (!args.queued)
    {
        args.queued = true;
        m.port_queue.push_back(number);
    }
}

void Engine::RunPortCommands()
{
    // JASPortCmd::execAllCommand runs each command in the order that it joined the list. A command finds its track
    // again by its number, and does nothing if the track has gone (setSePortParameter).
    Model& m = *model_;
    for (const uint32_t number : m.port_queue)
    {
        Model::PortArgs& args = m.ports[number];
        args.queued = false;
        const uint32_t index = JaiTrack(number);
        if (index == Track::kNone)
        {
            continue;
        }

        OuterParam* outer = m.tracks.TrackAt(index).outer;
        const std::array<std::pair<uint8_t, uint16_t>, 6> order = {{
            {kPortVolume, OuterParam::kVolume},
            {kPortPitch, OuterParam::kPitch},
            {kPortPan, OuterParam::kPan},
            {kPortFxmix, OuterParam::kFxmix},
            {kPortTempo, OuterParam::kTempo},
            {kPortDolby, OuterParam::kDolby},
        }};
        const std::array<float, 6> values = {args.volume, args.pitch, args.pan, args.fxmix, args.tempo, args.dolby};
        for (std::size_t i = 0; i < order.size(); i++)
        {
            if ((args.flags & order[i].first) != 0)
            {
                outer->SetParam(static_cast<uint8_t>(order[i].second), values[i]);
                args.flags = static_cast<uint8_t>(args.flags ^ order[i].first);
            }
        }

        // The request for the stop keeps its flag.
        if ((args.flags & kPortInterrupt) != 0 && args.interrupt)
        {
            m.tracks.SetInterrupt(index, kStopInterrupt);
        }
    }
    m.port_queue.clear();
}

void Engine::GameFrame()
{
    // checkPlayingSeq: the stop of a released effect's slot track (the interrupt switch), by a port command for a
    // track that has asked for its levels.
    Model& m = *model_;
    if (m.effect && m.started && m.effect->stop)
    {
        m.effect->stop = false;
        Model::PortArgs& args = m.ports[m.effect->slot];
        args.interrupt = true;
        if ((m.asked & (1u << m.effect->slot)) != 0)
        {
            args.flags = kPortInterrupt;
            if (!args.queued)
            {
                args.queued = true;
                m.port_queue.push_back(m.effect->slot);
            }
        }
    }

    // checkStartedSeq. Once the root track plays and has opened a child, JAI gives each track that it numbers and that
    // hasn't asked for its levels its levels (trackInit). JAI's other work changes nothing in a piece that the game
    // leaves alone. Neither does the directors' work (DirectorMgrBase::exec).
    if (m.started || !m.tracks.Playing(m.root) || !HasChildren(m.tracks, m.root))
    {
        return;
    }

    m.started = true;
    const uint32_t numbers = m.effect ? kRootNumber : kChildNumbers;
    for (uint32_t number = 0; number < numbers; number++)
    {
        if ((m.asked & (1u << number)) == 0)
        {
            OuterInit(number, 0xffff);
        }
    }
}

void Engine::EffectFrame()
{
    // processGFrameSe for the one effect: checkNextFrameSe, checkSeMovePara and checkPlayingSe. SeMgr finds the effect
    // sequence playing (SOUNDSTATE_Playing) a frame after the sequence manager does.
    Model& m = *model_;
    Effect& e = *m.effect;
    if (!m.started || e.state == Effect::kInactive)
    {
        return;
    }

    // checkNextFrameSe: the stored effect takes its category's first slot, and loads. A playing effect is ready again.
    if (e.state == Effect::kStored)
    {
        e.state = Effect::kLoaded;
    }
    else if (e.state == Effect::kPlaying)
    {
        e.state = Effect::kReady;
    }

    for (Lines* lines : {&e.volumes, &e.pans, &e.fxmixes, &e.dolbys, &e.pitches})
    {
        for (MoveLevel& line : *lines)
        {
            line.Move();
        }
    }

    // checkPlayingSe reads ports 2 and 0 of the slot track. A missing slot track leaves the game's values unknown.
    uint16_t playing = 0;
    uint16_t start = 0;
    if (!m.tracks.ReadPortApp(m.root, e.route | 0x20000, playing) || !m.tracks.ReadPortApp(m.root, e.route, start))
    {
        m.unmodelled++;
    }

    if (e.state == Effect::kLoaded)
    {
        // The start. An effect with a spread of pitches draws its rise at random. The distance's levels take their
        // values at once, then the sound info's. The slot track takes the levels when it asks for them. The ports take
        // the wait, the FX line, the effect's number and the start.
        const uint32_t spread = e.info.flags & kFlagPitchSpread;
        if (spread != 0)
        {
            const auto draw = static_cast<uint32_t>(static_cast<int32_t>(e.jai_random.Next() * 255.0f));
            const uint32_t mask = spread == 0x40 ? 0xf : spread == 0x80 ? 0x1f : 0x3f;
            e.pitch_spread = static_cast<uint8_t>(draw & mask);
        }
        SetDistanceLevels(e, 0);

        // JAIBasic::setSeExtParameter.
        const uint8_t format = files_.EffectFormat();
        if ((format & kFormatVolume) != 0)
        {
            e.volumes[kLineDoppler].Set(static_cast<float>(e.info.volume) / 127.0f, 0);
        }
        if ((format & kFormatSend) != 0)
        {
            e.fxmixes[kLineDoppler].Set(static_cast<float>(e.info.fxmix) / 127.0f, 0);
        }
        if ((format & kFormatPitch) != 0)
        {
            e.pitches[kLineDoppler].Set(e.info.pitch, 0);
        }
        SendEffectLevels(false);

        // JAIBasic::getSoundOffsetNumberFromID. An effect with the ground's flag adds the ground of map 0, which is 0.
        const uint16_t number = (format & kFormatOffset) != 0 ? e.info.offset : static_cast<uint16_t>(e.id & 0x3ff);
        m.tracks.WritePortApp(m.root, e.route | 0x30000, 0);
        m.tracks.WritePortApp(m.root, e.route | 0x60000, 0);
        m.tracks.WritePortApp(m.root, e.route | 0x40000, number);
        m.tracks.WritePortApp(m.root, e.route, 1);
        e.state = Effect::kPlaying;
    }
    else if (playing == 0 && start != 1)
    {
        // releaseSeRegist: the slot track has finished the effect. Its port 0 drops to 0, and the sequence manager
        // stops the track.
        m.tracks.WritePortApp(m.root, e.route, 0);
        e.stop = true;
        e.state = Effect::kInactive;
    }
    else if (e.state == Effect::kReady)
    {
        SetDistanceLevels(e, kDistanceTime);
        SendEffectLevels(true);
        e.state = Effect::kPlaying;
    }
}

void Engine::SendEffectLevels(bool send)
{
    // sendSeAllParameter: the levels from the lines, each in the order of the code. A level that has changed goes to
    // the slot track by a port command, unless the effect is loading. The category's volume is 1.
    Model& m = *model_;
    Effect& e = *m.effect;
    Model::PortArgs& args = m.ports[e.slot];
    uint8_t flags = 0;
    const auto update = [&](float value, float& level, float& arg, uint8_t flag)
    {
        if (level != value)
        {
            level = value;
            if (send)
            {
                flags |= flag;
                arg = value;
            }
        }
    };
    update(LineProduct(e.volumes, 1.0f), e.volume, args.volume, kPortVolume);
    update(LineSum(e.pans, 0.5f), e.pan, args.pan, kPortPan);
    update(LineProduct(e.pitches, 1.0f), e.pitch, args.pitch, kPortPitch);
    update(LineSum(e.fxmixes, 0.0f), e.fxmix, args.fxmix, kPortFxmix);
    update(LineSum(e.dolbys, kSurroundCenter / 127.0f), e.dolby, args.dolby, kPortDolby);
    if (flags != 0)
    {
        args.flags = flags;
        if (!args.queued)
        {
            args.queued = true;
            m.port_queue.push_back(e.slot);
        }
    }
}

uint16_t Engine::Sync(uint32_t index, uint32_t value)
{
    // PSGame's seqCpuSync, for a piece whose handle the game holds.
    Model& m = *model_;
    const Track& track = m.tracks.TrackAt(index);
    const bool directed = m.piece.kind != PieceSettings::kBgm;
    const bool jump = m.piece.kind == PieceSettings::kJump;
    uint16_t answer = 0;
    const std::optional<uint16_t> conducted = m.conductor ? m.conductor->Sync(m.tracks, index, value) : std::nullopt;
    switch (conducted ? kSyncConducted : value)
    {
    case kSyncConducted:
        answer = *conducted;
        break;

    case kSyncLevels:
        // A child of the root track asks for its levels: outerInit with the levels that the piece keeps. On the effect
        // sequence, a child of the root's children asks. A request from another track finds no sequence.
        if (!m.effect && track.parent == m.root)
        {
            const uint32_t number = track.route & 0xf;
            OuterInit(number, m.piece.kept_levels);
            m.asked |= 1u << number;
        }
        else if (m.effect && track.parent != Track::kNone && m.tracks.TrackAt(track.parent).parent == m.root)
        {
            const uint32_t number = track.route & 0xff;
            if (number >= kRootNumber)
            {
                m.unmodelled++;
                break;
            }
            OuterInit(number, m.piece.kept_levels);
            m.asked |= 1u << number;
        }
        break;

    case kSyncRootUpdate:
    case kSyncChildUpdate:
        // The update of the track's object (SeqTrackBase::update) runs the directors' tasks. A piece that the game
        // leaves alone has none. A piece without objects has no object to update.
        if (!directed)
        {
            m.unmodelled++;
        }
        break;

    case kSyncBeat:
        // SeqTrackRoot::beatUpdate: the root track takes its standard tempo again. The swing stays off. It plays only
        // while the player leads the second captain. A jump piece's requests at the beat's top (onBeatTop) stay empty.
        if (!directed)
        {
            m.unmodelled++;
            break;
        }
        m.tracks.SetTempo(m.root, m.standard_tempo);
        answer = m.piece.beat_interval;
        m.odd_beat = !m.odd_beat;
        if (m.conductor)
        {
            // The conductor's root object counts every other beat, and answers the conductor's beat interval.
            if (m.odd_beat)
            {
                m.conductor->CountBeat();
            }
            answer = m.conductor->BeatInterval();
        }
        break;

    case kSyncStartPoint:
        // JumpBgmSeq::getSeqStartPoint.
        if (!jump)
        {
            m.unmodelled++;
            break;
        }
        answer = m.piece.start_point;
        break;

    case kSyncJump:
        // JumpBgmSeq::outputJumpRequest: no request.
        if (!jump)
        {
            m.unmodelled++;
            break;
        }
        answer = kNoJump;
        break;

    case kSyncRootInit:
        // DirectedBgm::initRootTrack_onPlaying. The track's registers take its object's handle (setObject), and the
        // object takes the track's tempo as its standard tempo (SeqTrackRoot::init). The directors' start (playInit)
        // mutes no root track.
        if (!directed)
        {
            m.unmodelled++;
            break;
        }
        m.tracks.WriteRegDirect(index, kObjectRegister, 0);
        m.tracks.WriteRegDirect(index, kObjectRegister + 1, 0);
        m.standard_tempo = track.tempo;
        m.odd_beat = false;
        break;

    case kSyncChildInit:
        // DirectedBgm::initChildTrack_onPlaying for the slot in the track's route: setObject, then the directors'
        // start. The director of a layer mutes its track (TrackOnDirectorBase::onPlayInit).
        if (!directed)
        {
            m.unmodelled++;
            break;
        }
        m.tracks.WriteRegDirect(index, kObjectRegister, 0);
        m.tracks.WriteRegDirect(index, kObjectRegister + 1, 0);
        if ((m.piece.muted_tracks & (1u << (track.route & 0xf))) != 0)
        {
            m.tracks.TrackAt(index).pause_status |= kMutedLayerStatus;
            m.tracks.MuteTrack(index, true);
        }
        break;

    case kSyncHornPlace:
    case kSyncSecondHornPlace:
        answer = m.horn_places[value == kSyncHornPlace ? 0 : 1];
        break;

    case kSyncHornWait:
    case kSyncSecondHornWait:
        m.horn_places[value == kSyncHornWait ? 0 : 1] = m.tracks.ReadPortAppDirect(index, kHornPlacePort);
        answer = kHornWait;
        break;

    case kSyncDebtPaid:
        answer = 0;
        break;

    case kSyncSeLevels:
        // JAIBasic::setParameterSeqSync gives an effect's slot track, by the low byte of its route, the levels that it
        // last took. The other slots hold their first levels. The surround level stays 0 for a stereo output. A request
        // from a piece of music, or past the slots, reads outside the levels.
        if (!m.effect || (track.route & 0xff) >= m.effect->slots)
        {
            m.unmodelled++;
            break;
        }
        {
            const Effect& e = *m.effect;
            const bool effect_slot = (track.route & 0xff) == e.slot;
            OuterParam* outer = m.tracks.TrackAt(index).outer;
            outer->SetParam(static_cast<uint8_t>(OuterParam::kVolume), effect_slot ? e.volume : 1.0f);
            outer->SetParam(static_cast<uint8_t>(OuterParam::kPan), effect_slot ? e.pan : 0.5f);
            outer->SetParam(static_cast<uint8_t>(OuterParam::kPitch), effect_slot ? e.pitch : 1.0f);
            outer->SetParam(static_cast<uint8_t>(OuterParam::kFxmix), effect_slot ? e.fxmix : 0.0f);
            outer->SetParam(static_cast<uint8_t>(OuterParam::kDolby), 0.0f);
        }
        break;

    default:
        // The game's other code. Any other request changes nothing and gets 0.
        if (std::find(kSyncsOfOtherCode.begin(), kSyncsOfOtherCode.end(), value) != kSyncsOfOtherCode.end())
        {
            m.unmodelled++;
        }
        break;
    }

    return answer;
}

void Engine::RunSubframe()
{
    Model& m = *model_;
    const auto in_frame = static_cast<uint32_t>(m.subframe % kSubframesPerFrame);
    if (in_frame == 0)
    {
        m.dsp.StartFrame(kSubframesPerFrame, settings_.dsp_level, kLeft, kRight);
    }

    m.dsp.RenderSubframe();
    const uint32_t offset = in_frame * 2 * jaudio::Dsp::kSubframeSamples;
    for (int i = 0; i < jaudio::Dsp::kSubframeSamples; i++)
    {
        const auto at = static_cast<uint32_t>(2 * i) + offset;
        output_[2 * static_cast<std::size_t>(i)] = static_cast<int16_t>(memory_->Big16(kLeft + at));
        output_[2 * static_cast<std::size_t>(i) + 1] = static_cast<int16_t>(memory_->Big16(kRight + at));
    }

    // JAI's frame runs on the game's thread, between two sub-frames of the audio thread: the effects' work, then the
    // sequences'.
    if (static_cast<double>(m.subframe) >= m.next_frame)
    {
        if (m.effect)
        {
            EffectFrame();
        }
        GameFrame();
        m.next_frame += m.frame_subframes;
    }

    // JASDriver::updateDSP: the port commands, the DSP channels' update, then the sub-frame's callbacks.
    RunPortCommands();
    m.voices.UpdateDspChannels(*memory_, &m.tracks);
    m.tracks.RunSubframe();
    m.subframe++;
}

bool Engine::Playing() const
{
    if (!model_ || !model_->tracks.Playing(model_->root))
    {
        return false;
    }

    return !model_->effect || model_->effect->state != Effect::kInactive;
}

int Engine::ActiveChannels() const
{
    if (!model_)
    {
        return 0;
    }

    int count = 0;
    for (uint32_t i = 0; i < Voices::kDspChannels; i++)
    {
        count += model_->voices.DspChannelAt(i).status == DspChannel::kActive ? 1 : 0;
    }

    return count;
}

bool Engine::Busy() const
{
    return Playing() || ActiveChannels() != 0 || FxRinging();
}

bool Engine::FxRinging() const
{
    if (!model_)
    {
        return false;
    }

    // The buffers hold big-endian samples.
    const auto loud = [](int16_t sample)
    {
        return sample > kFxSilence || sample < -kFxSilence;
    };
    const std::span<const uint8_t> buffers = memory_->Bytes().subspan(kFxBuffers, 4 * kFxBufferBytes);
    for (std::size_t i = 0; i + 1 < buffers.size(); i += 2)
    {
        if (loud(static_cast<int16_t>(buffers[i] << 8 | buffers[i + 1])))
        {
            return true;
        }
    }

    const auto& delay = model_->dsp.Delay().samples;
    return std::any_of(delay.begin(), delay.end(), loud);
}

uint32_t Engine::MissingBankNotes() const
{
    return model_ ? model_->tracks.MissingBankNotes() : 0;
}

uint32_t Engine::Unmodelled() const
{
    if (!model_)
    {
        return 0;
    }

    return model_->tracks.Unmodelled() + model_->unmodelled + (model_->conductor ? model_->conductor->Unmodelled() : 0);
}

void Engine::SetLoopHook(LoopHook hook)
{
    loop_hook_ = std::move(hook);
    if (model_)
    {
        model_->tracks.SetLoopHook(loop_hook_);
    }
}

std::vector<uint32_t> Engine::TreeOrder() const
{
    std::vector<uint32_t> order;
    if (!model_ || model_->root == Track::kNone)
    {
        return order;
    }

    const Tracks& tracks = model_->tracks;
    std::vector<uint32_t> pending{model_->root};
    while (!pending.empty())
    {
        const uint32_t index = pending.back();
        pending.pop_back();
        const Track& track = tracks.TrackAt(index);
        if (track.status == Track::kClosed)
        {
            continue;
        }

        order.push_back(index);
        for (std::size_t slot = track.children.size(); slot-- > 0;)
        {
            if (track.children[slot] != Track::kNone)
            {
                pending.push_back(track.children[slot]);
            }
        }
    }

    return order;
}

bool Engine::PlayedNotes(uint32_t track) const
{
    return model_ && model_->tracks.PlayedNotes(track);
}

bool Engine::QuietPass(uint32_t track) const
{
    return model_ && model_->tracks.QuietPass(track);
}

bool Engine::Held() const
{
    if (!model_)
    {
        return false;
    }

    // A track that waits for its first note holds the count back.
    const Tracks& tracks = model_->tracks;
    for (const uint32_t index : TreeOrder())
    {
        if (!HasChildren(tracks, index) && !tracks.PlayedNotes(index) && !tracks.Looped(index))
        {
            return false;
        }
    }

    bool held = false;
    for (uint32_t i = 0; i < Voices::kChannels; i++)
    {
        const Channel& channel = model_->voices.ChannelAt(i);
        if (!channel.alive || channel.status == Channel::kStop)
        {
            continue;
        }

        if (!NoteHeld(channel))
        {
            return false;
        }

        held = true;
    }

    return held;
}

bool Engine::Moving() const
{
    if (!model_)
    {
        return false;
    }

    // A level that a command sets at once takes one step that changes nothing.
    for (const uint32_t index : TreeOrder())
    {
        const Track& track = model_->tracks.TrackAt(index);
        if (std::any_of(track.timed.begin(), track.timed.end(),
                        [](const MoveParam& p) { return p.time > 0.0f && p.amount != 0.0f; }))
        {
            return true;
        }
    }

    for (uint32_t i = 0; i < Voices::kChannels; i++)
    {
        const Channel& channel = model_->voices.ChannelAt(i);
        if (channel.alive && channel.sweep_steps != 0)
        {
            return true;
        }
    }

    return false;
}

bool Engine::WaitsForNotes(uint32_t track) const
{
    return model_ && model_->tracks.TrackAt(track).wait == -1;
}

bool Engine::WaitsForEver(uint32_t index) const
{
    if (!WaitsForNotes(index))
    {
        return false;
    }

    // The track waits for the note of its first channel to end.
    const uint32_t channel = model_->tracks.TrackAt(index).channels[0];
    return channel != Track::kNone && NoteHeld(model_->voices.ChannelAt(channel));
}

bool Engine::Waiting() const
{
    if (!model_)
    {
        return false;
    }

    // A track that has started no note and has jumped back idles: it polls the game. The effects sequence's other
    // slot tracks do that.
    const Tracks& tracks = model_->tracks;
    const std::vector<uint32_t> order = TreeOrder();
    return !order.empty() && std::all_of(order.begin(), order.end(),
                                         [this, &tracks](uint32_t index)
                                         {
                                             return HasChildren(tracks, index) || WaitsForNotes(index) ||
                                                    (!tracks.PlayedNotes(index) && tracks.Looped(index));
                                         });
}

Engine::WavePlace Engine::DspChannelPlace(uint32_t index) const
{
    WavePlace place;
    if (!model_ || index >= Voices::kDspChannels)
    {
        return place;
    }

    // An oscillator's channel plays no wave.
    const DspChannel& dsp_channel = model_->voices.DspChannelAt(index);
    if (dsp_channel.status != DspChannel::kActive || dsp_channel.channel == Channel::kNone ||
        model_->voices.ChannelAt(dsp_channel.channel).wave_format != Channel::kWave)
    {
        return place;
    }

    const uint32_t block = kBlocks + index * static_cast<uint32_t>(DspChannelBlock::kBytes);
    place.playing = true;
    place.loops = memory_->Native16(block + static_cast<uint32_t>(DspChannelBlock::kLoops)) != 0;
    place.position =
        -static_cast<int64_t>(memory_->Native32(block + static_cast<uint32_t>(DspChannelBlock::kRemaining)));

    return place;
}

} // namespace hsf::jaudio1
