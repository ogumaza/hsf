// SPDX-License-Identifier: MIT

// The Switch's audio renderer, as a game's nn::audio sets it up: voices, the final mix and sub-mixes, and a device
// sink, and the frames it renders from them. This follows the build of the renderer for the CPU in WarioWare: Get It
// Together!'s sdk (nn::audio::server and nn::audio::dsp). The calls change the game's copy of the parameters. Each
// frame then sends them to the renderer. The renderer takes them in, reports the voices' state from two frames before,
// generates the frame's command list and runs it. An aux effect in a mix sends the mix's samples to the game and
// receives what the game makes of them. Not modelled yet: the renderer's other effects, splitters, voice dropping, the
// downmix and the upsampler for a 32 kHz renderer.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "audio/dsp.h"
#include "audio/voice.h"

namespace hsf::audio
{

// nn::audio::VoiceType::PlayState.
enum class PlayState : uint8_t
{
    kStarted = 0,
    kStopped = 1,
    kPaused = 2,
};

// nn::audio::BiquadFilterParameter: the coefficients in Q14, the feedback ones added.
struct BiquadFilterParameter
{
    bool enabled = false;
    std::array<int16_t, 3> b{};
    std::array<int16_t, 2> a{};
};

// What AppendWaveBuffer takes (nn::audio::WaveBuffer): the samples and the range in them, and for DSP-ADPCM the context
// at the range's start, if any. GetReleasedWaveBuffer gives `tag` back once the renderer is done with the buffer.
struct WaveBufferInfo
{
    std::span<const uint8_t> buffer;
    int32_t start = 0;
    int32_t end = 0;
    bool loop = false;
    bool end_of_stream = false;
    const AdpcmContext* context = nullptr;
    const void* tag = nullptr;
};

// The renderer's settings (nn::audio::AudioRendererParameter).
struct RendererParameter
{
    int32_t sample_rate = 48000;
    int32_t sample_count = 240;
    int32_t mix_buffer_count = 1;
    int32_t sub_mix_count = 0;
    int32_t voice_count = 1;
};

// Sees each call that the client makes on a renderer once the renderer has taken it: the arguments the client passed,
// and what the call gave back. A test makes the same calls on Nintendo's renderer through it. A renderer has no
// observer unless one is set.
class RendererObserver
{
public:
    virtual ~RendererObserver() = default;

    // The renderer as the client opened it (OpenAudioRenderer), when the observer is set.
    virtual void OpenAudioRenderer(const RendererParameter& parameter) = 0;

    virtual void AcquireVoiceSlot(int32_t sample_rate, int channel_count, SampleFormat format, int32_t priority,
                                  const AdpcmCoefficients* coefficients, int voice) = 0;
    virtual void ReleaseVoiceSlot(int voice) = 0;
    virtual void SetVoicePlayState(int voice, PlayState state) = 0;
    virtual void GetVoicePlayState(int voice, PlayState state) = 0;
    virtual void SetVoicePitch(int voice, float pitch) = 0;
    virtual void SetVoiceVolume(int voice, float volume) = 0;
    virtual void SetVoicePriority(int voice, int32_t priority) = 0;
    virtual void SetVoiceSrcQuality(int voice, SrcQuality quality) = 0;
    virtual void SetVoiceBiquadFilterParameter(int voice, int index, const BiquadFilterParameter& parameter) = 0;
    virtual void SetVoiceDestination(int voice, int mix) = 0;
    virtual void SetVoiceMixVolume(int voice, float volume, int channel, int buffer) = 0;
    virtual void AppendWaveBuffer(int voice, const WaveBufferInfo& buffer, bool appended) = 0;
    virtual void GetReleasedWaveBuffer(int voice, const void* tag) = 0;
    virtual void GetVoicePlayedSampleCount(int voice, uint64_t count) = 0;
    virtual void IsVoiceDroppedFlagOn(int voice, bool dropped) = 0;
    virtual void AcquireFinalMix(int buffer_count, bool acquired) = 0;
    virtual void AcquireSubMix(int32_t sample_rate, int buffer_count, int sub_mix) = 0;
    virtual void SetSubMixDestination(int sub_mix, int destination) = 0;
    virtual void SetSubMixMixVolume(int sub_mix, float volume, int source, int destination) = 0;
    virtual void AddDeviceSink(std::span<const int> inputs) = 0;
    virtual void AddAux(int mix, uint32_t buffer_size, int aux) = 0;
    virtual void SetAuxInputOutput(int aux, std::span<const int8_t> inputs, std::span<const int8_t> outputs) = 0;
    virtual void SetAuxEnabled(int aux, bool enabled) = 0;
    virtual void GetAuxSampleCount(int aux, int32_t count) = 0;
    virtual void GetAuxSampleRate(int aux, int32_t rate) = 0;

    // The client's buffer after the read, with how many samples the renderer gave. The samples the client wrote, with
    // how many the renderer took.
    virtual void ReadAuxSendBuffer(int aux, std::span<const int32_t> samples, int32_t read) = 0;
    virtual void WriteAuxReturnBuffer(int aux, std::span<const int32_t> samples, int32_t written) = 0;

    // A frame: the update, and the command list's generation and processing.
    virtual void RenderFrame() = 0;
};

class Renderer
{
public:
    // A mix by its id: the final mix is 0, the sub-mixes 1 up.
    static constexpr int kFinalMix = 0;
    static constexpr int kNoMix = -1;

    // The most channels a voice has, and the most buffers a mix has.
    static constexpr int kMaxChannels = 6;
    static constexpr int kMaxMixBuffers = 24;

    explicit Renderer(const RendererParameter& parameter);

    const RendererParameter& Parameter() const
    {
        return parameter_;
    }

    // Shows every call from now on to `observer`, or to none when it's null, starting with the parameter the renderer
    // was opened with. The observer has to outlive the renderer, or be replaced first.
    void SetObserver(RendererObserver* observer);

    // A voice is its slot, or -1 when there is no free slot or not enough channel resources (AcquireVoiceSlot). A new
    // voice is stopped, at pitch 1 and volume 1, with no destination.
    int AcquireVoiceSlot(int32_t sample_rate, int channel_count, SampleFormat format, int32_t priority,
                         const AdpcmCoefficients* coefficients);

    // ReleaseVoiceSlot: the renderer stops the voice at once, with no depop.
    void ReleaseVoiceSlot(int voice);

    void SetVoicePlayState(int voice, PlayState state);
    PlayState GetVoicePlayState(int voice) const;
    void SetVoicePitch(int voice, float pitch);
    void SetVoiceVolume(int voice, float volume);
    void SetVoicePriority(int voice, int32_t priority);
    void SetVoiceSrcQuality(int voice, SrcQuality quality);
    void SetVoiceBiquadFilterParameter(int voice, int index, const BiquadFilterParameter& parameter);
    void SetVoiceDestination(int voice, int mix);

    // The gain from a channel of the voice to a buffer of its destination.
    void SetVoiceMixVolume(int voice, float volume, int channel, int buffer);

    // Queues a buffer: false when the voice already has 4 that it hasn't released.
    bool AppendWaveBuffer(int voice, const WaveBufferInfo& buffer);

    // The tag of the oldest buffer the renderer has finished with, or nullptr. Each one comes back once.
    const void* GetReleasedWaveBuffer(int voice);

    // What the last frame's update reported, as of two frames before it.
    uint64_t GetVoicePlayedSampleCount(int voice) const;
    bool IsVoiceDroppedFlagOn(int voice) const;

    // The final mix (AcquireFinalMix) and the sub-mixes (AcquireSubMix). AcquireSubMix returns the id or kNoMix. A
    // sub-mix's sample rate sets its depop's decay.
    bool AcquireFinalMix(int buffer_count);
    int AcquireSubMix(int32_t sample_rate, int buffer_count);
    void SetSubMixDestination(int sub_mix, int destination);

    // The gain from a buffer of the sub-mix to a buffer of its destination.
    void SetSubMixMixVolume(int sub_mix, float volume, int source, int destination);

    // The device sink: the final mix's buffers it takes, one for each channel of the output (AddDeviceSink).
    void AddDeviceSink(std::span<const int> inputs);

    // The size of each of an aux effect's two buffers for `frames` frames of `channels` channels
    // (GetRequiredBufferSizeForAuxSendReturnBuffer, 0x3f5c0): a header of 0x80 bytes and the samples, in 0x40 bytes.
    static uint32_t GetRequiredBufferSizeForAuxSendReturnBuffer(const RendererParameter& parameter, int frames,
                                                                int channels);

    // An aux effect on a mix, after the mix's other effects (AddAux, 0x3f750, with buffers of `buffer_size` bytes).
    // Each frame, the renderer writes the effect's input buffers into a send ring for the game to read. The renderer
    // takes what the game wrote into a return ring, from the same place in the ring, and puts it into the effect's
    // output buffers. A ring holds whole frames of every channel. What the game writes back therefore comes out a
    // ring's worth of frames after it went in. Returns the effect.
    int AddAux(int mix, uint32_t buffer_size);

    // The mix's buffers that the effect takes and gives, one for each channel and at most the mix's buffer count
    // (SetAuxInputOutput, 0x3fb70).
    void SetAuxInputOutput(int aux, std::span<const int8_t> inputs, std::span<const int8_t> outputs);

    // A disabled effect passes its inputs through and starts its rings again (SetAuxEnabled, 0x3fa90).
    void SetAuxEnabled(int aux, bool enabled);

    // The samples a ring has room for (GetAuxSampleCount, 0x3fcc0), and the mix's rate.
    int32_t GetAuxSampleCount(int aux) const;
    int32_t GetAuxSampleRate(int aux) const;

    // The samples the renderer has sent since the last read, up to the span's size (ReadAuxSendBuffer, 0x3fc70, through
    // ReadAuxBuffer, 0x3d780). Returns how many.
    int32_t ReadAuxSendBuffer(int aux, std::span<int32_t> samples);

    // Samples for the renderer, as many as the ring has room for up to where the renderer reads next
    // (WriteAuxReturnBuffer, 0x3fc90, through WriteAuxBuffer, 0x3d950). Returns how many.
    int32_t WriteAuxReturnBuffer(int aux, std::span<const int32_t> samples);

    // A frame: the update, then the command list's generation and processing.
    void RenderFrame();

    // The device sink's last frame: its channels interleaved, held to 16 bits.
    std::span<const int16_t> DeviceOutput() const
    {
        return output_;
    }

private:
    struct ClientWaveBuffer
    {
        WaveBufferInfo info;
        bool sent = true; // an update has sent it
    };

    // A channel resource: the client's mix volumes, the renderer's mix volumes with the last frame's volumes, and two
    // copies of the channel's processing state. The commands use one copy. Updates report the other.
    struct ChannelResource
    {
        bool in_use = false;
        std::array<float, kMaxMixBuffers> client_mix{};
        std::array<float, kMaxMixBuffers> mix{};
        std::array<float, kMaxMixBuffers> previous_mix{};
        VoiceState dsp;
        VoiceState reported;
    };

    // A voice slot: what the client keeps, and the renderer's copy and state.
    struct Voice
    {
        // The client's fields.
        bool in_use = false;
        bool is_new = false;
        PlayState play_state = PlayState::kStopped;
        SampleFormat format = SampleFormat::kPcmInt16;
        int32_t sample_rate = 48000;
        int32_t priority = 0;
        int channel_count = 0;
        float pitch = 1.0f;
        float volume = 1.0f;
        SrcQuality quality = SrcQuality::kMedium;
        std::array<BiquadFilterParameter, 2> biquads;
        AdpcmCoefficients coefficients{};
        int mix = kNoMix;
        std::array<ClientWaveBuffer, kWaveBufferCount> buffers;
        int buffer_count = 0; // appended and not yet released
        int buffer_index = 0; // the oldest not yet released
        int released = 0;     // released since the voice was acquired
        std::array<int, kMaxChannels> resources{};
        uint64_t played = 0; // the statuses of the last update
        int consumed = 0;
        bool dropped = false;

        // The renderer's fields.
        struct Server
        {
            bool in_use = false;
            bool is_new = false;
            int state = 1;    // 0 started, 1 stopped, 2 stop requested, 3 paused
            int previous = 1; // the state before the last update
            SampleFormat format = SampleFormat::kPcmInt16;
            int32_t sample_rate = 48000;
            int32_t priority = 0;
            int32_t sort_order = 0;
            int channel_count = 0;
            float pitch = 1.0f;
            float volume = 1.0f;
            float previous_volume = 0.0f;
            SrcQuality quality = SrcQuality::kMedium;
            std::array<BiquadFilterParameter, 2> biquads;
            std::array<bool, 2> biquads_were_enabled{};
            int mix = kNoMix;
            int buffer_count = 0;
            std::array<WaveBuffer, kWaveBufferCount> buffers;
            std::array<AdpcmContext, kWaveBufferCount> contexts{};
            std::array<bool, kWaveBufferCount> sent{true, true, true, true}; // false: taken, valid at the next frame
            bool stopping = false;
        } server;
    };

    struct Mix
    {
        bool in_use = false;
        int32_t sample_rate = 48000;
        int buffer_count = 0;
        float volume = 1.0f;
        int destination = kNoMix;
        std::vector<std::array<float, kMaxMixBuffers>> mix_volumes; // [source buffer][destination buffer]
        int offset = 0;                                             // its first mix buffer
        std::vector<int> effects;                                   // in their order
    };

    // An aux effect: the client's settings and the renderer's copy of them, and the two rings with their offsets, in
    // samples (the headers' fields at +0x0, +0x4, +0x40 and +0x44).
    struct Aux
    {
        int mix = kNoMix;
        bool enabled = false;
        std::vector<int8_t> inputs;
        std::vector<int8_t> outputs;
        uint32_t capacity = 0;        // samples a ring has room for
        uint32_t capacity_frames = 0; // frames of one channel
        uint32_t ring = 0;            // the samples the rings go round in: whole frames of every channel

        bool server_enabled = false;
        std::vector<int8_t> server_inputs;
        std::vector<int8_t> server_outputs;
        uint32_t server_ring = 0;

        std::vector<int32_t> send;
        std::vector<int32_t> receive;
        uint32_t send_read = 0;     // the client's offset
        uint32_t send_write = 0;    // the renderer's offset
        uint32_t receive_write = 0; // the client's offset
        uint32_t receive_read = 0;  // the renderer's offset
    };

    Voice& At(int voice);
    const Voice& At(int voice) const;
    void Insert(int voice);
    void Remove(int voice);

    // What the client calls of the same names do, before the observer sees them.
    int TakeVoiceSlot(int32_t sample_rate, int channel_count, SampleFormat format, int32_t priority,
                      const AdpcmCoefficients* coefficients);
    bool QueueWaveBuffer(int voice, const WaveBufferInfo& buffer);
    const void* TakeReleasedWaveBuffer(int voice);
    bool TakeFinalMix(int buffer_count);
    int TakeSubMix(int32_t sample_rate, int buffer_count);
    int32_t ReadAuxSend(int aux, std::span<int32_t> samples);
    int32_t WriteAuxReturn(int aux, std::span<const int32_t> samples);

    void Update();
    void Generate();
    void LayOutMixes();
    void RenderVoice(Voice& voice);
    void RenderSubMix(const Mix& mix);
    void RenderFinalMix();
    void Depop(const Mix& mix);
    void RenderEffects(const Mix& mix);
    int32_t* Buffer(int index);

    RendererParameter parameter_;
    std::vector<Voice> voices_;
    std::vector<ChannelResource> resources_;
    std::vector<int> order_; // the client's voices in use, by priority, the newest first among equals
    std::vector<Mix> mixes_;
    std::vector<Aux> auxes_;
    bool mixes_changed_ = true;
    std::vector<int> sinks_;       // the device sink's inputs
    std::vector<int32_t> buffers_; // the mix buffers, then a working buffer for each channel a voice may have
    std::vector<int32_t> depop_;   // a value for each mix buffer
    std::vector<int16_t> output_;
    RendererObserver* observer_ = nullptr;
};

} // namespace hsf::audio
