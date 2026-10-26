// SPDX-License-Identifier: MIT

#include "jac/channel.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "jaudio/calc.h"

namespace hsf::jac
{

namespace
{

using jaudio::DspChannelBlock;
using jaudio::ToInt32;

// polys_table (0x5b7090 in main's ro): the voices of a sound's group by the class in the low 4 bits of its
// instrument's flags.
constexpr std::array<uint8_t, 16> kPolys = {0, 1, 2, 3, 4, 5, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32};

// The DSP's buses by a mixer's bus number (0x5bf4ce in main's ro). Number 10 names 0x0a00 on the Switch. The table
// holds 25 entries before other data. The model gives 0 for a number past them.
constexpr std::array<uint16_t, 25> kBuses = {0x0000, 0x0d00, 0x0d60, 0x0dc0, 0x0e20, 0x0e80,
                                             0x0ee0, 0x0ca0, 0x0f40, 0x0fa0, 0x0a00, 0x09a0};

// By a wave's format (0 to 7, 0x5b7048 and 0x5b7000 in main's ro): the samples that the DSP's first block holds, and
// the bytes of a block of 16 samples. No wave of the bank file has a format above 3. The model writes 0 for a format
// above 7.
constexpr std::array<uint8_t, 8> kFirstBlockSamples = {0x10, 0x10, 1, 1, 1, 0x10, 0x10, 1};
constexpr std::array<uint8_t, 8> kBlockBytes = {9, 5, 8, 16, 1, 1, 1, 1};

// Two words of a DSP channel's block that DspChannelBlock doesn't name: the frames since the DSP channel started
// (DSP_PlayStart clears them, and UpdateDSPchannelAll counts them), and the wave's count of samples.
constexpr std::size_t kPlayedFrames = 0x10c;
constexpr std::size_t kSampleCount = 0x11c;

// The pitch of the DSP's oscillators at key 60 (0x4682c008: 16,736.016), and a third (0x3eaaaaab), the weight of each
// source of a pan at a note's start.
const float kOscillatorPitch = std::bit_cast<float>(0x4682c008u);
const float kThird = std::bit_cast<float>(0x3eaaaaabu);

// The envelopes of a drum (PERC_ENV, 0x6b0500 in main's data) and of a DSP oscillator (OSC_ENV, 0x6b0520) with its
// release (OSC_REL, 0x6b0540). A drum's envelope has no tables and holds the note at full volume until its direct
// release.
constexpr std::array<OscPoint, 2> kOscReleasePoints = {{{1, 10, 0}, {15, 0, 1}}};
const OscData kPercEnvelope{OscData::kVolume, 1.0f, {}, {}, 1.0f, 0.0f};
const OscData kOscEnvelope{
    OscData::kVolume, 1.0f, {}, {kOscReleasePoints.data(), kOscReleasePoints.size()}, 1.0f, 0.0f};

// The ways that the three sources of a pan, an effect send or a surround level add up (0x5bf478 in main's ro): for
// each source, 0 leaves it out, 1 adds its value, and 2 adds its value times its weight. The table holds 27 types.
// A higher type reads the data that follows the table. Of those bytes, only the ones below hold a 1 or a 2.
using PanModes = std::array<uint8_t, 3>;

constexpr std::array<PanModes, 27> kPanTypes = {{
    {0, 0, 0}, {0, 0, 1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 1}, {0, 1, 2}, {0, 1, 0}, {0, 2, 1}, {0, 2, 2},
    {1, 0, 0}, {1, 0, 1}, {1, 0, 2}, {1, 1, 0}, {1, 1, 1}, {1, 1, 2}, {1, 2, 0}, {1, 2, 1}, {1, 2, 2},
    {1, 0, 0}, {2, 0, 1}, {2, 0, 2}, {2, 1, 0}, {2, 1, 1}, {2, 1, 2}, {2, 2, 0}, {2, 2, 1}, {2, 2, 2},
}};

struct PanByte
{
    uint8_t type;
    uint8_t source;
    uint8_t mode;
};

constexpr std::array<PanByte, 15> kPanBytesPastTable = {{
    {80, 1, 2},
    {83, 0, 2},
    {85, 2, 1},
    {88, 1, 1},
    {90, 2, 1},
    {91, 0, 1},
    {98, 0, 2},
    {162, 2, 2},
    {170, 0, 1},
    {170, 1, 1},
    {173, 0, 1},
    {175, 2, 1},
    {178, 1, 2},
    {181, 0, 2},
    {248, 0, 1},
}};

constexpr std::array<PanModes, 256> MakePanTable()
{
    std::array<PanModes, 256> table{};
    for (std::size_t i = 0; i < kPanTypes.size(); i++)
    {
        table[i] = kPanTypes[i];
    }
    for (const PanByte& b : kPanBytesPastTable)
    {
        table[b.type][b.source] = b.mode;
    }

    return table;
}

constexpr std::array<PanModes, 256> kPanTable = MakePanTable();

// A level within 0 to 1. A NaN stays NaN: the game's compares then fail.
float ClampUnit(float value)
{
    if (value < 0.0f)
    {
        return 0.0f;
    }

    return value > 1.0f ? 1.0f : value;
}

int32_t ClampKey(int32_t key)
{
    return std::min(std::max(key, 0), 127);
}

// The square of a velocity's fraction of 127.
float VelocityVolume(uint32_t velocity)
{
    const float fraction = static_cast<float>(velocity & 0xff) / 127.0f;

    return fraction * fraction;
}

// Bank_SenseToOfs (0x272420 in main): an effect that follows the key or the velocity `value`.
float SenseToOfs(const InstEffect* effect, uint32_t value)
{
    if (effect == nullptr)
    {
        return 1.0f;
    }

    const uint32_t v = value & 0xff;
    const uint32_t center = effect->center;
    if (center == 0x7f || center == 0)
    {
        return effect->low + ((effect->high - effect->low) * static_cast<float>(v)) / 127.0f;
    }
    if (center > v)
    {
        return effect->low + (static_cast<float>(v) / static_cast<float>(center)) * (1.0f - effect->low);
    }

    const float above =
        static_cast<float>(static_cast<int32_t>(v - center)) / static_cast<float>(static_cast<int32_t>(127 - center));

    return above * (effect->high + -1.0f) + 1.0f;
}

// Bank_RandToOfs (0x2724d0 in main): a random effect.
float RandToOfs(const InstEffect* effect, Random& random)
{
    if (effect == nullptr)
    {
        return 1.0f;
    }

    return random.SF32() * effect->width + effect->base;
}

// The effect of a kind in slot `slot` (0 or 1) of an instrument or a drum. ntdlib keeps the effects that the record
// gives, the random ones first. No record of the Switch's bank file leaves its first slot of a kind empty and fills
// the second.
const InstEffect* EffectAt(const std::vector<InstEffect>& effects, InstEffect::Kind kind, std::size_t slot)
{
    std::size_t seen = 0;
    for (const InstEffect& effect : effects)
    {
        if (effect.kind == kind && seen++ == slot)
        {
            return &effect;
        }
    }

    return nullptr;
}

// GetInstVmap's search of a key's velocity regions: the first whose range reaches `velocity`, or null.
const ntdlib::jaudio::VelocityRegion* RegionFor(const std::vector<ntdlib::jaudio::VelocityRegion>& regions,
                                                uint32_t velocity)
{
    for (const ntdlib::jaudio::VelocityRegion& region : regions)
    {
        if (region.high_velocity >= velocity)
        {
            return &region;
        }
    }

    return nullptr;
}

// The writes of dspinterface (0x276d70 to 0x277290 in main) that differ from JASDsp's. The rest are DspBlock's.

std::size_t MixerOffset(uint32_t mixer)
{
    return DspChannelBlock::kMixers + (mixer & 0xff) * 8;
}

void ClearBlock(DspBlock& block, std::size_t start, std::size_t end)
{
    for (std::size_t offset = start; offset < end; offset += 2)
    {
        block.SetU16(offset, 0);
    }
}

// DSP_AllocInit: the pause, the forced stop and the start flags clear, the auto mixer off, the FIR and IIR filters
// passing the signal through, no distortion. The FIR's last 12 coefficients, the space after them and the PCM
// position keep their values.
void AllocInit(DspBlock& block)
{
    block.SetU16(DspChannelBlock::kPause, 0);
    block.SetU16(DspChannelBlock::kForcedStop, 0);
    block.SetU32(DspChannelBlock::kActive, 0);
    block.SetU16(DspChannelBlock::kAutoMixer, 0);
    ClearBlock(block, DspChannelBlock::kFirCoefficients, DspChannelBlock::kFirCoefficients + 0x10);
    block.SetU16(DspChannelBlock::kFirCoefficients, 0x7fff);
    ClearBlock(block, DspChannelBlock::kIirCoefficients, DspChannelBlock::kIirCoefficients + 8);
    block.SetU16(DspChannelBlock::kIirCoefficients, 0x7fff);
    block.SetU16(DspChannelBlock::kDist, 0);
}

// DSP_PlayStart: JASDsp's start, and the PCM position at 0.
void StartBlock(DspBlock& block)
{
    block.PlayStart();
    block.SetU32(DspChannelBlock::kPcmPosition, 0);
}

// DSP_SetWaveInfo: the wave's address, format and loop. The PCM position keeps its value, and no part of the wave is
// skipped.
void SetWaveInfo(DspBlock& block, const WaveRecord& record, uint32_t address)
{
    const jaudio::WaveInfo& wave = record.info;
    const bool known = wave.format < kBlockBytes.size();
    block.SetU32(DspChannelBlock::kWaveAddress, address);
    block.SetU16(DspChannelBlock::kLeftover, known ? kFirstBlockSamples[wave.format] : 0);
    block.SetU16(DspChannelBlock::kFormat, known ? kBlockBytes[wave.format] : 0);
    if (wave.format > 3)
    {
        return;
    }

    block.SetU32(kSampleCount, wave.sample_count);
    const auto loops = static_cast<uint16_t>(wave.loop_flags);
    block.SetU16(DspChannelBlock::kLoops, loops);
    if (loops != 0)
    {
        block.SetU32(DspChannelBlock::kLoopStart, wave.loop_start);
        block.SetU32(DspChannelBlock::kLoopEnd, wave.loop_end);
        block.SetU16(DspChannelBlock::kLoopLast, static_cast<uint16_t>(wave.last));
        block.SetU16(DspChannelBlock::kLoopPenult, static_cast<uint16_t>(wave.penult));
    }
    else
    {
        block.SetU32(DspChannelBlock::kLoopEnd, wave.sample_count);
    }
    ClearBlock(block, DspChannelBlock::kSavedBlock, DspChannelBlock::kDelayHistory);
}

// DSP_SetBusConnect.
void SetBusConnect(DspBlock& block, uint32_t mixer, uint32_t bus)
{
    const uint32_t number = bus & 0xff;
    block.SetU16(MixerOffset(mixer), number < kBuses.size() ? kBuses[number] : 0);
}

// DSP_SetMixerInitDelayMax.
void SetMixerInitDelayMax(DspBlock& block, uint8_t delay)
{
    block.SetU16(DspChannelBlock::kDelayCount, delay);
}

// DSP_SetMixerInitVolume: the volume and the target, and the delay in both bytes of the delays' word.
void SetMixerInitVolume(DspBlock& block, uint32_t mixer, uint16_t volume, uint8_t delay)
{
    const std::size_t offset = MixerOffset(mixer);
    block.SetU16(offset + 4, volume);
    block.SetU16(offset + 2, volume);
    block.SetU16(offset + 6, uint32_t{delay} * 0x101);
}

// DSP_SetMixerVolume: the target and the delay in the delays' high byte, unless the channel is stopping.
void SetMixerVolume(DspMemory& memory, DspBlock& block, uint32_t mixer, uint16_t volume, uint8_t delay)
{
    if (block.U16(DspChannelBlock::kForcedStop) != 0)
    {
        return;
    }

    const std::size_t offset = MixerOffset(mixer);
    block.SetU16(offset + 2, volume);
    memory.SetByte(block.Address() + static_cast<uint32_t>(offset + 7), delay);
}

// DSP_SetMixerTarget: the target alone, unless the channel is stopping.
void SetMixerTarget(DspBlock& block, uint32_t mixer, uint16_t volume)
{
    if (block.U16(DspChannelBlock::kForcedStop) != 0)
    {
        return;
    }

    block.SetU16(MixerOffset(mixer) + 2, volume);
}

// DSP_SetIIR4: the IIR filter's 4 coefficients.
void SetIir4(DspBlock& block, const std::array<int16_t, 4>& iir)
{
    for (std::size_t i = 0; i < iir.size(); i++)
    {
        block.SetU16(DspChannelBlock::kIirCoefficients + 2 * i, static_cast<uint16_t>(iir[i]));
    }
}

void SetFir8(DspBlock& block, const std::array<int16_t, 8>& fir)
{
    std::array<uint16_t, 8> values{};
    for (std::size_t i = 0; i < fir.size(); i++)
    {
        values[i] = static_cast<uint16_t>(fir[i]);
    }
    block.SetFir8FilterParam(values);
}

} // namespace

Channels::Channels(DspMemory& memory, uint32_t blocks, const Banks& banks, Random& random)
    : memory_(memory), blocks_(blocks), banks_(banks), random_(random)
{
    // DSP_InitBuffer (0x277400 in main): each block clear, then the DSP channels and the channels.
    waits_.channels.fill(kNone);
    for (uint32_t i = 0; i < kDspChannels; i++)
    {
        DspBlock block = Block(i);
        ClearBlock(block, 0, DspChannelBlock::kBytes);
    }
    InitDspChannels();
    InitGlobalChannels();
}

void Channels::SetMixerLevel(float level)
{
    // Channel_SetMixerLevel (0x274240 in main).
    mixer_level_ = static_cast<uint16_t>(ToInt32(level * 32767.0f));
}

// dspdriver.

void Channels::InitDspChannels()
{
    // InitDSPchannel (0x276760 in main).
    for (uint32_t i = 0; i < kDspChannels; i++)
    {
        DspChannel& d = dsp_channels_[i];
        d = DspChannel{};
        d.block = static_cast<uint8_t>(i);
    }
}

uint32_t Channels::AllocDsp(uint32_t channel)
{
    // DSP_AllocDSPchannel (0x276790 in main) for a mono channel: the first free DSP channel.
    for (uint32_t i = 0; i < kDspChannels; i++)
    {
        DspChannel& d = dsp_channels_[i];
        if (d.state == DspChannel::kFree)
        {
            d.state = DspChannel::kMono;
            d.channel = channel;
            d.priority = 1;
            DspBlock block = Block(i);
            AllocInit(block);

            return i;
        }
    }

    return kNone;
}

int32_t Channels::DeallocDsp(uint32_t dsp, uint32_t channel)
{
    // DSP_DeAllocDSPchannel (0x276880 in main). The model allocates no stereo pairs.
    if (dsp == kNone)
    {
        return -1;
    }

    DspChannel& d = dsp_channels_[dsp];
    if (d.channel != channel)
    {
        return -2;
    }

    if (d.state >= DspChannel::kMono && d.state <= DspChannel::kStopping)
    {
        d.state = DspChannel::kFree;
    }
    d.priority = 0;
    d.channel = kNone;
    d.callback = false;

    return 0;
}

bool Channels::ForceStopDsp(uint32_t dsp)
{
    // ForceStopDSPchannel (0x276950 in main).
    DspChannel& d = dsp_channels_[dsp];
    DspBlock block = Block(d.block);
    if (d.state == DspChannel::kStopping || block.U16(DspChannelBlock::kActive) == 0)
    {
        return false;
    }

    block.SetU16(DspChannelBlock::kForcedStop, 1);
    d.state = DspChannel::kStopping;

    return true;
}

void Channels::UpdateDspChannels()
{
    // UpdateDSPchannelAll (0x276b60 in main).
    EntryCheck();
    for (uint32_t i = 0; i < kDspChannels; i++)
    {
        DspChannel& d = dsp_channels_[i];
        if (d.state == DspChannel::kFree)
        {
            continue;
        }

        DspBlock block = Block(d.block);
        if (block.U16(DspChannelBlock::kFinished) != 0)
        {
            if (d.callback)
            {
                d.timer = static_cast<uint16_t>(Callback(i, DspChannel::kFinish));
            }
            block.SetU32(DspChannelBlock::kActive, 0);
            if (d.state == DspChannel::kFree)
            {
                continue;
            }
        }

        if (block.U16(DspChannelBlock::kForcedStop) == 0)
        {
            const uint32_t frames = block.U32(kPlayedFrames) + 1;
            block.SetU32(kPlayedFrames, frames);
            if (frames == d.release_time && d.callback)
            {
                d.timer = static_cast<uint16_t>(Callback(i, DspChannel::kPriority));
            }
        }

        if (!d.callback)
        {
            continue;
        }

        if (d.timer != 0)
        {
            d.timer--;
            if (d.timer != 0)
            {
                continue;
            }
        }

        d.timer = static_cast<uint16_t>(Callback(i, DspChannel::kUpdate));
        if (d.timer == 0)
        {
            block.SetU32(DspChannelBlock::kActive, 0);
        }
    }
}

// driverinterface: the managers and the lists.

void Channels::InitGlobalChannels()
{
    // InitGlobalChannel (0x2748e0 in main): InitJcs on the global manager, then each channel in its free list, the
    // last channel first.
    managers_[kGlobal] = ChannelManager{};
    for (uint32_t i = 0; i < kChannels; i++)
    {
        Channel& c = channels_[i];
        c.id = c.id == 0xffff ? 1 : static_cast<uint16_t>(c.id + 1);
        c.wave = nullptr;
        c.source = 0;
        c.mix_configs = {0x150, 0x210, 0x352, 0x412, 0, 0};
        c.dsp_channel = kNone;
        c.next = i == 0 ? kNone : i - 1;
        c.kind = Channel::kWave;
        c.moved = false;
        c.priority = 0x10101;
        c.release_time = 600;
        c.pan_calcs = {26, 1, 1};
        c.time = 0;
        c.countdown = 0;
        c.pause = false;
        c.callback = Channel::kNoCallback;
        c.sweep = Channel::kNoSweep;
        c.oscillators = {};
        c.manager = kGlobal;
        c.list_manager = kGlobal;
        c.list = ChannelManager::kFree;
    }

    managers_[kGlobal].lists[ChannelManager::kFree] = kChannels - 1;
    managers_[kGlobal].count = kChannels;
}

void Channels::InitManager(uint32_t manager)
{
    // InitJcs (0x274780 in main).
    managers_[manager] = ChannelManager{};
}

void Channels::InitOneShot(uint32_t manager, uint32_t count)
{
    // Init_1shot (0x2783c0 in main).
    if (managers_[manager].count != 0)
    {
        FixReleaseAll(manager);
    }
    InitManager(manager);
    FixAlloc(manager, count);
    managers_[manager].stealing = count != 0;
}

uint32_t Channels::FixAlloc(uint32_t manager, uint32_t count)
{
    // FixAllocChannel (0x274370 in main): up to `count` channels from the global manager's free list.
    ChannelManager& global = managers_[kGlobal];
    uint32_t moved = 0;
    while (moved < count)
    {
        const uint32_t channel = global.lists[ChannelManager::kFree];
        if (channel == kNone)
        {
            break;
        }

        Channel& c = channels_[channel];
        global.lists[ChannelManager::kFree] = c.next;
        ListAdd(manager, ChannelManager::kFree, channel);
        c.manager = manager;
        moved++;
    }

    managers_[manager].count += moved;
    global.count -= moved;

    return moved;
}

void Channels::FixRelease(uint32_t channel)
{
    // FixReleaseChannel (0x2743e0 in main): into the global manager's free list. The game doesn't cut the channel
    // from its list first.
    Channel& c = channels_[channel];
    ListAdd(kGlobal, ChannelManager::kFree, channel);
    managers_[c.manager].count--;
    managers_[kGlobal].count++;
    c.manager = kGlobal;
}

void Channels::FixReleaseAll(uint32_t manager)
{
    // FixReleaseChannelAll (0x274430 in main). The game never ends the move of the global manager onto itself.
    if (manager == kGlobal)
    {
        return;
    }

    FixMove(manager, kGlobal);
    managers_[kGlobal].count += managers_[manager].count;
    managers_[manager].count = 0;
}

void Channels::FixMoveAll(uint32_t from, uint32_t to)
{
    // FixMoveChannelAll (0x2745e0 in main).
    if (from == to)
    {
        return;
    }

    FixMove(from, to);
    managers_[to].count += managers_[from].count;
    managers_[from].count = 0;
    managers_[to].allocations += managers_[from].allocations;
    managers_[from].allocations = 0;
}

void Channels::FixMove(uint32_t from, uint32_t to)
{
    // Each list's channels in turn, each at the head of the same list of `to`. A channel that plays or releases is
    // marked as moved. A waiting channel that holds a slot of the queue loses the slot and goes into the free list.
    ChannelManager& m = managers_[from];
    for (const uint8_t list : {ChannelManager::kFree, ChannelManager::kActive, ChannelManager::kReleasing})
    {
        while (m.lists[list] != kNone)
        {
            const uint32_t channel = m.lists[list];
            Channel& c = channels_[channel];
            m.lists[list] = c.next;
            ListAdd(to, list, channel);
            c.manager = to;
            if (list != ChannelManager::kFree)
            {
                c.moved = true;
            }
        }
    }

    const uint32_t count = waits_.count;
    const uint32_t top = waits_.top;
    while (m.lists[ChannelManager::kWaiting] != kNone)
    {
        const uint32_t channel = m.lists[ChannelManager::kWaiting];
        Channel& c = channels_[channel];
        m.lists[ChannelManager::kWaiting] = c.next;
        uint8_t list = ChannelManager::kWaiting;
        for (uint32_t k = 0; k < count; k++)
        {
            const uint32_t slot = (top + k) & 0x3f;
            if (waits_.channels[slot] == channel)
            {
                waits_.channels[slot] = kNone;
                list = ChannelManager::kFree;
                break;
            }
        }
        ListAdd(to, list, channel);
        c.manager = to;
    }
}

uint32_t Channels::ListCount(uint32_t manager, uint8_t list) const
{
    // List_CountChannel (0x274270 in main).
    uint32_t count = 0;
    for (uint32_t channel = managers_[manager].lists[list]; channel != kNone; channel = channels_[channel].next)
    {
        count++;
    }

    return count;
}

int32_t Channels::ListCut(uint32_t channel)
{
    // List_CutChannel (0x2742a0 in main). The game reads through a null list and crashes. The model gives -1.
    Channel& c = channels_[channel];
    if (c.list_manager == kNone)
    {
        return -1;
    }

    uint32_t& head = managers_[c.list_manager].lists[c.list];
    if (head == channel)
    {
        head = c.next;
        c.list_manager = kNone;
        return 0;
    }

    int32_t place = 0;
    for (uint32_t before = head; before != kNone; before = channels_[before].next)
    {
        place++;
        if (channels_[before].next == channel)
        {
            channels_[before].next = c.next;
            c.list_manager = kNone;
            return place;
        }
    }

    return -1;
}

uint32_t Channels::ListGet(uint32_t manager, uint8_t list)
{
    // List_GetChannel (0x274300 in main).
    uint32_t& head = managers_[manager].lists[list];
    const uint32_t channel = head;
    if (channel != kNone)
    {
        head = channels_[channel].next;
        channels_[channel].list_manager = kNone;
    }

    return channel;
}

void Channels::ListAddTail(uint32_t manager, uint8_t list, uint32_t channel)
{
    // List_AddChannelTail (0x274320 in main).
    Channel& c = channels_[channel];
    c.list_manager = manager;
    c.list = list;
    uint32_t& head = managers_[manager].lists[list];
    if (head == kNone)
    {
        head = channel;
    }
    else
    {
        uint32_t last = head;
        while (channels_[last].next != kNone)
        {
            last = channels_[last].next;
        }
        channels_[last].next = channel;
    }
    c.next = kNone;
}

void Channels::ListAdd(uint32_t manager, uint8_t list, uint32_t channel)
{
    // List_AddChannel (0x274350 in main).
    Channel& c = channels_[channel];
    uint32_t& head = managers_[manager].lists[list];
    c.list_manager = manager;
    c.list = list;
    c.next = head;
    head = channel;
}

// driverinterface: the channels' levels and their DSP channels.

void Channels::InitChannel(uint32_t channel)
{
    // Channel_Init (0x274800 in main). The settings of the mixers, the priority, the release and the pans' types come
    // from the manager.
    Channel& c = channels_[channel];
    c.kind = Channel::kWave;
    c.moved = false;
    c.time = 0;
    c.countdown = 0;
    c.wave = nullptr;
    c.source = 0;
    c.callback = Channel::kNoCallback;
    c.sweep = Channel::kNoSweep;
    if (c.manager != kNone)
    {
        const ChannelManager& m = managers_[c.manager];
        c.mix_configs = m.mix_configs;
        c.priority = m.priority;
        c.release_time = m.release_time;
        c.pan_calcs = m.pan_calcs;
    }
    else
    {
        c.mix_configs = {0x150, 0x210, 0x352, 0x412, 0, 0};
        c.release_time = 600;
        c.priority = 0x10101;
        c.pan_calcs = {26, 1, 1};
    }
    c.id = c.id == 0xffff ? 1 : static_cast<uint16_t>(c.id + 1);
    c.oscillators = {};
    c.pause = false;
}

float Channels::PanCalc(const Channel& c, std::size_t matrix, uint8_t type) const
{
    // PanCalc (in UpdateEffecterParam, 0x274ba0 in main): the sources' values in turn, from 0.
    const PanModes& modes = kPanTable[type];
    const std::array<float, 3>& values = c.pans[matrix];
    const std::array<float, 3>& weights = c.pans[Channel::kPower];
    float sum = 0.0f;
    if (modes[0] == 2)
    {
        sum = values[0] * weights[0] + 0.0f;
    }
    else if (modes[0] == 1)
    {
        sum = values[0] + 0.0f;
    }
    for (std::size_t k = 1; k < 3; k++)
    {
        if (modes[k] == 2)
        {
            sum = sum + values[k] * weights[k];
        }
        else if (modes[k] == 1)
        {
            sum = sum + values[k];
        }
    }

    return sum;
}

void Channels::UpdateEffecterParam(uint32_t channel)
{
    // UpdateEffecterParam (0x274ba0 in main): the manager's levels when the channel took them last from the same
    // manager, then the pan, the effect send and the surround level, the volume and the pitch, and each mixer's
    // volume.
    Channel& c = channels_[channel];
    const ChannelManager& m = managers_[c.manager];
    if (c.last_manager == c.manager)
    {
        c.manager_pitch = m.pitch;
        c.manager_volume = m.volume;
        c.pans[Channel::kPan][Channel::kManager] = m.pan;
        c.pans[Channel::kFxmix][Channel::kManager] = m.fxmix;
        c.pans[Channel::kDolby][Channel::kManager] = m.dolby;
        c.pan_calcs = m.pan_calcs;
    }

    // Stereo gives each level its sum. Mono gives the pan's middle, the effect send's sum and no surround. In any
    // other mode, the game leaves the pan and the effect send unset. The model gives the middle and no send there.
    float pan = 0.5f;
    float fxmix = 0.0f;
    float dolby = 0.0f;
    if (output_mode_ == 1)
    {
        pan = c.pan_calcs[0] == 0 ? 0.5f : PanCalc(c, Channel::kPan, c.pan_calcs[0]);
        fxmix = PanCalc(c, Channel::kFxmix, c.pan_calcs[1]);
        dolby = PanCalc(c, Channel::kDolby, c.pan_calcs[2]);
    }
    else if (output_mode_ == 0)
    {
        fxmix = PanCalc(c, Channel::kFxmix, c.pan_calcs[1]);
    }

    const float volume_level = (c.volume * c.volume_modifier) * c.manager_volume;
    pan = ClampUnit(pan);
    fxmix = ClampUnit(fxmix);
    dolby = ClampUnit(dolby);
    c.final_pitch = static_cast<uint16_t>(ToInt32(((c.pitch * c.pitch_modifier) * c.manager_pitch) * 4096.0f));
    const float volume = ClampUnit(volume_level);
    const auto max = static_cast<float>(mixer_level_);

    if (c.mix_configs[0] == 0xffff)
    {
        // The auto mixer.
        if (c.dsp_channel != kNone)
        {
            DspBlock block = Block(dsp_channels_[c.dsp_channel].block);
            block.SetAutoMixer(ToInt32(volume * max), ToInt32(pan * 127.5f), ToInt32(dolby * 127.5f),
                               ToInt32(fxmix * 127.5f));
        }
        return;
    }

    // Each mixer's config: its bus in the high byte, then two steps of 4 bits. Each step multiplies the level by a
    // curve of a value: a quarter sine (Sinf3), the value itself, or the surround curve (the second step's surround
    // level). A step of 4 or above 7 takes the value of the last step before it, in this mixer or an earlier one,
    // or the volume before any step. The first mixer's first step takes the pan there.
    const float pan_rest = 1.0f - pan;
    const float fxmix_rest = 1.0f - fxmix;
    const float dolby_rest = 1.0f - dolby;
    float value = volume_level;
    for (std::size_t i = 0; i < c.mix_configs.size(); i++)
    {
        const uint16_t config = c.mix_configs[i];
        if (config < 0x100)
        {
            c.mixer_levels[i] = 0;
            continue;
        }

        float level = volume;
        const uint32_t first = (config >> 4) & 0xf;
        if (first != 0)
        {
            bool linear = false;
            switch (first)
            {
            case 1:
                value = pan;
                break;

            case 2:
                value = fxmix;
                linear = true;
                break;

            case 3:
                value = dolby;
                break;

            case 5:
                value = pan_rest;
                break;

            case 6:
                value = fxmix_rest;
                linear = true;
                break;

            case 7:
                value = dolby_rest;
                break;

            default:
                // The first mixer takes the pan here.
                if (i == 0)
                {
                    value = pan;
                }
                break;
            }
            level = volume * (linear ? value : Sinf3(value));
        }

        const uint32_t second = config & 0xf;
        if (second != 0)
        {
            enum Curve
            {
                kSine,
                kLinear,
                kSurround,
            };
            Curve curve = kSine;
            switch (second)
            {
            case 1:
                value = pan;
                break;

            case 2:
                value = fxmix;
                curve = kLinear;
                break;

            case 3:
                value = dolby;
                curve = kSurround;
                break;

            case 5:
                value = pan_rest;
                break;

            case 6:
                value = fxmix_rest;
                curve = kLinear;
                break;

            case 7:
                value = dolby_rest;
                curve = kSurround;
                break;

            default:
                break;
            }
            const float factor = curve == kLinear ? value : (curve == kSine ? Sinf3(value) : SurroundCurve(value));
            level = level * factor;
        }

        c.mixer_levels[i] = static_cast<uint16_t>(ToInt32(level * max));
    }
}

void Channels::UpdateToDsp(uint32_t channel)
{
    // UpdateJcToDSP (0x274a00 in main). A moved channel keeps its block's delays and filters.
    const Channel& c = channels_[channel];
    DspBlock block = Block(dsp_channels_[c.dsp_channel].block);
    if (c.moved)
    {
        for (uint32_t i = 0; i < c.mixer_levels.size(); i++)
        {
            SetMixerTarget(block, i, c.mixer_levels[i]);
        }
        block.SetPitch(c.final_pitch);
    }
    else
    {
        const ChannelManager& m = managers_[c.manager];
        for (uint32_t i = 0; i < c.mixer_levels.size(); i++)
        {
            SetMixerVolume(memory_, block, i, c.mixer_levels[i], m.delays[i]);
        }
        block.SetPitch(c.final_pitch);
        if ((m.filter_mode & 0x20) != 0)
        {
            SetIir4(block, m.iir);
        }
        if ((m.filter_mode & 0x1f) != 0)
        {
            SetFir8(block, m.fir);
        }
        block.SetFilterMode(m.filter_mode);
        block.SetDistFilter(static_cast<uint16_t>(m.distortion));
    }
    block.SetPauseFlag(c.pause ? 1 : 0);
}

void Channels::UpdateToDspInit(uint32_t channel)
{
    // UpdateJcToDSPInit (0x276210 in main): the mixers' start volumes, or the auto mixer's start, then the pitch and
    // the filters. The distortion keeps its value.
    const Channel& c = channels_[channel];
    const ChannelManager& m = managers_[c.manager];
    DspBlock block = Block(dsp_channels_[c.dsp_channel].block);
    if (static_cast<int16_t>(c.mix_configs[0]) == -1)
    {
        block.InitAutoMixer();
    }
    else
    {
        SetMixerInitDelayMax(block, m.delay_max);
        for (uint32_t i = 0; i < c.mixer_levels.size(); i++)
        {
            SetMixerInitVolume(block, i, c.mixer_levels[i], m.delays[i]);
        }
    }
    block.SetPitch(c.final_pitch);
    if ((m.filter_mode & 0x20) != 0)
    {
        SetIir4(block, m.iir);
    }
    if ((m.filter_mode & 0x1f) != 0)
    {
        SetFir8(block, m.fir);
    }
    block.SetFilterMode(m.filter_mode);
    block.SetPauseFlag(c.pause ? 1 : 0);
}

void Channels::DoEffectOsc(uint32_t channel, uint8_t target, float value)
{
    // DoEffectOsc (0x275780 in main).
    Channel& c = channels_[channel];
    switch (target)
    {
    case OscData::kVolume:
        c.volume_modifier = c.volume_modifier * value;
        break;
    case OscData::kPitch:
        c.pitch_modifier = c.pitch_modifier * value;
        break;
    case OscData::kPan:
        c.pans[Channel::kPan][Channel::kEffect] = value;
        break;
    case OscData::kFxmix:
        c.pans[Channel::kFxmix][Channel::kEffect] = value;
        break;
    case OscData::kDolby:
        c.pans[Channel::kDolby][Channel::kEffect] = value;
        break;
    default:
        break;
    }
}

void Channels::DoEffect(uint32_t channel, uint8_t target, float value)
{
    // An instrument's or a drum's effect at a note's start (in Play_1shot and Play_1shot_Perc).
    Channel& c = channels_[channel];
    switch (target)
    {
    case OscData::kVolume:
        c.volume = value * c.volume;
        break;
    case OscData::kPitch:
        c.pitch = value * c.pitch;
        break;
    case OscData::kPan:
        c.pans[Channel::kPan][Channel::kEffect] = value;
        break;
    case OscData::kFxmix:
        c.pans[Channel::kFxmix][Channel::kEffect] = value;
        break;
    case OscData::kDolby:
        c.pans[Channel::kDolby][Channel::kEffect] = value;
        break;
    default:
        break;
    }
}

bool Channels::StopLogical(uint32_t channel)
{
    // StopLogicalChannel (0x2757e0 in main).
    Channel& c = channels_[channel];
    if (c.dsp_channel == kNone)
    {
        return false;
    }

    DspChannel& d = dsp_channels_[c.dsp_channel];
    d.callback = false;
    d.timer = 0;
    Block(d.block).PlayStop();
    DeallocDsp(c.dsp_channel, channel);
    c.dsp_channel = kNone;

    return true;
}

bool Channels::CheckLogical(uint32_t channel) const
{
    // CheckLogicalChannel (0x275850 in main).
    const Channel& c = channels_[channel];

    return c.wave != nullptr || c.kind != Channel::kWave;
}

bool Channels::PlayLogical(uint32_t channel)
{
    // PlayLogicalChannel (0x275870 in main).
    Channel& c = channels_[channel];
    if (c.dsp_channel == kNone || (c.wave == nullptr && c.kind == Channel::kWave))
    {
        return false;
    }

    DspChannel& d = dsp_channels_[c.dsp_channel];
    DspBlock block = Block(d.block);
    d.callback = true;
    d.timer = 1;
    if (c.kind == Channel::kOscillator)
    {
        block.SetOscInfo(c.source);
    }
    else if (c.kind == Channel::kWave)
    {
        SetWaveInfo(block, *c.wave, c.source);
    }

    // Mono sends bus 9 to bus 2 and bus 8 to bus 11.
    for (uint32_t i = 0; i < c.mix_configs.size(); i++)
    {
        uint32_t bus = c.mix_configs[i] >> 8;
        if (output_mode_ == 0)
        {
            if (bus == 9)
            {
                bus = 2;
            }
            else if (bus == 8)
            {
                bus = 11;
            }
        }
        SetBusConnect(block, i, bus);
    }

    c.last_manager = c.manager;
    UpdateEffecterParam(channel);
    UpdateToDspInit(channel);
    d.priority = static_cast<uint8_t>(c.priority);
    d.release_time = c.release_time;
    StartBlock(block);

    return true;
}

void Channels::KillBrokenChannels(uint32_t dsp)
{
    // In CommonCallbackLogicalChannel: each channel that points at the DSP channel stops it, and goes into its
    // manager's free list.
    for (uint32_t channel = 0; channel < kChannels; channel++)
    {
        Channel& c = channels_[channel];
        if (c.dsp_channel != dsp)
        {
            continue;
        }

        StopLogical(channel);
        if (ListCut(channel) != -1)
        {
            ListAdd(c.manager, ChannelManager::kFree, channel);
        }
    }
}

int32_t Channels::Callback(uint32_t dsp, uint32_t event)
{
    // CommonCallbackLogicalChannel (0x275ae0 in main). It returns the frames to the next update, or -1 after a
    // forced stop. UpdateDspChannels raises only the update, the finish and the drop in priority. The model leaves out
    // the game's work for the other events: a break (event 3, from BreakLowerDSPchannel) and any other event.
    DspChannel& d = dsp_channels_[dsp];
    const uint32_t owner = d.channel;
    if (owner == kNone)
    {
        d.callback = false;
        d.priority = 0;
        KillBrokenChannels(dsp);
        return 0;
    }

    Channel& c = channels_[owner];
    if (c.dsp_channel != dsp)
    {
        // The DSP channel's owner has another DSP channel, or none.
        if (c.dsp_channel != kNone && dsp_channels_[c.dsp_channel].channel == owner)
        {
            KillBrokenChannels(dsp);
        }
        else
        {
            StopLogical(owner);
            if (ListCut(owner) != -1)
            {
                ListAdd(c.manager, ChannelManager::kFree, owner);
            }
        }
        d.priority = 0;
        d.channel = kNone;
        d.callback = false;
        return 0;
    }

    if (event == DspChannel::kFinish)
    {
        if (c.callback != Channel::kNoCallback)
        {
            RunCallback(owner, 1);
            return 0;
        }

        StopLogical(owner);
        if (ListCut(owner) != -1)
        {
            ListAdd(c.manager, ChannelManager::kFree, owner);
        }
        return 0;
    }

    // A wave whose load the game has undone (its scene closed) stops at once.
    if (c.wave != nullptr && banks_.RecordStatus(*c.wave) == 0)
    {
        ForceStopDsp(dsp);
        return -1;
    }

    if (event == DspChannel::kPriority)
    {
        d.priority = std::min(d.priority, static_cast<uint8_t>(c.priority >> 16));
        return 0;
    }

    return UpdateChannel(owner, dsp);
}

int32_t Channels::UpdateChannel(uint32_t channel, uint32_t dsp)
{
    // The update in CommonCallbackLogicalChannel: the oscillators, the levels, the sweep and the note's time.
    Channel& c = channels_[channel];
    c.pitch_modifier = 1.0f;
    c.volume_modifier = 1.0f;
    c.pans[Channel::kPan][Channel::kEffect] = 0.5f;
    c.pans[Channel::kFxmix][Channel::kEffect] = 0.0f;
    c.pans[Channel::kDolby][Channel::kEffect] = 0.0f;

    bool moving = false;
    for (std::size_t k = 0; k < c.oscillators.size(); k++)
    {
        const OscData* osc = c.oscillators[k];
        if (osc == nullptr)
        {
            continue;
        }

        const float value = OscToOfs(osc, c.osc_states[k]);
        DoEffectOsc(channel, osc->target, value);
        if (c.osc_states[k].state == OscState::kStop)
        {
            // The envelope has ended.
            if (c.callback != Channel::kNoCallback)
            {
                RunCallback(channel, 2);
                return 0;
            }

            if (c.dsp_channel != kNone)
            {
                StopLogical(channel);
            }
            else
            {
                Block(dsp_channels_[dsp].block).PlayStop();
            }
            if (ListCut(channel) != -1)
            {
                ListAdd(c.manager, ChannelManager::kFree, channel);
            }
            return 0;
        }
        moving = true;
    }

    if (moving)
    {
        UpdateEffecterParam(channel);
        c.flush = 1;
    }
    if (c.sweep != Channel::kNoSweep && SweepUpdate(channel))
    {
        c.flush++;
    }
    if (c.callback == Channel::kNoCallback)
    {
        return static_cast<int32_t>(kFrameStep);
    }

    if (c.countdown >= 1)
    {
        if (static_cast<uint32_t>(c.countdown) <= kFrameStep)
        {
            c.countdown = 0;
            RunCallback(channel, 0);
            c.countdown = c.time;
        }
        else
        {
            c.countdown -= static_cast<int32_t>(kFrameStep);
        }
    }
    else if (c.countdown == 0)
    {
        RunCallback(channel, 0);
        c.countdown = c.time;
    }

    if (c.flush != 0)
    {
        UpdateToDsp(channel);
        c.flush = 0;
    }

    return static_cast<int32_t>(kFrameStep);
}

bool Channels::ResetInitialVolume(uint32_t channel)
{
    // ResetInitialVolume (0x276350 in main).
    const Channel& c = channels_[channel];
    if (c.dsp_channel == kNone || (c.wave == nullptr && c.kind == Channel::kWave))
    {
        return false;
    }

    UpdateEffecterParam(channel);
    UpdateToDspInit(channel);

    return true;
}

// driverinterface: the queue of waiting channels.

bool Channels::AddWait(uint32_t channel)
{
    // Add_WaitDSPChannel (0x2763c0 in main).
    if (waits_.count == WaitQueue::kSize)
    {
        return false;
    }

    waits_.times[waits_.tail] = 0;
    waits_.channels[waits_.tail] = channel;
    waits_.count++;
    waits_.tail++;
    if (waits_.tail == WaitQueue::kSize)
    {
        waits_.tail = 0;
    }

    return true;
}

bool Channels::DeleteWait(uint32_t channel)
{
    // Del_WaitDSPChannel (0x276420 in main).
    for (uint32_t k = 0; k < waits_.count; k++)
    {
        const uint32_t slot = (waits_.top + k) & 0x3f;
        if (waits_.channels[slot] == channel)
        {
            waits_.channels[slot] = kNone;
            return true;
        }
    }

    return false;
}

void Channels::EntryWait()
{
    // __Entry_WaitChannel (0x276480 in main) as DSP_EntryCheck calls it: the queue's channels in order on the free DSP
    // channels, until the queue is empty or no DSP channel is free. A started channel's slot keeps its value. The game
    // can also stop after the first channel that starts. Nothing in the model asks for that.
    const auto advance = [this]
    {
        waits_.top = waits_.top + 1 == WaitQueue::kSize ? 0 : waits_.top + 1;
        waits_.count--;
    };

    while (waits_.count != 0)
    {
        const uint32_t channel = waits_.channels[waits_.top];
        if (channel == kNone)
        {
            advance();
            continue;
        }

        const uint32_t dsp = AllocDsp(channel);
        if (dsp == kNone)
        {
            return;
        }

        Channel& c = channels_[channel];
        c.dsp_channel = dsp;
        PlayLogical(channel);
        if (ListCut(channel) != -1)
        {
            ListAddTail(c.manager, ChannelManager::kActive, channel);
        }
        advance();
    }
}

void Channels::EntryCheck()
{
    // DSP_EntryCheck (0x276680 in main): the waiting channels start where they can. Then each channel that still
    // waits counts its frames. Its note's time runs down, and a note whose time is over leaves the queue.
    EntryWait();
    uint32_t count = waits_.count;
    uint32_t top = waits_.top;
    for (uint32_t k = 0; k < count; k++)
    {
        const uint32_t slot = (top + k) & 0x3f;
        const uint32_t channel = waits_.channels[slot];
        if (channel == kNone)
        {
            continue;
        }

        waits_.times[slot]++;
        Channel& c = channels_[channel];
        int32_t time = c.time;
        if (time >= 1)
        {
            time--;
            c.time = time;
        }
        if (time != 0)
        {
            continue;
        }

        RunCallback(channel, 6);
        top = waits_.top;
        waits_.channels[(k + top) & 0x3f] = kNone;
        count = waits_.count;
    }
}

bool Channels::ForceStopLogical(uint32_t channel)
{
    // ForceStopLogicalChannel (0x276740 in main).
    const Channel& c = channels_[channel];
    if (c.dsp_channel == kNone)
    {
        return false;
    }

    ForceStopDsp(c.dsp_channel);

    return true;
}

// oneshot.

uint32_t Channels::GetLogical(uint32_t manager, const WaveHandle* handle)
{
    // Get_LogicalChannel (0x279330 in main): a channel from the manager's free list, or one more from the global
    // manager. A manager that steals then also stops a releasing or a playing channel and moves it to its waiting
    // list. The channel that it takes keeps playing.
    if (handle != nullptr && banks_.HandleStart(*handle) == 0)
    {
        return kNone;
    }

    ChannelManager& m = managers_[manager];
    uint32_t channel = ListGet(manager, ChannelManager::kFree);
    if (channel == kNone)
    {
        if (FixAlloc(manager, 1) == 0)
        {
            return kNone;
        }

        m.allocations++;
        channel = ListGet(manager, ChannelManager::kFree);
        if (channel == kNone)
        {
            return kNone;
        }

        if (m.stealing)
        {
            uint32_t victim = ListGet(manager, ChannelManager::kReleasing);
            if (victim == kNone)
            {
                victim = ListGet(manager, ChannelManager::kActive);
            }
            if (victim != kNone)
            {
                Channel& v = channels_[victim];
                v.osc_states[0].state = OscState::kForceStop;
                ListAdd(manager, ChannelManager::kWaiting, victim);
                if (v.dsp_channel != kNone)
                {
                    ForceStopDsp(v.dsp_channel);
                }
            }
        }
    }

    InitChannel(channel);
    Channel& c = channels_[channel];
    if (handle != nullptr)
    {
        c.wave = banks_.Record(*handle);
        c.source = banks_.HandleStart(*handle);
        c.kind = Channel::kWave;
    }
    c.pans[Channel::kPower] = {kThird, kThird, kThird};

    return channel;
}

uint32_t Channels::PlayStart(uint32_t manager, uint32_t channel, int32_t time)
{
    // Play_1shot's start (0x279450 in main): the channel waits for a DSP channel, or goes back to the free list if
    // the queue is full or it has no wave.
    Channel& c = channels_[channel];
    c.dsp_channel = kNone;
    c.callback = Channel::kOneShot;
    c.time = time != 0 ? time : -1;
    c.countdown = c.time;
    if (CheckLogical(channel))
    {
        if (AddWait(channel))
        {
            ListAddTail(manager, ChannelManager::kWaiting, channel);
            return channel;
        }
    }

    ListAddTail(manager, ChannelManager::kFree, channel);

    return kNone;
}

uint32_t Channels::Play(uint32_t manager, uint32_t bank, uint32_t program, uint32_t key, uint32_t velocity,
                        int32_t time)
{
    const uint32_t sound = (bank & 0xff) << 24 | (program & 0xff) << 16 | (key & 0xff) << 8 | (velocity & 0xff);
    if ((program & 0xff) >= 0xf0)
    {
        return PlayOsc(manager, sound, time);
    }
    if ((program & 0xff) >= 0xe4)
    {
        return PlayPerc(manager, sound, time);
    }

    return PlayOneShot(manager, sound, time);
}

const WaveHandle* Channels::RegionWave(uint32_t place, const ntdlib::jaudio::VelocityRegion& region) const
{
    // WaveidToWavegroup and GetSoundHandle.
    const uint32_t system = banks_.BankSystem(place);
    return system != Banks::kNone ? banks_.FindSound(system, region.wave_id) : nullptr;
}

uint32_t Channels::PlayOneShot(uint32_t manager, uint32_t sound, int32_t time)
{
    // Play_1shot (0x278d10 in main).
    const uint32_t place = sound >> 24;
    const uint32_t program = (sound >> 16) & 0xff;
    const uint32_t key = (sound >> 8) & 0xff;
    const uint32_t velocity = sound & 0xff;

    // Bank_Get, InstChange, GetInstVmap, WaveidToWavegroup and GetSoundHandle.
    const Instrument* inst = banks_.FindInstrument(place, program);
    if (inst == nullptr)
    {
        return kNone;
    }

    std::size_t keymap = 0;
    while (keymap < inst->keys.size() && inst->keys[keymap].high_key < key)
    {
        keymap++;
    }
    if (keymap == inst->keys.size())
    {
        return kNone;
    }

    const ntdlib::jaudio::VelocityRegion* region = RegionFor(inst->keys[keymap].velocities, velocity);
    if (region == nullptr)
    {
        return kNone;
    }

    const WaveHandle* handle = RegionWave(place, *region);
    if (handle == nullptr)
    {
        return kNone;
    }

    const uint32_t channel = GetLogical(manager, handle);
    if (channel == kNone)
    {
        return kNone;
    }

    Channel& c = channels_[channel];
    const WaveRecord& record = *c.wave;
    const int32_t k = ClampKey(static_cast<int32_t>(key) - record.info.base_key + 60);
    c.velocity = static_cast<uint8_t>(velocity);
    c.key = static_cast<uint8_t>(key);
    c.base_pitch = ((record.info.sample_rate / kDacRate) * region->pitch) * inst->pitch;
    c.pitch = KeyPitch(static_cast<uint32_t>(k)) * c.base_pitch;
    c.base_volume = region->volume * inst->volume;
    c.volume = VelocityVolume(velocity) * c.base_volume;
    c.pans[Channel::kPan][Channel::kSound] = 0.5f;
    c.pans[Channel::kPan][Channel::kEffect] = 0.5f;
    c.pans[Channel::kFxmix][Channel::kSound] = 0.0f;
    c.pans[Channel::kFxmix][Channel::kEffect] = 0.0f;
    c.pans[Channel::kDolby][Channel::kSound] = 0.0f;
    c.pans[Channel::kDolby][Channel::kEffect] = 0.0f;
    c.pitch_modifier = 1.0f;
    c.volume_modifier = 1.0f;

    // The effects in the game's order: for each slot, the effect that follows the key or velocity, the random one and
    // the oscillator. The first slot's key or velocity comes from the sound, the second's from the channel.
    for (std::size_t slot = 0; slot < 2; slot++)
    {
        const InstEffect* sense = EffectAt(inst->effects, InstEffect::Kind::kSense, slot);
        if (sense != nullptr)
        {
            uint32_t value = 0;
            if (sense->trigger == 1)
            {
                value = slot == 0 ? sound : c.velocity;
            }
            else if (sense->trigger == 2)
            {
                value = slot == 0 ? sound >> 8 : c.key;
            }
            DoEffect(channel, sense->target, SenseToOfs(sense, value));
        }

        const InstEffect* random = EffectAt(inst->effects, InstEffect::Kind::kRandom, slot);
        if (random != nullptr)
        {
            DoEffect(channel, random->target, RandToOfs(random, random_));
        }

        if (slot < inst->oscillators.size())
        {
            const OscData* osc = banks_.Oscillator(place, inst->oscillators[slot]);
            c.osc_states[slot].state = OscState::kStart;
            c.oscillators[slot] = osc;
            const float value = OscToOfs(osc, c.osc_states[slot]);
            if (osc != nullptr)
            {
                DoEffectOsc(channel, osc->target, value);
            }
        }
        else
        {
            c.oscillators[slot] = nullptr;
        }
    }

    c.pans[Channel::kPan][Channel::kSound] = ClampUnit(c.pans[Channel::kPan][Channel::kSound]);
    c.pans[Channel::kFxmix][Channel::kSound] = ClampUnit(c.pans[Channel::kFxmix][Channel::kSound]);

    // The sound's group for the limit on its voices: the flags in the top byte, the bank's place and the program,
    // and by the flags' bits 6 and 7 the key region, all keys, or all of the bank's sounds.
    const uint8_t flags = banks_.InstrumentFlags(place, program);
    uint32_t group = uint32_t{flags} << 24 | place << 8 | program;
    switch (flags & 0xc0)
    {
    case 0x40:
        group |= static_cast<uint32_t>(keymap) << 16;
        break;

    case 0x80:
        group |= 0xff;
        break;

    case 0xc0:
        group = uint32_t{flags} << 24 | 0xffffff;
        break;

    default:
        break;
    }

    c.sound = 0;
    const uint32_t polys_class = (group >> 24) & 0xf;
    if (polys_class == 0)
    {
        c.sound = group;
        return PlayStart(manager, channel, time);
    }

    const uint32_t polys = kPolys[polys_class];
    const ChannelManager& m = managers_[c.manager];
    if ((group & 0x20000000) != 0)
    {
        // A count of the group's voices. Past the limit, the note either takes no channel (bit 4) or starts paused.
        uint32_t count = 0;
        for (const uint8_t list : {ChannelManager::kActive, ChannelManager::kReleasing})
        {
            for (uint32_t o = m.lists[list]; o != kNone; o = channels_[o].next)
            {
                count += channels_[o].sound == group ? 1 : 0;
            }
        }
        c.voices = static_cast<uint8_t>(count);
        if (count >= polys)
        {
            if ((flags & 0x10) != 0)
            {
                ListAddTail(manager, ChannelManager::kFree, channel);
                return kNone;
            }

            c.sound = group;
            const uint32_t started = PlayStart(manager, channel, time);
            if (started != kNone)
            {
                channels_[started].pause = true;
                channels_[started].flush = 1;
            }
            return started;
        }

        c.sound = group;
        return PlayStart(manager, channel, time);
    }

    // Each voice of the group counts one more before it. The one that reaches the limit stops (bit 28) or pauses,
    // and a releasing one stops.
    for (uint32_t o = m.lists[ChannelManager::kActive]; o != kNone; o = channels_[o].next)
    {
        Channel& other = channels_[o];
        if (other.sound != group)
        {
            continue;
        }

        other.voices++;
        if (polys == other.voices)
        {
            if ((group & 0x10000000) != 0)
            {
                ForceStopLogical(o);
            }
            else
            {
                other.pause = true;
                other.flush = 1;
            }
        }
    }
    for (uint32_t o = m.lists[ChannelManager::kReleasing]; o != kNone; o = channels_[o].next)
    {
        Channel& other = channels_[o];
        if (other.sound != group)
        {
            continue;
        }

        other.voices++;
        if (polys == other.voices)
        {
            ForceStopLogical(o);
        }
    }

    c.voices = 0;
    c.sound = group;

    return PlayStart(manager, channel, time);
}

uint32_t Channels::PlayPerc(uint32_t manager, uint32_t sound, int32_t time)
{
    // Play_1shot_Perc (0x279500 in main). The game reads past a drum set's 128 keys for a higher key. The model
    // starts no note there.
    const uint32_t place = sound >> 24;
    const uint32_t program = (sound >> 16) & 0xff;
    const uint32_t key = (sound >> 8) & 0xff;
    const uint32_t velocity = sound & 0xff;
    const ntdlib::jaudio::DrumSet* drums = banks_.FindDrums(place, program);
    if (drums == nullptr || key >= drums->keys.size() || !drums->keys[key])
    {
        return kNone;
    }

    const Drum& drum = *drums->keys[key];
    const ntdlib::jaudio::VelocityRegion* region = RegionFor(drum.velocities, velocity);
    if (region == nullptr)
    {
        return kNone;
    }

    const WaveHandle* handle = RegionWave(place, *region);
    if (handle == nullptr)
    {
        return kNone;
    }

    const uint32_t channel = GetLogical(manager, handle);
    if (channel == kNone)
    {
        return kNone;
    }

    // A drum plays at its wave's rate, with no key's pitch. A drum set of the second kind (PER2) gives each key a
    // pan and a release. The first kind plays in the middle with a release of 1,000.
    Channel& c = channels_[channel];
    const WaveRecord& record = *c.wave;
    c.velocity = static_cast<uint8_t>(velocity);
    c.key = static_cast<uint8_t>(key);
    c.base_pitch = ((record.info.sample_rate / kDacRate) * region->pitch) * drum.pitch;
    c.pitch = c.base_pitch;
    c.base_volume = region->volume * drum.volume;
    c.volume = VelocityVolume(velocity) * c.base_volume;
    c.pans[Channel::kPan][Channel::kSound] = drum.pan;
    c.pans[Channel::kFxmix][Channel::kSound] = 0.0f;
    c.pans[Channel::kDolby][Channel::kSound] = 0.0f;
    c.pitch_modifier = 1.0f;
    c.volume_modifier = 1.0f;
    c.pans[Channel::kPan][Channel::kEffect] = 0.5f;
    c.pans[Channel::kFxmix][Channel::kEffect] = 0.0f;
    c.pans[Channel::kDolby][Channel::kEffect] = 0.0f;

    const InstEffect* first = EffectAt(drum.effects, InstEffect::Kind::kRandom, 0);
    if (first != nullptr)
    {
        DoEffect(channel, first->target, RandToOfs(first, random_));
    }
    c.oscillators[0] = nullptr;
    const InstEffect* second = EffectAt(drum.effects, InstEffect::Kind::kRandom, 1);
    if (second != nullptr)
    {
        DoEffect(channel, second->target, RandToOfs(second, random_));
    }

    c.osc_states[0].state = OscState::kStart;
    c.oscillators[0] = &kPercEnvelope;
    c.oscillators[1] = nullptr;
    OscToOfs(&kPercEnvelope, c.osc_states[0]);
    c.osc_states[0].direct_release = drum.release;

    c.pans[Channel::kPan][Channel::kSound] = ClampUnit(c.pans[Channel::kPan][Channel::kSound]);
    c.pans[Channel::kFxmix][Channel::kSound] = ClampUnit(c.pans[Channel::kFxmix][Channel::kSound]);
    c.sound = 0;

    return PlayStart(manager, channel, time);
}

uint32_t Channels::PlayOsc(uint32_t manager, uint32_t sound, int32_t time)
{
    // Play_1shot_Osc (0x279810 in main): one of the DSP's oscillators by the program's distance from 0xf0.
    const uint32_t channel = GetLogical(manager, nullptr);
    if (channel == kNone)
    {
        return kNone;
    }

    const uint32_t key = (sound >> 8) & 0xff;
    Channel& c = channels_[channel];
    c.kind = Channel::kOscillator;
    c.source = ((sound >> 16) & 0xff) - 0xf0;
    c.velocity = static_cast<uint8_t>(sound);
    c.key = static_cast<uint8_t>(key);
    c.base_pitch = kOscillatorPitch / kDacRate;
    c.pitch = KeyPitch(std::min(key, 0x7fu)) * c.base_pitch;
    c.base_volume = 1.0f;
    c.volume = VelocityVolume(sound);
    c.pans[Channel::kPan][Channel::kSound] = 0.5f;
    c.pans[Channel::kPan][Channel::kEffect] = 0.5f;
    c.pans[Channel::kFxmix][Channel::kSound] = 0.0f;
    c.pans[Channel::kFxmix][Channel::kEffect] = 0.0f;
    c.pans[Channel::kDolby][Channel::kSound] = 0.0f;
    c.pans[Channel::kDolby][Channel::kEffect] = 0.0f;
    c.pitch_modifier = 1.0f;
    c.volume_modifier = 1.0f;
    c.oscillators[0] = &kOscEnvelope;
    c.oscillators[1] = nullptr;
    c.osc_states[0].state = OscState::kStart;
    OscToOfs(&kOscEnvelope, c.osc_states[0]);
    c.sound = 0;

    return PlayStart(manager, channel, time);
}

void Channels::StopBody(uint32_t channel)
{
    // The release in Stop_1Shot (0x278410 in main): the first two oscillators release unless they already stop. A
    // channel whose oscillators release moves to the releasing list, and its DSP channel drops to the release's
    // priority. The note then has no time.
    Channel& c = channels_[channel];
    bool released = false;
    for (std::size_t k = 0; k < 2; k++)
    {
        if (c.oscillators[k] != nullptr && (c.osc_states[k].state & 0xfe) != OscState::kForceStop)
        {
            c.osc_states[k].state = OscState::kRelease;
            released = true;
        }
    }

    if (released && ListCut(channel) != -1)
    {
        ListAddTail(c.manager, ChannelManager::kReleasing, channel);
        if (c.dsp_channel != kNone)
        {
            dsp_channels_[c.dsp_channel].priority =
                (c.priority & 0xff00) != 0 ? static_cast<uint8_t>(c.priority >> 8) : uint8_t{1};
        }
    }
    c.time = -1;
}

void Channels::Stop(uint32_t channel)
{
    // Stop_1Shot (0x278410 in main). A channel that waits for a DSP channel leaves the queue.
    if (channels_[channel].dsp_channel == kNone)
    {
        UpdateOneShot(channel, 6);
        return;
    }

    StopBody(channel);
}

void Channels::StopRelease(uint32_t channel, uint16_t release)
{
    // Stop_1Shot_R (0x2787f0 in main).
    if (channels_[channel].dsp_channel == kNone)
    {
        UpdateOneShot(channel, 6);
        return;
    }

    channels_[channel].osc_states[0].direct_release = release;
    StopBody(channel);
}

void Channels::AllStop(uint32_t manager)
{
    // AllStop_1Shot (0x2788e0 in main).
    for (uint32_t channel = managers_[manager].lists[ChannelManager::kActive]; channel != kNone;)
    {
        const uint32_t next = channels_[channel].next;
        if (channels_[channel].dsp_channel == kNone)
        {
            UpdateOneShot(channel, 6);
        }
        else
        {
            StopBody(channel);
        }
        channel = next;
    }
}

void Channels::FlushRelease(uint32_t manager)
{
    // FlushRelease_1Shot (0x278bd0 in main): each releasing channel's first two oscillators stop at once, and the
    // channel moves to the list's end.
    uint32_t count = ListCount(manager, ChannelManager::kReleasing);
    while (count != 0)
    {
        const uint32_t channel = ListGet(manager, ChannelManager::kReleasing);
        if (channel == kNone)
        {
            return;
        }

        Channel& c = channels_[channel];
        for (std::size_t k = 0; k < 2; k++)
        {
            if (c.oscillators[k] != nullptr && (c.osc_states[k].state & 0xfe) != OscState::kForceStop)
            {
                c.osc_states[k].state = OscState::kForceStop;
            }
        }
        ListAddTail(manager, ChannelManager::kReleasing, channel);
        count--;
    }
}

void Channels::StopVoices(uint32_t channel)
{
    // StopMonoPolyCheck (in Jesus1Shot_Update): the group's later voices count one fewer. One that drops below the
    // limit, or to one below it in counting mode (bit 29), plays on.
    Channel& c = channels_[channel];
    const uint32_t group = c.sound;
    c.sound = 0;
    const uint32_t polys_class = (group >> 24) & 0xf;
    if (group == 0 || polys_class == 0)
    {
        return;
    }

    const uint32_t polys = kPolys[polys_class];
    const bool counting = (group & 0x20000000) != 0;
    for (uint32_t o = managers_[c.manager].lists[ChannelManager::kActive]; o != kNone; o = channels_[o].next)
    {
        Channel& other = channels_[o];
        if (other.sound != group || other.voices <= c.voices)
        {
            continue;
        }

        other.voices--;
        if (counting ? polys - 1 == other.voices : polys > other.voices)
        {
            other.pause = false;
            other.flush = 1;
        }
    }
}

void Channels::UpdateOneShot(uint32_t channel, uint32_t status)
{
    // Jesus1Shot_Update (0x278500 in main). Status 1 (the wave's end), 2 (the envelope's end) and 6 (a stop while
    // the channel waits): the channel goes back to the free list, or to the global manager's for a channel that the
    // manager took for a note. Status 0 (the note's time is over): the release.
    Channel& c = channels_[channel];
    if (status == 1 || status == 2 || status == 6)
    {
        const uint32_t allocations = managers_[c.manager].allocations;
        if (ListCut(channel) != -1)
        {
            if (allocations != 0)
            {
                managers_[c.manager].allocations--;
                StopVoices(channel);
                FixRelease(channel);
            }
            else
            {
                StopVoices(channel);
                ListAdd(c.manager, ChannelManager::kFree, channel);
            }
        }

        if (status == 6)
        {
            DeleteWait(channel);
        }
        else
        {
            StopLogical(channel);
        }
        c.key = 0xff;
        c.time = -1;
        c.callback = Channel::kNoCallback;
    }
    else if (status == 0)
    {
        StopBody(channel);
    }
}

void Channels::RunCallback(uint32_t channel, uint32_t status)
{
    if (channels_[channel].callback == Channel::kOneShot)
    {
        UpdateOneShot(channel, status);
    }
}

bool Channels::SweepUpdate(uint32_t channel)
{
    // Extra_Update (0x278a10 in main): a step of the sweep to the target pitch.
    Channel& c = channels_[channel];
    if (c.sweep_steps == 0)
    {
        return false;
    }

    const float distance = c.target_pitch - c.pitch;
    if (kFrameStep >= c.sweep_steps)
    {
        c.sweep_steps = 1;
    }
    const auto steps = static_cast<float>(c.sweep_steps);
    c.sweep_steps = static_cast<uint16_t>(c.sweep_steps - kFrameStep);
    c.pitch = c.pitch + distance / steps;
    if (c.sweep_steps == 0)
    {
        c.sweep = Channel::kNoSweep;
    }

    return true;
}

void Channels::SetKeyTarget(uint32_t channel, uint32_t key, uint32_t steps)
{
    // SetKeyTarget_1Shot (0x278a80 in main).
    Channel& c = channels_[channel];
    int32_t k = static_cast<int32_t>(key & 0xff);
    if (c.kind != Channel::kOscillator && c.wave != nullptr)
    {
        k = k - c.wave->info.base_key + 60;
    }

    const float pitch = KeyPitch(static_cast<uint32_t>(ClampKey(k))) * c.base_pitch;
    if (steps != 0)
    {
        c.target_pitch = pitch;
        c.sweep_steps = static_cast<uint16_t>(steps);
        c.sweep = Channel::kPitchSweep;
    }
    else
    {
        c.pitch = pitch;
        c.sweep = Channel::kNoSweep;
    }
}

void Channels::Gate(uint32_t channel, uint32_t key, uint32_t velocity, int32_t time)
{
    // Gate_1Shot (0x278b00 in main). The game reads a wave channel's record without a check. The model takes the key
    // as it is for a channel without one.
    Channel& c = channels_[channel];
    if (c.time != -1)
    {
        return;
    }

    c.time = time;
    c.countdown = time;
    int32_t k = static_cast<int32_t>(key & 0xff);
    if (c.kind != Channel::kOscillator && c.wave != nullptr)
    {
        k = k - c.wave->info.base_key + 60;
    }

    c.key = static_cast<uint8_t>(key);
    c.pitch = KeyPitch(static_cast<uint32_t>(ClampKey(k))) * c.base_pitch;
    c.velocity = static_cast<uint8_t>(velocity);
    c.volume = VelocityVolume(velocity) * c.base_volume;
}

void Channels::SetPanPower(uint32_t channel, float a, float b, float c)
{
    // UpdatePanPower_1Shot (0x278ba0 in main).
    const float sum = (a + b) + c;
    if (sum == 0.0f)
    {
        return;
    }

    channels_[channel].pans[Channel::kPower] = {a / sum, b / sum, c / sum};
}

void Channels::OverwriteOscillator(uint32_t channel, const OscData* data, uint32_t slot)
{
    // Effecter_Overwrite_1ShotD (0x278360 in main). The game reads the new oscillator's target without a check.
    if (slot > 3)
    {
        return;
    }

    Channel& c = channels_[channel];
    c.osc_states[slot].state = OscState::kStart;
    c.oscillators[slot] = data;
    const float value = OscToOfs(data, c.osc_states[slot]);
    if (data != nullptr)
    {
        DoEffectOsc(channel, data->target, value);
    }
}

uint32_t Channels::CheckInstWave(uint32_t bank, uint32_t program, uint32_t key, uint32_t velocity) const
{
    // One_CheckInstWave (0x278c70 in main). InstChange takes a drum set's program as an instrument. A drum set holds
    // no key regions there. Its program therefore gives 2. A program past the drum sets gives 1: the bank file holds no
    // instrument there up to 0xf2, and the game reads other data above.
    if (banks_.FindDrums(bank, program) != nullptr)
    {
        return 2;
    }

    const Instrument* inst = banks_.FindInstrument(bank, program);
    if (inst == nullptr)
    {
        return 1;
    }

    const ntdlib::jaudio::VelocityRegion* region = nullptr;
    for (const ntdlib::jaudio::KeyRegion& keymap : inst->keys)
    {
        if (keymap.high_key >= (key & 0xff))
        {
            region = RegionFor(keymap.velocities, velocity & 0xff);
            break;
        }
    }
    if (region == nullptr)
    {
        return 2;
    }

    const uint32_t system = banks_.BankSystem(bank);
    if (system == Banks::kNone)
    {
        return 3;
    }

    return banks_.FindSound(system, region->wave_id) != nullptr ? 0 : 4;
}

} // namespace hsf::jac
