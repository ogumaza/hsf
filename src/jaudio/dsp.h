// SPDX-License-Identifier: MIT

// A model of JAudio's DSP program in the Switch ports. Each port's `main` links Hagi, an emulator of the GameCube's and
// the Wii's hardware. Hagi runs the DSP program as a high-level emulation in C++. This model follows that emulation in
// the Super Mario Galaxy games' build, for their stereo output: the channels' decoding of AFC and PCM16 waves, their
// resampling, filters, mixers and auto mixer, the four FX lines, the matrix of the buses, and the output. The CPU side
// of JAudio writes each channel's block and the FX lines' settings in the emulated console's memory. The DSP reads them
// there, renders 80 samples a sub-frame at 32 kHz, and writes the output there.
//
// The arithmetic follows Hagi's. Products are rounded half to even at bit 15, like the DSP's accumulator. Sums are
// clamped to 16 bits in the decoders, the resamplers and the mixers. In the matrix and the FX lines, they wrap.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hsf::jaudio
{

// The emulated console's memory, by the DSP program's addresses. The CPU side keeps the channels' blocks, the FX
// lines' settings and buffers, the output buffers and the waves here. A read outside the memory gives zeros, and a
// write outside it is lost.
class DspMemory
{
public:
    explicit DspMemory(uint32_t size);

    uint32_t Size() const
    {
        return static_cast<uint32_t>(bytes_.size());
    }

    uint8_t Byte(uint32_t address) const;
    void SetByte(uint32_t address, uint8_t value);

    // A 16-bit value in the host's order, little-endian. The CPU side and the emulation share these.
    uint16_t Native16(uint32_t address) const;
    void SetNative16(uint32_t address, uint16_t value);

    // A 16-bit value in the console's order, big-endian. The DSP reads and writes samples in it.
    uint16_t Big16(uint32_t address) const;
    void SetBig16(uint32_t address, uint16_t value);

    uint32_t Native32(uint32_t address) const;
    void SetNative32(uint32_t address, uint32_t value);

    std::span<uint8_t> Bytes()
    {
        return bytes_;
    }

    std::span<const uint8_t> Bytes() const
    {
        return bytes_;
    }

private:
    std::vector<uint8_t> bytes_;
};

// A channel's block: the 0x180 bytes of JASDsp::TChannel, in 16-bit words. The DSP reads the whole block and
// writes back its first 0x100 bytes.
struct DspChannelBlock
{
    static constexpr std::size_t kBytes = 0x180;
    static constexpr std::size_t kWrittenBytes = 0x100;

    // The fields by their byte offsets.
    static constexpr std::size_t kActive = 0x00;
    static constexpr std::size_t kFinished = 0x02;
    static constexpr std::size_t kPitch = 0x04; // the step through the wave per output sample, 4.12
    static constexpr std::size_t kReset = 0x08; // set by the CPU side when the channel starts
    static constexpr std::size_t kLoopPending = 0x0a;
    static constexpr std::size_t kPause = 0x0c;
    static constexpr std::size_t kDelayCount = 0x0e;   // samples of history that the mixers' delays read
    static constexpr std::size_t kMixers = 0x10;       // 6 of 8 bytes: bus, target volume, volume, delays
    static constexpr std::size_t kAutoPanDolby = 0x50; // the auto mixer's pan, then its surround level
    static constexpr std::size_t kAutoFxmix = 0x52;    // the auto mixer's send to the FX lines
    static constexpr std::size_t kAutoVolume = 0x54;
    static constexpr std::size_t kAutoTarget = 0x56;
    static constexpr std::size_t kAutoMixer = 0x58; // the auto mixer's switch, nonzero for Dolby mode
    static constexpr std::size_t kFraction = 0x60;  // the position's fraction, 12 bits
    static constexpr std::size_t kLeftover = 0x64;  // decoded samples left in the saved block
    static constexpr std::size_t kLastSample = 0x66;
    static constexpr std::size_t kPcmPosition = 0x68;
    static constexpr std::size_t kReadAddress = 0x70;
    static constexpr std::size_t kRemaining = 0x74; // samples left before the loop's end or the wave's
    static constexpr std::size_t kHistory = 0x78;   // the resampler's 4 samples
    static constexpr std::size_t kFirHistory = 0x80;
    static constexpr std::size_t kIirState = 0xa8; // 2 inputs and 2 outputs
    static constexpr std::size_t kSavedBlock = 0xb0;
    static constexpr std::size_t kAdpcmHistory2 = 0xcc; // the saved block's last 2 samples
    static constexpr std::size_t kAdpcmHistory1 = 0xce;
    static constexpr std::size_t kDelayHistory = 0xd0; // ends at 0xee
    static constexpr std::size_t kDistState = 0xf0;
    static constexpr std::size_t kFormat = 0x100; // bytes per block of 16 samples: 9 or 5 for AFC, 16 for PCM16
    static constexpr std::size_t kLoops = 0x102;
    static constexpr std::size_t kLoopLast = 0x104; // the decoder's history at the loop start
    static constexpr std::size_t kLoopPenult = 0x106;
    static constexpr std::size_t kFilterMode = 0x108; // FIR taps in bits 0-4, IIR in bit 5
    static constexpr std::size_t kForcedStop = 0x10a;
    static constexpr std::size_t kLoopStart = 0x110;
    static constexpr std::size_t kLoopEnd = 0x114;
    static constexpr std::size_t kWaveAddress = 0x118;
    static constexpr std::size_t kFirCoefficients = 0x120;
    static constexpr std::size_t kIirCoefficients = 0x148;
    static constexpr std::size_t kDist = 0x150;

    uint16_t U16(std::size_t offset) const
    {
        return words[offset / 2];
    }

    int16_t S16(std::size_t offset) const
    {
        return static_cast<int16_t>(words[offset / 2]);
    }

    uint32_t U32(std::size_t offset) const
    {
        return words[offset / 2] | static_cast<uint32_t>(words[offset / 2 + 1]) << 16;
    }

    void SetU16(std::size_t offset, uint32_t value)
    {
        words[offset / 2] = static_cast<uint16_t>(value);
    }

    void SetU32(std::size_t offset, uint32_t value)
    {
        words[offset / 2] = static_cast<uint16_t>(value);
        words[offset / 2 + 1] = static_cast<uint16_t>(value >> 16);
    }

    std::array<uint16_t, kBytes / 2> words{};
};

// An FX line's settings. The DSP copies them from the CPU side's JASDsp::FxBuf at each frame's start.
struct DspFxLine
{
    static constexpr std::size_t kBytes = 0x28;

    uint16_t Flags() const
    {
        return words[0];
    }

    uint16_t Subframes() const // the delay's length
    {
        return words[1];
    }

    uint32_t Buffer() const
    {
        return words[4] | static_cast<uint32_t>(words[5]) << 16;
    }

    std::array<uint16_t, kBytes / 2> words{};
};

class Dsp
{
public:
    // The emulation's switches for the Galaxy games' DSP program. Hagi picks them by the program's hash.
    static constexpr uint32_t kGalaxyFlags = 0x19b7c;

    // The DSP's mixing buses, in its memory's order. A mixer or an FX line names a bus by its address in the DSP's
    // memory. Bus 5 is the left output, bus 6 the right one, buses 7 to 10 feed the FX lines.
    static constexpr std::size_t kBusCount = 13;
    static constexpr std::size_t kBusSamples = 96;
    static constexpr std::array<uint16_t, 14> kGalaxyBusAddresses = {
        0x09a0, 0x0a00, 0x0a60, 0x0be0, 0x0c40, 0x0ca0, 0x0d00, 0x0d60, 0x0dc0, 0x0e20, 0x0e80, 0x0ee0, 0x0f40, 0x0fa0};

    static constexpr int kSubframeSamples = 80;
    static constexpr std::size_t kResampleTableSize = 0x280;
    static constexpr std::size_t kAdpcmTableSize = 32;
    static constexpr std::size_t kAutoMixerSine = 0x200; // where the auto mixer's quarter of a sine starts

    using Bus = std::array<int16_t, kBusSamples>;

    // The DSP program's tables. The CPU side hands them over at start-up (DsetupTable): the resampler's filter, 64
    // phases of 4 taps with the oscillators' waveforms and the auto mixer's quarter of a sine after them, and the AFC
    // decoder's 16 pairs of coefficients.
    struct Tables
    {
        std::array<int16_t, kResampleTableSize> resample{};
        std::array<int16_t, kAdpcmTableSize> adpcm{};
    };

    Dsp(DspMemory& memory, const Tables& tables, uint32_t flags = kGalaxyFlags,
        const std::array<uint16_t, 14>& bus_addresses = kGalaxyBusAddresses);

    // The start-up commands: DsetupTable's channel count and the addresses of the channels' blocks and the FX lines'
    // settings, and DsetVARAM's base of the waves' addresses.
    void Setup(uint16_t channel_count, uint32_t channel_blocks, uint32_t fx_settings);
    void SetVaram(uint32_t base);

    // A frame's start (DsyncFrame2ch): the sub-frames to render, the mixer's level (4096 for 1.0) and the addresses of
    // the left and right output buffers. The DSP copies the FX lines' settings.
    void StartFrame(uint16_t subframes, uint16_t level, uint32_t left, uint32_t right);

    // A sub-frame: its start, each channel that the CPU side has released, in order, and its end. The end writes 80
    // samples of each output.
    void StartSubframe();
    void ProcessChannel(uint16_t index);
    void EndSubframe();

    // Renders a whole sub-frame: its start, every channel whose block is active, and its end.
    void RenderSubframe();

    uint16_t ChannelCount() const
    {
        return channel_count_;
    }

    // The state, for tests.
    const Bus& BusSamples(std::size_t bus) const
    {
        return buses_[bus];
    }

    Bus& BusSamples(std::size_t bus)
    {
        return buses_[bus];
    }

    struct CenterDelay
    {
        uint16_t write = 0;
        uint16_t read = 0;
        int16_t direction = 0;
        uint16_t low = 0;
        uint16_t high = 0;
        uint16_t step = 0;
        uint16_t fraction = 0;
        uint16_t depth = 0;
        std::array<int16_t, 160> samples{};
    };

    CenterDelay& Delay()
    {
        return delay_;
    }

    const CenterDelay& Delay() const
    {
        return delay_;
    }

    std::array<uint16_t, 4>& FxPositions()
    {
        return fx_positions_;
    }

    const std::array<uint16_t, 4>& FxPositions() const
    {
        return fx_positions_;
    }

    std::array<DspFxLine, 4>& FxLines()
    {
        return fx_lines_;
    }

    const std::array<DspFxLine, 4>& FxLines() const
    {
        return fx_lines_;
    }

    std::array<uint32_t, 4>& Outputs()
    {
        return outputs_;
    }

    const std::array<uint32_t, 4>& Outputs() const
    {
        return outputs_;
    }

    uint16_t Subframe() const
    {
        return subframe_;
    }

    uint16_t SubframeCount() const
    {
        return subframe_count_;
    }

    int16_t Level() const
    {
        return level_;
    }

    void SetLevel(int16_t level)
    {
        level_ = level;
    }

    uint16_t Resets() const
    {
        return resets_;
    }

private:
    // A bus by the address that a mixer or an FX line names, or null. The address 0x0a60 names no bus that they can
    // reach. Hagi's emulation doesn't check the address: for one that names no bus, it writes through a null pointer.
    // The model leaves the mixer or the send out.
    Bus* FindBus(uint16_t address);

    // A channel's work buffer, laid out like the emulation's on its stack. The source comes first: the resampler's 4
    // samples of history, then the decoded samples. The output follows: 32 samples of room for the FIR filter's and
    // the mixers' history, then the 80 samples.
    static constexpr std::size_t kSourceSamples = 1312;
    static constexpr std::size_t kWorkSamples = kSourceSamples + 32 + kSubframeSamples;

    // The steps of a channel.
    void DecodeAfc(DspChannelBlock& block, std::span<int16_t> source, int blocks);
    void FetchPcm16(DspChannelBlock& block, int16_t* destination);
    void Resample(DspChannelBlock& block, int16_t* source, int16_t* out);
    void Dist(DspChannelBlock& block, int16_t* samples);
    void Fir(DspChannelBlock& block, int16_t* samples);
    void Iir(DspChannelBlock& block, int16_t* samples);
    void Mix(DspChannelBlock& block, std::span<int16_t> work, int offset);
    void AutoMix(DspChannelBlock& block, const int16_t* samples);
    void ProcessAfc(DspChannelBlock& block, std::span<int16_t> source);

    // The FX lines at a sub-frame's start, and the matrix of the surround buses.
    void ReadFxLine(const DspFxLine& line, uint16_t position, Bus& bus);
    void StartFxLines();
    void DelayCenter();
    void FinishFxLines();

    DspMemory& memory_;
    Tables tables_;
    uint32_t flags_;
    std::array<uint16_t, 14> bus_addresses_;

    uint16_t channel_count_ = 0;
    uint32_t channel_blocks_ = 0;
    uint32_t fx_settings_ = 0;
    uint32_t varam_ = 0;

    uint16_t subframe_ = 0;
    uint16_t subframe_count_ = 0;
    int16_t level_ = 0;
    std::array<uint32_t, 4> outputs_{}; // left, right, and the surround outputs (0 in stereo)
    uint16_t resets_ = 0;               // the emulation's count of the channels started

    CenterDelay delay_;
    std::array<uint16_t, 4> fx_positions_{};
    std::array<DspFxLine, 4> fx_lines_{};
    std::array<Bus, kBusCount> buses_{};
};

} // namespace hsf::jaudio
