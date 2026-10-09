// SPDX-License-Identifier: MIT

// nn::atk's channels (driver::Channel and driver::ChannelManager). nw::snd has the same channels. A channel is a note
// on a MultiVoice, with an envelope, four LFOs, a pitch sweep and the parameters from the note's sequence track. Every
// sound frame, the channel works these out into the MultiVoice's parameters. Each function's comment gives the address
// of the routine it follows in WarioWare: Get It Together!'s main.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <span>
#include <vector>

#include "atk/envelope.h"
#include "atk/hardware.h"
#include "atk/low_level_voice.h"
#include "atk/util.h"
#include "atk/voice.h"

namespace hsf::atk
{

// A wave as nn::atk reads it from its file (WaveInfo, which WaveFileReader::ReadWaveInfo fills, 0xc0b410): the sample
// format, the loop, the rate, the loop's start and the end (also the length when the wave doesn't loop), and each
// channel's samples, with the DSP-ADPCM coefficients and the decoder's state at the start and at the loop.
struct WaveInfo
{
    struct Channel
    {
        std::span<const uint8_t> data;
        AdpcmCoefficients coefs{};
        AdpcmContext context;
        AdpcmContext loop_context;
    };

    SampleFormat format = SampleFormat::kDspAdpcm;
    bool loop = false;
    int channel_count = 1;
    uint32_t sample_rate = 32000;
    int64_t loop_start = 0;
    int64_t loop_end = 0;
    int64_t original_loop_start = 0;
    std::array<Channel, 2> channels;
};

class ChannelManager;

class Channel
{
public:
    // Why a channel calls its owner back (ChannelCallbackStatus): the channel was stopped (0), the channel's voice was
    // dropped (1), the wave played to its end (2), or the channel was cancelled (3).
    enum class CallbackStatus : uint32_t
    {
        kStopped = 0,
        kDrop = 1,
        kFinish = 2,
        kCancel = 3,
    };

    using Callback = std::function<void(Channel& channel, CallbackStatus status)>;

    // Plays a wave on the channel's MultiVoice from its start, for `length` ticks or for ever when negative (Start,
    // 0xc23720). A note always starts at the wave's start. The model therefore leaves out the start offset that the
    // game can give.
    void Start(const WaveInfo& wave, int32_t length);

    // Each sound frame: the volume, pitch and output parameters, from the note, the track, the envelope and the LFOs
    // (Update, 0xc23120). With `periodic`, the envelope, LFOs and sweep advance by `frames` renderer frames of 5 ms.
    void Update(bool periodic, int frames, Random& random);

    void Stop();    // 0xc23670
    void Release(); // 0xc236d0

    // The note's end (NoteOff, 0xc23a70). A channel with ignore_note_off set plays on. A channel that hasn't had an
    // update yet releases at its next periodic update, after its envelope steps. Otherwise the channel releases at
    // once.
    void NoteOff();

    // Calls the owner's callback, once (CallChannelCallback, 0xc23c80).
    void CallChannelCallback(CallbackStatus status);

    // Whether the wave loops.
    bool WaveLoops() const
    {
        return loop_;
    }

    // A pitch sweep from `pitch` semitones away to the note's key over `count` (SetSweepParam, 0xc23af0): ticks, or
    // milliseconds when the sweep updates by itself.
    void SetSweepParam(float pitch, int32_t count, bool auto_update)
    {
        sweep_pitch = pitch;
        sweep_length = count;
        auto_sweep = auto_update;
        sweep_counter = 0;
    }

    // 0xc236b0.
    void UpdateSweep(int32_t count)
    {
        sweep_counter += count;
        if (sweep_counter > sweep_length)
        {
            sweep_counter = sweep_length;
        }
    }

    CurveAdshr envelope;                // +0x20
    std::array<CurveLfo, 4> lfos;       // +0x3c
    std::array<uint8_t, 4> lfo_types{}; // +0xbc: 0 pitch, 1 volume, 2 pan, 0xff none
    bool pause = false;                 // +0xc0
    bool active = false;                // +0xc1
    bool allocated = false;             // +0xc2
    bool auto_sweep = true;             // +0xc3
    bool release_priority_fix = false;  // +0xc4
    bool ignore_note_off = false;       // +0xc5
    uint8_t biquad_type = 0;            // +0xc6
    float track_volume = 1.0f;          // +0xc8
    float user_pitch = 1.0f;            // +0xcc
    float lpf = 0.0f;                   // +0xd0
    float biquad_value = 0.0f;          // +0xd4
    uint32_t output_line = 1;           // +0xd8
    OutputParam output;                 // +0xdc
    float bend = 0.0f;                  // +0x138: semitones
    float sweep_pitch = 0.0f;           // +0x13c
    int32_t sweep_counter = 0;          // +0x140
    int32_t sweep_length = 0;           // +0x144
    float init_pan = 0.0f;              // +0x148
    float init_span = 0.0f;             // +0x14c
    float tune = 1.0f;                  // +0x150
    uint8_t silence_from = 255;         // +0x154: a ramp of the volume, in 255ths, that silences a track
    uint8_t silence_to = 255;           // +0x155
    uint16_t silence_duration = 0;      // +0x156: frames
    uint16_t silence_counter = 0;       // +0x158
    float cached_pitch = 0.0f;          // +0x15c
    float cached_ratio = 1.0f;          // +0x160
    int32_t length = 0;                 // +0x164: ticks, or negative for none
    uint32_t pan_mode = 0;              // +0x168
    uint32_t pan_curve = 0;             // +0x16c
    uint8_t key = 60;                   // +0x170
    uint8_t original_key = 60;          // +0x171
    uint8_t key_group = 0;              // +0x172
    uint8_t interpolation_type = 0;     // +0x173
    float init_volume = 1.0f;           // +0x174
    float velocity_volume = 1.0f;       // +0x178
    Callback callback;                  // +0x180
    MultiVoice* voice = nullptr;        // +0x190
    Channel* next_in_track = nullptr;   // +0x198

private:
    friend class ChannelManager;

    void InitParam(Callback owner);                                          // 0xc22fd0
    void AppendWaveBuffer(const WaveInfo& wave);                             // 0xc237e0
    void OnVoiceEvent(MultiVoice& voice, MultiVoice::CallbackStatus status); // VoiceCallbackFunc, 0xc23bd0

    // The sweep's pitch now, in semitones.
    float GetSweepValue() const;

    std::array<std::array<WaveBuffer, 2>, 2> wave_buffers_; // +0x1a0: each wave channel's start and loop
    std::array<AdpcmContext, 2> contexts_;                  // +0x2c0
    std::array<AdpcmContext, 2> loop_contexts_;             // +0x340
    int64_t loop_start_ = 0;                                // +0x3c0
    int64_t original_loop_start_ = 0;                       // +0x3c8
    bool loop_ = false;                                     // +0x3d0
    bool release_requested_ = false;                        // +0x3d1: a note-off waits for the next periodic update
    bool updated_ = false;                                  // +0x3d2: the channel has had an update
};

// What a track passes to its bank when it plays a note (NoteOnInfo): the program, the key and the velocity (scaled by
// the track's velocity range), the length in ticks or -1, the track's initial pan, the channel's priority (the player's
// priority and the track's priority added), the track to notify when the channel ends, and the mix its voices output
// to.
struct NoteOnInfo
{
    uint32_t prg_no = 0;
    int key = 60;
    int velocity = 127;
    int32_t length = -1;
    int init_pan = 0;
    int32_t priority = 64;
    Channel::Callback callback;
    const OutputReceiver* receiver = nullptr;
};

// The channels: a pool of a fixed number, and a list of those in use, in the order they were allocated. They update in
// that order. The pool reuses the last freed channel first.
class ChannelManager
{
public:
    // `count` is the sound system's voice count. The sound system has as many channels as voices.
    ChannelManager(MultiVoiceManager& voices, int count);

    ChannelManager(const ChannelManager&) = delete;
    ChannelManager& operator=(const ChannelManager&) = delete;

    // A channel with a MultiVoice for `channels` channels at `priority`, or nullptr (Channel::AllocChannel, 0xc23cb0).
    Channel* AllocChannel(int channels, int32_t priority, Channel::Callback callback);

    // Each sound frame: every channel's update, and those that have stopped are freed (UpdateAllChannel, 0xc24440).
    void UpdateAllChannel(int frames, Random& random);

    int ActiveCount() const
    {
        return static_cast<int>(active_.size());
    }

    // The channels in use, in the list's order.
    const std::list<Channel*>& InUse() const
    {
        return active_;
    }

    // A channel's place in the pool, from 0, or -1 if it isn't in use.
    int Index(const Channel& channel) const;

    // Whether any channel's envelope is in its attack, hold, decay or release, rather than settled at its sustain level
    // (for the ripper's length analysis).
    bool AnyChanging() const;

    // Takes a channel out of the list and frees it (Free, 0xc24200).
    void Free(Channel& channel);

private:
    MultiVoiceManager* voices_;
    std::vector<std::unique_ptr<Channel>> pool_; // each place's channel while it's in use
    std::vector<int> free_;                      // the free places, the next one to use last
    std::list<Channel*> active_;
};

} // namespace hsf::atk
