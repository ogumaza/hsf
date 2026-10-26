// SPDX-License-Identifier: MIT

// The channels of the C JAudio as the Switch release of Pikmin builds them for AArch64: the 256 channels that play
// the notes, the channel managers that hold them, and the 64 DSP channels that the DSP renders. Each track has a
// manager, and the global manager holds the channels that no track holds. A manager keeps its channels in four lists:
// free, playing, releasing and waiting for a DSP channel. A note takes a channel from its track's manager, and a DSP
// channel or a place in the queue of waiting channels (oneshot's Play_1shot functions, driverinterface and
// dspdriver).
//
// Each frame, UpdateDSPchannelAll starts waiting channels on the DSP channels that are free, then updates every DSP
// channel. The callback of a DSP channel's channel (CommonCallbackLogicalChannel) then moves the channel's
// oscillators, works out its mixers' volumes and its pitch, and writes them into the DSP channel's block
// (dspinterface's DSP_ functions).
//
// The channels, the managers and the DSP channels refer to one another by index. kNone stands for the game's null
// pointer.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "jac/bank.h"
#include "jac/calc.h"
#include "jac/oscillator.h"
#include "jaudio/channel.h"
#include "jaudio/dsp.h"

namespace hsf::jac
{

using jaudio::DspBlock;
using ntdlib::jaudio::Drum;
using ntdlib::jaudio::InstEffect;
using ntdlib::jaudio::Instrument;

constexpr uint32_t kNone = 0xffffffff;

// A channel manager (InitJcs's fields): the channels that it holds in its lists, the levels that its track gives
// them, and the settings of their DSP channels' mixers and filters.
struct ChannelManager
{
    // The lists.
    enum List : uint8_t
    {
        kFree = 0,
        kActive = 1,
        kReleasing = 2,
        kWaiting = 3,
        kLists = 4,
    };

    uint32_t count = 0;       // the channels that the manager holds
    uint32_t allocations = 0; // the channels that it took from the global manager for its notes
    std::array<uint32_t, kLists> lists{kNone, kNone, kNone, kNone}; // each list's first channel

    float volume = 1.0f;
    float pitch = 1.0f;
    float pan = 0.5f;
    float fxmix = 0.0f;
    float dolby = 0.0f;
    std::array<int16_t, 8> fir{0x7fff, 0, 0, 0, 0, 0, 0, 0};
    std::array<int16_t, 4> iir{0x7fff, 0, 0, 0};
    int16_t distortion = 0;
    std::array<uint16_t, 6> mix_configs{0x150, 0x210, 0x352, 0x412, 0, 0};
    std::array<uint8_t, 6> delays{}; // each mixer's delay in samples. InitJcs clears all six and the maximum.
    uint8_t delay_max = 0;
    uint8_t filter_mode = 0; // the FIR filter's taps in the low 5 bits, the IIR filter's switch in bit 5
    std::array<uint8_t, 3> pan_calcs{26, 1, 1}; // how the pan, the effect send and the surround level add up
    uint32_t priority = 0x20103; // the channels' priority: while they play in the low byte, from the release in the
                                 // next, while they wait for a DSP channel in the third
    uint16_t release_time = 600; // frames from a DSP channel's start to its first drop in priority
    bool stealing = true;        // a note may take a channel that plays or releases when the manager has none free
};

// A channel (a logical channel), by the game's fields. A field that InitGlobalChannel leaves alone starts at 0. The
// game's memory does the same.
struct Channel
{
    // The wave's kinds.
    static constexpr uint8_t kWave = 0;
    static constexpr uint8_t kOscillator = 2;

    // The update callback: none, or the notes' callback (Jesus1Shot_Update).
    enum Callback : uint8_t
    {
        kNoCallback = 0,
        kOneShot = 1,
    };

    // The sweep's callback: none, or the move to the target pitch (Extra_Update).
    enum Sweep : uint8_t
    {
        kNoSweep = 0,
        kPitchSweep = 1,
    };

    // The sources of a pan matrix's three values, and the matrices.
    static constexpr std::size_t kSound = 0;
    static constexpr std::size_t kEffect = 1;
    static constexpr std::size_t kManager = 2;
    static constexpr std::size_t kPower = 0; // the matrix of the sources' weights
    static constexpr std::size_t kPan = 1;
    static constexpr std::size_t kFxmix = 2;
    static constexpr std::size_t kDolby = 3;

    uint8_t velocity = 0;
    uint8_t key = 0; // 0xff once the note has stopped
    bool pause = false;
    uint8_t flush = 0; // the block takes the channel's levels at the next update
    uint32_t manager = kNone;
    uint32_t list_manager = kNone; // the list that holds the channel: its manager and the list, or kNone
    uint8_t list = ChannelManager::kFree;
    uint8_t kind = kWave;
    const WaveRecord* wave = nullptr; // the wave's record (waveData), or null
    uint32_t source = 0;              // the start of the wave's heap in ARAM, or the DSP oscillator's waveform
    uint32_t dsp_channel = kNone;
    uint32_t next = kNone; // the next channel of the list
    uint8_t callback = kNoCallback;
    uint8_t sweep = kNoSweep;
    int32_t time = 0;      // the note's time in frames, or -1 for none
    int32_t countdown = 0; // the frames left of the note's time
    std::array<const OscData*, 4> oscillators{};
    std::array<OscState, 4> osc_states{};
    float base_pitch = 0.0f;
    float base_volume = 0.0f;
    float pitch = 0.0f;
    float volume = 0.0f;
    std::array<uint8_t, 3> pan_calcs{};
    std::array<std::array<float, 3>, 4> pans{}; // by matrix, then by source
    float pitch_modifier = 0.0f;
    float volume_modifier = 0.0f;
    float target_pitch = 0.0f;
    uint16_t final_pitch = 0; // the pitch for the block, in 4.12
    uint16_t sweep_steps = 0;
    uint32_t last_manager = kNone; // the manager whose levels the channel took last
    float manager_pitch = 0.0f;
    float manager_volume = 0.0f;
    std::array<uint16_t, 6> mix_configs{};
    std::array<uint16_t, 6> mixer_levels{};
    uint32_t priority = 0;
    uint16_t release_time = 0;
    uint16_t id = 0;    // counts the channel's uses, from 1. A track checks a channel's id against its notes'.
    uint32_t sound = 0; // the note's group for the limits on its voices, or 0
    uint8_t voices = 0; // the voices of the group that play before this one (polyphonyCounter)

    // The channel moved to the global manager or to another track's while it played or released
    // (FixReleaseChannelAll and FixMoveChannelAll). UpdateJcToDSP then writes only the mixers' targets and the pitch.
    bool moved = false;
};

// A DSP channel (one of dspdriver's 64), by the game's fields.
struct DspChannel
{
    enum State : uint8_t
    {
        kFree = 0,
        kMono = 1,
        kStereoLeft = 2,
        kStereoRight = 3,
        kStopping = 4,
    };

    // The events of the channel's callback.
    enum Event : uint32_t
    {
        kUpdate = 0,
        kFinish = 2,
        kPriority = 4,
    };

    uint8_t block = 0; // the block's number
    uint8_t state = kFree;
    uint8_t priority = 0;
    uint16_t release_time = 0;
    uint16_t timer = 0;       // frames to the callback's next update
    uint32_t channel = kNone; // the channel that owns the DSP channel
    bool callback = false;    // whether the owner's callback (CommonCallbackLogicalChannel) is set
};

// The queue of channels that wait for a DSP channel (driverinterface's waitp and waittime). An empty slot holds
// kNone.
struct WaitQueue
{
    static constexpr std::size_t kSize = 64;

    uint32_t count = 0;
    uint32_t top = 0;
    uint32_t tail = 0;
    std::array<uint32_t, kSize> channels{};
    std::array<uint32_t, kSize> times{}; // the frames that each slot's channel has waited
};

// The channels, their managers and the DSP channels.
class Channels
{
public:
    static constexpr std::size_t kChannels = 256;
    static constexpr std::size_t kDspChannels = 64;

    // The managers by index: one for each track by the track's index (the pool's 256, then the 16 root tracks), then
    // the global manager.
    static constexpr std::size_t kTrackManagers = 272;
    static constexpr uint32_t kGlobal = kTrackManagers;

    // The DSP channels' blocks start at `blocks` in `memory`, one every 0x180 bytes. `banks` gives the notes their
    // instruments and waves, and `random` the instruments' random effects. All three must stay in place. The channels
    // start as DSP_InitBuffer leaves them: the blocks clear, then InitDSPchannel and InitGlobalChannel, with every
    // channel in the global manager's free list.
    Channels(DspMemory& memory, uint32_t blocks, const Banks& banks, Random& random);

    Channels(const Channels&) = delete;
    Channels& operator=(const Channels&) = delete;

    // Channel_SetMixerLevel: a mixer's level at full volume, `level` × 32,767.
    void SetMixerLevel(float level);

    Channel& At(uint32_t index)
    {
        return channels_[index];
    }

    const Channel& At(uint32_t index) const
    {
        return channels_[index];
    }

    ChannelManager& Manager(uint32_t index)
    {
        return managers_[index];
    }

    const ChannelManager& Manager(uint32_t index) const
    {
        return managers_[index];
    }

    DspChannel& Dsp(uint32_t index)
    {
        return dsp_channels_[index];
    }

    const DspChannel& Dsp(uint32_t index) const
    {
        return dsp_channels_[index];
    }

    const WaitQueue& Waits() const
    {
        return waits_;
    }

    DspBlock Block(uint32_t block) const
    {
        return DspBlock(memory_, blocks_ + block * 0x180);
    }

    // The managers (InitJcs, Init_1shot, FixAllocChannel, FixReleaseChannel, FixReleaseChannelAll and
    // FixMoveChannelAll). Init_1shot gives a manager `count` channels, and turns its stealing on when `count` isn't 0.
    void InitManager(uint32_t manager);
    void InitOneShot(uint32_t manager, uint32_t count);
    uint32_t FixAlloc(uint32_t manager, uint32_t count);
    void FixRelease(uint32_t channel);
    void FixReleaseAll(uint32_t manager);
    void FixMoveAll(uint32_t from, uint32_t to);

    // The lists (List_CountChannel, List_CutChannel, List_GetChannel, List_AddChannelTail and List_AddChannel).
    // ListCut gives the channel's place in its list, or -1 when no list holds it.
    uint32_t ListCount(uint32_t manager, uint8_t list) const;
    int32_t ListCut(uint32_t channel);
    uint32_t ListGet(uint32_t manager, uint8_t list);
    void ListAddTail(uint32_t manager, uint8_t list, uint32_t channel);
    void ListAdd(uint32_t manager, uint8_t list, uint32_t channel);

    // A note on the bank at place `bank` in the bank file, program `program` (Play_1shot from 0 to 0xe3,
    // Play_1shot_Perc from 0xe4 to 0xef, and Play_1shot_Osc from 0xf0), with its manager's channels. `time` is the
    // note's time in frames, or 0 for none. The channel then holds -1. Returns the channel, or kNone.
    uint32_t Play(uint32_t manager, uint32_t bank, uint32_t program, uint32_t key, uint32_t velocity, int32_t time);

    // Stop_1Shot, Stop_1Shot_R (a release over `release`), AllStop_1Shot and FlushRelease_1Shot.
    void Stop(uint32_t channel);
    void StopRelease(uint32_t channel, uint16_t release);
    void AllStop(uint32_t manager);
    void FlushRelease(uint32_t manager);

    // Gate_1Shot: a tied note's key, velocity and time, for a channel whose note has no time left.
    void Gate(uint32_t channel, uint32_t key, uint32_t velocity, int32_t time);

    // SetKeyTarget_1Shot: a sweep to `key` over `steps` frames.
    void SetKeyTarget(uint32_t channel, uint32_t key, uint32_t steps);

    // UpdatePause_1Shot and UpdatePanPower_1Shot.
    void SetPause(uint32_t channel, bool pause)
    {
        channels_[channel].pause = pause;
    }

    void SetPanPower(uint32_t channel, float a, float b, float c);

    // Effecter_Overwrite_1ShotD: oscillator `slot` (0 to 3) of the channel becomes `data`, and starts.
    void OverwriteOscillator(uint32_t channel, const OscData* data, uint32_t slot);

    // ResetInitialVolume: the channel's levels as its DSP channel's start volumes. False without a DSP channel or a
    // wave.
    bool ResetInitialVolume(uint32_t channel);

    // One_CheckInstWave: 0 if program `program` of the bank at place `bank` has a wave for the key and velocity that
    // the scenes hold, or what's missing: 1 the program, 2 the region, 3 the wave's group, 4 the wave.
    uint32_t CheckInstWave(uint32_t bank, uint32_t program, uint32_t key, uint32_t velocity) const;

    // UpdateDSPchannelAll: the start of waiting channels on the DSP channels that are free (DSP_EntryCheck), then
    // each DSP channel's frame. Its sub-frame index is 0 on the Switch. The model leaves out the check of the DSP's
    // time. There the game drops channels by priority when the DSP falls behind.
    void UpdateDspChannels();

private:
    // dspdriver.
    void InitDspChannels();
    uint32_t AllocDsp(uint32_t channel);
    int32_t DeallocDsp(uint32_t dsp, uint32_t channel);
    bool ForceStopDsp(uint32_t dsp);

    // driverinterface.
    void InitGlobalChannels();
    void FixMove(uint32_t from, uint32_t to);
    void InitChannel(uint32_t channel);
    float PanCalc(const Channel& c, std::size_t matrix, uint8_t type) const;
    void UpdateEffecterParam(uint32_t channel);
    void UpdateToDsp(uint32_t channel);
    void UpdateToDspInit(uint32_t channel);
    void DoEffectOsc(uint32_t channel, uint8_t target, float value);
    void KillBrokenChannels(uint32_t dsp);
    int32_t Callback(uint32_t dsp, uint32_t event);
    int32_t UpdateChannel(uint32_t channel, uint32_t dsp);
    bool StopLogical(uint32_t channel);
    bool CheckLogical(uint32_t channel) const;
    bool PlayLogical(uint32_t channel);
    bool AddWait(uint32_t channel);
    bool DeleteWait(uint32_t channel);
    void EntryWait();
    void EntryCheck();
    bool ForceStopLogical(uint32_t channel);

    // oneshot. RegionWave gives the handle of a velocity region's wave in the current scenes of the wave system that
    // the bank at place `place` plays, or null.
    const WaveHandle* RegionWave(uint32_t place, const ntdlib::jaudio::VelocityRegion& region) const;
    uint32_t GetLogical(uint32_t manager, const WaveHandle* handle);
    uint32_t PlayStart(uint32_t manager, uint32_t channel, int32_t time);
    uint32_t PlayOneShot(uint32_t manager, uint32_t sound, int32_t time);
    uint32_t PlayPerc(uint32_t manager, uint32_t sound, int32_t time);
    uint32_t PlayOsc(uint32_t manager, uint32_t sound, int32_t time);
    void StopBody(uint32_t channel);
    void StopVoices(uint32_t channel);
    void UpdateOneShot(uint32_t channel, uint32_t status);
    void RunCallback(uint32_t channel, uint32_t status);
    bool SweepUpdate(uint32_t channel);
    void DoEffect(uint32_t channel, uint8_t target, float value);

    DspMemory& memory_;
    uint32_t blocks_;
    const Banks& banks_;
    Random& random_;
    uint32_t output_mode_ = 1; // JAC_SYSTEM_OUTPUT_MODE: 0 mono, 1 stereo. The model plays in stereo.
    uint16_t mixer_level_ = 12000;
    std::array<Channel, kChannels> channels_{};
    std::array<ChannelManager, kTrackManagers + 1> managers_{};
    std::array<DspChannel, kDspChannels> dsp_channels_{};
    WaitQueue waits_;
};

} // namespace hsf::jac
