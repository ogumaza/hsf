// SPDX-License-Identifier: MIT

#include "jaudio/dsp.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace hsf::jaudio
{

namespace
{

// The emulation's switches (Dsp::kGalaxyFlags) that this model follows. With kFlagAutoMixer16, which the Galaxy games
// leave off, the auto mixer's gains are over 65536, and its sends take the volume twice.
constexpr uint32_t kFlagAutoMixer = 1u << 6;  // a channel can mix by the auto mixer
constexpr uint32_t kFlagMatrix = 1u << 3;     // the surround buses are matrixed into the FX lines and the output
constexpr uint32_t kFlagFastPaths = 1u << 12; // the decoders' paths for AFC that this model follows
constexpr uint32_t kFlagAutoMixer16 = 1u << 14;
constexpr uint32_t kFlagFxDelay = 1u << 15;     // the FX lines keep 8 samples of history and read through a gain
constexpr uint32_t kFlagDirectFetch = 1u << 16; // the decoders read the console's memory directly
constexpr uint32_t kFlagCopyAutoMixer = 1u << 17;

int16_t Clamp16(int64_t value)
{
    return static_cast<int16_t>(std::clamp<int64_t>(value, -0x8000, 0x7fff));
}

// `p` / 32768, rounded to the nearest integer with ties to even, like the DSP's accumulator: up when bit 14 is set
// together with bit 15 or any bit below 14.
int64_t RoundQ15(int64_t p)
{
    const int64_t floor = (p * 2) >> 16;
    const bool up = (p & 0x4000) != 0 && (p & 0xbfff) != 0;

    return floor + (up ? 1 : 0);
}

// `value` added to `sample` in 16 bits. The sum wraps, like those of the matrix and the FX lines.
int16_t Wrap16(int64_t sample, int64_t value)
{
    return static_cast<int16_t>(static_cast<uint16_t>(sample + value));
}

// The product of a coefficient and a sample, doubled in 32 bits. The FX lines' filters form each term this way.
int64_t Doubled32(int32_t coefficient, int32_t sample)
{
    const uint32_t product = static_cast<uint32_t>(coefficient * sample);

    return static_cast<int32_t>(product << 1);
}

// An 8-tap filter in place over the 88 samples of `samples`: each of the first 80 becomes the sum of the products of
// `coefficients` and the 8 samples from it on, rounded at bit 16, in 16 bits.
void Fir8(const uint16_t* coefficients, int16_t* samples)
{
    for (int i = 0; i < Dsp::kSubframeSamples; i++)
    {
        int64_t sum = 0;
        for (int k = 0; k < 8; k++)
        {
            sum += Doubled32(static_cast<int16_t>(coefficients[k]), samples[i + k]);
        }

        samples[i] = static_cast<int16_t>(static_cast<uint16_t>(RoundQ15(sum / 2)));
    }
}

// An oscillator that reads a table of 64 samples. The phase's top 6 bits are the index, and its low 10 bits the
// fraction. The fraction takes 16 bits as the samples go. Returns the phase after the 80 samples.
uint32_t ReadOscillatorTable(const int16_t* table, uint32_t phase, uint32_t pitch, int16_t* out)
{
    uint32_t fraction = (phase << 6) & 0xffff;
    uint32_t index = phase >> 10;
    for (int i = 0; i < Dsp::kSubframeSamples; i++)
    {
        out[i] = table[index];
        fraction += pitch << 5;
        index += fraction >> 16;
        fraction &= 0xffff;
        if (index >= Dsp::kOscillatorTableSize)
        {
            index -= Dsp::kOscillatorTableSize;
        }
    }

    return fraction >> 6 | index << 10;
}

// Adds a channel's 80 samples to a bus at a volume that moves towards a target over the first 64 samples, in 32
// steps. The volume is the top 16 bits of `level`, and `step` is added to it every 2 samples. Each sum is clamped.
// Returns the level at the end.
uint32_t Ramp(Dsp::Bus& bus, const int16_t* source, uint32_t level, uint32_t step)
{
    const auto volume = [](uint32_t value)
    {
        return int64_t{static_cast<int32_t>(value) >> 16};
    };
    bus[0] = Clamp16(((int64_t{bus[0]} << 16) + source[0] * volume(level)) >> 16);
    for (int i = 1; i < 65; i += 2)
    {
        const int64_t v = volume(level);
        level += step;
        bus[i] = Clamp16(((int64_t{bus[i]} << 16) + source[i] * v) >> 16);
        bus[i + 1] = Clamp16(((int64_t{bus[i + 1]} << 16) + source[i + 1] * v) >> 16);
    }

    const int64_t v = volume(level);
    for (int i = 65; i < Dsp::kSubframeSamples; i++)
    {
        bus[i] = Clamp16(((int64_t{bus[i]} << 16) + source[i] * v) >> 16);
    }

    return level;
}

// Adds `source` times `coefficient` / 32768 to `destination`, sample by sample, in 16 bits.
void MixInto(const int16_t* source, int16_t* destination, int count, int32_t coefficient)
{
    for (int i = 0; i < count; i++)
    {
        destination[i] = Wrap16(destination[i], RoundQ15(static_cast<int64_t>(source[i]) * coefficient));
    }
}

// The matrix's coefficients over 32768.
constexpr int32_t kUnity = 32767;
constexpr int32_t kSurround = 18400;
constexpr int32_t kRear = 26500;
constexpr int32_t kCenter = 23170;
constexpr int32_t kHalf = 16383;

} // namespace

DspMemory::DspMemory(uint32_t size) : bytes_(size)
{
}

uint8_t DspMemory::Byte(uint32_t address) const
{
    return address < bytes_.size() ? bytes_[address] : 0;
}

void DspMemory::SetByte(uint32_t address, uint8_t value)
{
    if (address < bytes_.size())
    {
        bytes_[address] = value;
    }
}

uint16_t DspMemory::Native16(uint32_t address) const
{
    return static_cast<uint16_t>(Byte(address) | Byte(address + 1) << 8);
}

void DspMemory::SetNative16(uint32_t address, uint16_t value)
{
    SetByte(address, static_cast<uint8_t>(value));
    SetByte(address + 1, static_cast<uint8_t>(value >> 8));
}

uint16_t DspMemory::Big16(uint32_t address) const
{
    return static_cast<uint16_t>(Byte(address) << 8 | Byte(address + 1));
}

void DspMemory::SetBig16(uint32_t address, uint16_t value)
{
    SetByte(address, static_cast<uint8_t>(value >> 8));
    SetByte(address + 1, static_cast<uint8_t>(value));
}

uint32_t DspMemory::Native32(uint32_t address) const
{
    return Native16(address) | static_cast<uint32_t>(Native16(address + 2)) << 16;
}

void DspMemory::SetNative32(uint32_t address, uint32_t value)
{
    SetNative16(address, static_cast<uint16_t>(value));
    SetNative16(address + 2, static_cast<uint16_t>(value >> 16));
}

Dsp::Dsp(DspMemory& memory, const Tables& tables, uint32_t flags, const std::array<uint16_t, 14>& bus_addresses)
    : memory_(memory), tables_(tables), flags_(flags), bus_addresses_(bus_addresses)
{
}

void Dsp::Setup(uint16_t channel_count, uint32_t channel_blocks, uint32_t fx_settings)
{
    channel_count_ = channel_count;
    channel_blocks_ = channel_blocks;
    fx_settings_ = fx_settings;

    // The center bus's delay line starts with its read position 150 samples on. The position drifts back from there.
    delay_ = {};
    delay_.read = 150;
    delay_.direction = -1;
    delay_.low = 0x30;
    delay_.high = 0x10;
    delay_.step = 0x7f00;
    delay_.depth = 0x100;
}

void Dsp::SetVaram(uint32_t base)
{
    varam_ = base;
}

void Dsp::StartFrame(uint16_t subframes, uint16_t level, uint32_t left, uint32_t right)
{
    outputs_ = {left, right, 0, 0};
    subframe_ = 0;
    subframe_count_ = subframes;
    level_ = static_cast<int16_t>(level);

    // The FX lines' settings that the CPU side left.
    for (std::size_t line = 0; line < fx_lines_.size(); line++)
    {
        auto& words = fx_lines_[line].words;
        if (fx_layout_ == FxLayout::kJaudio2)
        {
            const uint32_t address = (fx_settings_ & 0x3fffffc) + static_cast<uint32_t>(line * DspFxLine::kBytes);
            for (std::size_t i = 0; i < words.size(); i++)
            {
                words[i] = memory_.Native16(address + static_cast<uint32_t>(2 * i));
            }
            continue;
        }

        // JAudio1's 16 words go to their places in JAudio2's layout: the flags and the length to words 0 and 1, the
        // buffer to 4 and 5, the sends to 8 to 11 and the filter to 12 to 19.
        constexpr std::array<std::size_t, 16> kPlaces = {0, 1, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
        const uint32_t address = (fx_settings_ & 0x3fffffc) + static_cast<uint32_t>(line * 0x20);
        words = {};
        for (std::size_t i = 0; i < kPlaces.size(); i++)
        {
            words[kPlaces[i]] = memory_.Native16(address + static_cast<uint32_t>(2 * i));
        }
    }
}

Dsp::Bus* Dsp::FindBus(uint16_t address)
{
    // The order of the buses in the DSP's memory. The third address names no bus.
    static constexpr std::array<int, 14> kBuses = {0, 1, -1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    for (std::size_t i = 0; i < bus_addresses_.size(); i++)
    {
        if (kBuses[i] >= 0 && bus_addresses_[i] == address)
        {
            return &buses_[static_cast<std::size_t>(kBuses[i])];
        }
    }

    return nullptr;
}

void Dsp::StartSubframe()
{
    // The output buses and the surround buses start empty. The surround buses' last sub-frame goes into the left and
    // right buses through the matrix first.
    std::fill_n(buses_[5].begin(), kSubframeSamples, int16_t{0});
    std::fill_n(buses_[6].begin(), kSubframeSamples, int16_t{0});
    if ((flags_ & kFlagFxDelay) == 0 || (outputs_[2] >> 16) == 0)
    {
        std::array<int16_t, kSubframeSamples> rear{};
        for (int i = 0; i < kSubframeSamples; i++)
        {
            rear[i] = static_cast<int16_t>(static_cast<uint16_t>(RoundQ15(buses_[11][i] * int64_t{kRear})));
        }
        MixInto(rear.data(), buses_[5].data(), kSubframeSamples, kUnity);
        MixInto(rear.data(), buses_[6].data(), kSubframeSamples, -kSurround);

        for (int i = 0; i < kSubframeSamples; i++)
        {
            rear[i] = static_cast<int16_t>(static_cast<uint16_t>(RoundQ15(buses_[4][i] * int64_t{kRear})));
        }
        MixInto(rear.data(), buses_[5].data(), kSubframeSamples, kSurround);
        MixInto(rear.data(), buses_[6].data(), kSubframeSamples, -kUnity);
    }

    for (const std::size_t bus : {std::size_t{4}, std::size_t{11}, std::size_t{12}, std::size_t{1}, std::size_t{0}})
    {
        std::fill_n(buses_[bus].begin(), kSubframeSamples, int16_t{0});
    }

    // Each FX line's bus keeps its last 8 samples as the history of its filter.
    if ((flags_ & kFlagFxDelay) != 0)
    {
        for (std::size_t bus = 7; bus <= 10; bus++)
        {
            std::copy_n(buses_[bus].begin() + 80, 8, buses_[bus].begin());
            std::fill_n(buses_[bus].begin() + 8, kSubframeSamples, int16_t{0});
        }
    }

    StartFxLines();
    UpdateOscillatorTables();
}

void Dsp::UpdateOscillatorTables()
{
    // Table 3, a pair of samples at a time from the start. Each sample of the pair takes its old value and a product
    // of two others. The first takes the sample 2 before it and the old value of the one before it. The second takes
    // the new value of the one before it and the old value of the first. The table wraps around.
    int16_t* noise = tables_.resample.data() + kOscillatorTables + 2 * kOscillatorTableSize;
    int32_t before = noise[kOscillatorTableSize - 1];
    int64_t second = 0;
    for (std::size_t j = 0; j < kOscillatorTableSize; j += 2)
    {
        const int32_t first = noise[(j + kOscillatorTableSize - 2) % kOscillatorTableSize];
        const int16_t old = noise[j];
        const uint32_t product = static_cast<uint32_t>(first * before) - (static_cast<uint32_t>(old) << 16);
        noise[j] = static_cast<int16_t>(product >> 16);
        before = noise[j + 1];
        second = int64_t{noise[(j + kOscillatorTableSize - 1) % kOscillatorTableSize]} * old + (int64_t{before} << 16);
        noise[j + 1] = static_cast<int16_t>(static_cast<uint64_t>(second) >> 15);
    }

    // Table 4, a ramp on from its last sample. The step's size is 0x2000 plus 9 bits of a product: bits 16 to 24 of
    // the last sample plus 0x10000 times the sample before it, rounded half to even at bit 16. The step is negative
    // when the DSP's accumulator holds more than 0. The accumulator holds table 3's last product doubled, and the
    // ramp's last sample in its bits 16 to 31.
    int16_t* ramp = tables_.resample.data() + kOscillatorTables + 3 * kOscillatorTableSize;
    const auto end = static_cast<uint16_t>(ramp[kOscillatorTableSize - 1]);
    const int64_t product = (int64_t{ramp[kOscillatorTableSize - 1]} + 0x10000) * ramp[kOscillatorTableSize - 2];
    const auto low = static_cast<uint32_t>(product);
    const uint32_t round = (product & 0x17fff) != 0 ? (low << 1) & 0x10000 : 0;
    const uint32_t size = 0x2000 | (((round + (low & 0x1ff0000)) >> 16) & 0x1ff);
    const uint64_t accumulator = (static_cast<uint64_t>(second) << 1 & 0xffffffff0000ffffull) | uint64_t{end} << 16;
    const uint32_t step = static_cast<int64_t>(accumulator) > 0 ? 0u - size : size;
    for (std::size_t i = 0; i < kOscillatorTableSize; i++)
    {
        ramp[i] = static_cast<int16_t>(static_cast<uint16_t>(end + static_cast<uint32_t>(i + 1) * step));
    }
}

void Dsp::ReadFxLine(const DspFxLine& line, uint16_t position, Bus& bus)
{
    // The delayed sends, 80 samples written `Subframes()` sub-frames ago, go in after the 8 samples of history.
    const uint32_t address = (line.Buffer() + position * 0xa0u) & 0x7fffffc;
    for (int i = 0; i < kSubframeSamples; i++)
    {
        const auto delayed = static_cast<int16_t>(memory_.Big16(address + static_cast<uint32_t>(2 * i)));
        bus[8 + i] = Wrap16(bus[8 + i], RoundQ15(delayed * int64_t{kUnity}));
    }

    if ((line.Flags() & 1) != 0)
    {
        Fir8(&line.words[12], bus.data());
    }

    // The line's output to its two buses.
    for (int send = 0; send < 2; send++)
    {
        const uint16_t target = line.words[8 + 2 * send];
        if (target == 0)
        {
            continue;
        }

        Bus* destination = FindBus(target);
        if (destination != nullptr)
        {
            MixInto(bus.data(), destination->data(), kSubframeSamples, static_cast<int16_t>(line.words[9 + 2 * send]));
        }
    }

    if ((line.Flags() & 2) != 0)
    {
        Fir8(&line.words[12], bus.data());
    }
}

void Dsp::StartFxLines()
{
    for (std::size_t line = 0; line < fx_lines_.size(); line++)
    {
        if (fx_lines_[line].Flags() == 0)
        {
            continue;
        }

        if ((flags_ & kFlagFxDelay) == 0)
        {
            std::copy_n(buses_[7 + line].begin() + 80, 8, buses_[7 + line].begin());
        }
        ReadFxLine(fx_lines_[line], fx_positions_[line], buses_[7 + line]);
    }

    if ((flags_ & kFlagMatrix) == 0)
    {
        return;
    }

    // The surround buses 2 and 3 feed FX lines 2 and 3 through the matrix. Bus 3 reaches them 40 samples late: its
    // first half at the sub-frame's end (FinishFxLines), its second half here.
    if ((flags_ & kFlagFxDelay) == 0 || (outputs_[2] >> 16) == 0)
    {
        MixInto(buses_[2].data(), buses_[9].data(), kSubframeSamples, kUnity);
        MixInto(buses_[2].data(), buses_[10].data(), kSubframeSamples, -kSurround);
        MixInto(buses_[3].data() + 40, buses_[9].data(), 40, -kSurround);
        MixInto(buses_[3].data() + 40, buses_[10].data(), 40, kUnity);
    }
    std::fill_n(buses_[2].begin(), kSubframeSamples, int16_t{0});
    std::fill_n(buses_[3].begin(), kSubframeSamples, int16_t{0});
}

void Dsp::DelayCenter()
{
    // Bus 1 goes through a delay line of two halves of 80 samples. The line is read at a rate that drifts between
    // 0.992 and 1.008 samples a sample. The delay therefore wanders between bounds.
    CenterDelay& d = delay_;
    std::copy_n(buses_[1].begin(), kSubframeSamples, d.samples.begin() + d.write);
    d.write = d.write == 0 ? 80 : 0;

    const uint32_t read = d.read;
    uint32_t distance = 0xa0 - read;
    if ((distance & 0xffff) > 0x4f)
    {
        distance = 0x50 - read;
    }

    if (d.direction < 0)
    {
        if (d.low <= (distance & 0xffff))
        {
            d.step = static_cast<uint16_t>(d.depth ^ 0x8000);
            d.direction = static_cast<int16_t>(-d.direction);
        }
    }
    else if (d.high >= (distance & 0xffff))
    {
        d.step = static_cast<uint16_t>(-0x8000 - d.depth);
        d.direction = static_cast<int16_t>(-d.direction);
    }

    const uint32_t step = static_cast<uint32_t>(d.step) << 1;
    uint32_t position = d.read;
    uint32_t fraction = d.fraction;
    for (int i = 0; i < kSubframeSamples; i++)
    {
        // A position past the line reads past it in the emulation. The model reads 0 there. No channel of the Galaxy
        // games reaches bus 1. The line only ever holds 0 there.
        const uint32_t index = position & 0xffff;
        const int16_t sample = index < d.samples.size() ? d.samples[index] : int16_t{0};
        fraction = step + (fraction & 0xffff);
        position += fraction >> 16;
        buses_[1][i] = sample;
        if ((position & 0xffff) >= 0xa0)
        {
            position -= 0xa0;
        }
    }

    // The last step's fraction comes back off.
    fraction = (fraction & 0xffff) - (step & 0xfffe);
    position += fraction >> 16;
    d.fraction = static_cast<uint16_t>(fraction);
    d.read = static_cast<uint16_t>(position);

    MixInto(buses_[1].data(), buses_[6].data(), kSubframeSamples, kHalf);
    MixInto(buses_[1].data(), buses_[5].data(), kSubframeSamples, kHalf);
}

void Dsp::FinishFxLines()
{
    MixInto(buses_[3].data(), buses_[9].data() + 40, 40, -kSurround);
    MixInto(buses_[3].data(), buses_[10].data() + 40, 40, kUnity);

    // Each FX line's bus goes into its delay. The delay then moves on a sub-frame.
    for (std::size_t line = 0; line < fx_lines_.size(); line++)
    {
        const DspFxLine& settings = fx_lines_[line];
        if (settings.Flags() == 0)
        {
            continue;
        }

        uint16_t& position = fx_positions_[line];
        const uint32_t address = (settings.Buffer() + position * 0xa0u) & 0x7fffffc;
        for (int i = 0; i < kSubframeSamples; i++)
        {
            memory_.SetBig16(address + static_cast<uint32_t>(2 * i), static_cast<uint16_t>(buses_[7 + line][i]));
        }
        position =
            settings.Subframes() <= static_cast<uint16_t>(position + 1) ? 0 : static_cast<uint16_t>(position + 1);
    }
}

void Dsp::EndSubframe()
{
    DelayCenter();
    MixInto(buses_[0].data(), buses_[5].data(), kSubframeSamples, kCenter);
    MixInto(buses_[0].data(), buses_[6].data(), kSubframeSamples, kCenter);
    if ((flags_ & kFlagMatrix) != 0)
    {
        MixInto(buses_[4].data(), buses_[5].data() + 40, 40, -kSurround);
        MixInto(buses_[4].data(), buses_[6].data() + 40, 40, kUnity);
    }

    // The output: the left bus, and the right bus less bus 12, each at the mixer's level and clamped.
    for (int i = 0; i < kSubframeSamples; i++)
    {
        buses_[5][i] = Clamp16((buses_[5][i] * int32_t{level_}) >> 12);
    }
    const uint32_t left = outputs_[0] & 0x7fffffc;
    for (int i = 0; i < kSubframeSamples; i++)
    {
        memory_.SetBig16(left + static_cast<uint32_t>(2 * i), static_cast<uint16_t>(buses_[5][i]));
    }

    for (int i = 0; i < kSubframeSamples; i++)
    {
        buses_[6][i] = Wrap16(buses_[6][i], -int64_t{buses_[12][i]});
    }
    for (int i = 0; i < kSubframeSamples; i++)
    {
        buses_[6][i] = Clamp16((buses_[6][i] * int32_t{level_}) >> 12);
    }
    const uint32_t right = outputs_[1] & 0x7fffffc;
    for (int i = 0; i < kSubframeSamples; i++)
    {
        memory_.SetBig16(right + static_cast<uint32_t>(2 * i), static_cast<uint16_t>(buses_[6][i]));
    }

    outputs_[0] += 0xa0;
    outputs_[1] += 0xa0;
    FinishFxLines();
    subframe_++;
}

void Dsp::RenderSubframe()
{
    StartSubframe();
    for (uint16_t channel = 0; channel < channel_count_; channel++)
    {
        ProcessChannel(channel);
    }
    EndSubframe();
}

void Dsp::DecodeAfc(DspChannelBlock& block, std::span<int16_t> destination, int blocks)
{
    int64_t history2 = block.S16(DspChannelBlock::kAdpcmHistory2);
    int64_t history1 = block.S16(DspChannelBlock::kAdpcmHistory1);
    const bool two_bit = block.U16(DspChannelBlock::kFormat) == 5;
    std::size_t at = 0;
    for (int b = 0; b < (blocks & 0xffff); b++)
    {
        // The block's bytes, from the line of 32 bytes that holds its start.
        const uint32_t address = block.U32(DspChannelBlock::kReadAddress);
        const uint32_t line = (varam_ + (address & 0x7ffffe0)) & 0x7fffffc;
        const uint32_t start = line + (address & 0x1f);
        block.SetU32(DspChannelBlock::kReadAddress, address + block.U16(DspChannelBlock::kFormat));

        const uint8_t header = memory_.Byte(start);
        const int scale = header >> 4;
        const int64_t c0 = tables_.adpcm[2 * (header & 0xf)];
        const int64_t c1 = tables_.adpcm[2 * (header & 0xf) + 1];

        for (int s = 0; s < 16; s++)
        {
            int32_t raw = 0;
            if (two_bit)
            {
                const uint8_t byte = memory_.Byte(start + 1 + static_cast<uint32_t>(s / 4));
                raw = static_cast<int16_t>(static_cast<uint16_t>(((byte >> (6 - 2 * (s % 4))) & 3) << 14));
            }
            else
            {
                const uint8_t byte = memory_.Byte(start + 1 + static_cast<uint32_t>(s / 2));
                raw = static_cast<int16_t>(static_cast<uint16_t>(((s % 2 == 0 ? byte >> 4 : byte) & 0xf) << 12));
            }

            const int64_t delta = scale >= 1 ? int64_t{raw} << (scale - 1) : int64_t{raw} >> 1;
            const int16_t sample = Clamp16((c0 * history1 + c1 * history2 + delta) >> 11);
            if (at < destination.size())
            {
                destination[at] = sample;
            }
            at++;
            history2 = history1;
            history1 = sample;
        }
    }

    block.SetU16(DspChannelBlock::kAdpcmHistory2, static_cast<uint16_t>(history2));
    block.SetU16(DspChannelBlock::kAdpcmHistory1, static_cast<uint16_t>(history1));
}

void Dsp::FetchPcm(DspChannelBlock& block, int16_t* destination, uint32_t bytes)
{
    const uint32_t needed = static_cast<uint32_t>(
        (uint64_t{block.U16(DspChannelBlock::kPitch)} * 80 + block.U16(DspChannelBlock::kFraction)) >> 12);

    // Reads `count` samples from the read address on: big-endian ones of 16 bits, or bytes in the high half of a
    // sample (0x175a04 and 0x2001f0 in the main of Pikmin's Switch release).
    const auto read = [&](int16_t* out, uint32_t count)
    {
        const uint32_t address = block.U32(DspChannelBlock::kReadAddress);
        const uint32_t line = (varam_ + (address & 0xffffffe0)) & 0x7fffffc;
        if (bytes == 1)
        {
            for (uint32_t i = 0; i < count; i++)
            {
                out[i] = static_cast<int16_t>(memory_.Byte(line + (address & 0x1f) + i) << 8);
            }
            return;
        }

        const uint32_t offset = (address >> 1) & 0xf;
        for (uint32_t i = 0; i < count; i++)
        {
            out[i] = static_cast<int16_t>(memory_.Big16(line + (offset + i) * 2));
        }
    };

    // A channel's start, or a loop that the last sub-frame reached the end of.
    if (block.U16(DspChannelBlock::kReset) != 0)
    {
        block.SetU32(DspChannelBlock::kRemaining, block.U32(DspChannelBlock::kLoopEnd));
        block.SetU32(DspChannelBlock::kPcmPosition, 0);
        block.SetU16(DspChannelBlock::kLoopPending, 0);
        block.SetU32(DspChannelBlock::kReadAddress, block.U32(DspChannelBlock::kWaveAddress));
    }
    else if (block.U16(DspChannelBlock::kLoopPending) != 0)
    {
        block.SetU16(DspChannelBlock::kLoopPending, 0);
        if (block.U16(DspChannelBlock::kLoops) == 0)
        {
            std::fill_n(destination, needed, int16_t{0});
            block.SetU16(DspChannelBlock::kFinished, 1);

            return;
        }

        const uint32_t loop_start = block.U32(DspChannelBlock::kLoopStart);
        block.SetU32(DspChannelBlock::kPcmPosition, loop_start);
        block.SetU32(DspChannelBlock::kRemaining, block.U32(DspChannelBlock::kLoopEnd) - loop_start);
        block.SetU32(DspChannelBlock::kReadAddress, block.U32(DspChannelBlock::kWaveAddress) + loop_start * bytes);
    }

    // The wave's end, or its loop's, within the sub-frame. `wanted` counts the samples still to read. `step` is what
    // the read address and the remaining count move by.
    int16_t* out = destination;
    uint32_t wanted = needed;
    uint32_t step = needed;
    uint32_t remaining = block.U32(DspChannelBlock::kRemaining);
    while (remaining <= step)
    {
        const uint32_t count = remaining & 0xffff;
        read(out, count);
        out += count;
        wanted -= block.U16(DspChannelBlock::kRemaining);
        if (block.U16(DspChannelBlock::kLoops) == 0)
        {
            std::fill_n(out, wanted & 0xffff, int16_t{0});
            block.SetU16(DspChannelBlock::kFinished, 1);

            return;
        }

        const uint32_t loop_start = block.U32(DspChannelBlock::kLoopStart);
        step = wanted & 0xffff;
        block.SetU16(DspChannelBlock::kLoopPending, 0);
        block.SetU32(DspChannelBlock::kPcmPosition, loop_start);
        remaining = block.U32(DspChannelBlock::kLoopEnd) - loop_start;
        block.SetU32(DspChannelBlock::kRemaining, remaining);
        block.SetU32(DspChannelBlock::kReadAddress, block.U32(DspChannelBlock::kWaveAddress) + loop_start * bytes);
        if (remaining > step)
        {
            break;
        }
    }

    read(out, wanted & 0xffff);
    const uint32_t left = block.U32(DspChannelBlock::kRemaining) - step;
    block.SetU32(DspChannelBlock::kRemaining, left);
    if (left == 0)
    {
        block.SetU16(DspChannelBlock::kLoopPending, 1);
    }
    block.SetU32(DspChannelBlock::kReadAddress, block.U32(DspChannelBlock::kReadAddress) + step * bytes);
}

void Dsp::Resample(DspChannelBlock& block, int16_t* source, int16_t* out)
{
    for (int k = 0; k < 4; k++)
    {
        source[k] = block.S16(DspChannelBlock::kHistory + 2 * static_cast<std::size_t>(k));
    }

    const uint32_t pitch = block.U16(DspChannelBlock::kPitch);
    const uint32_t step = pitch << 4;
    uint32_t position = static_cast<uint32_t>(block.U16(DspChannelBlock::kFraction)) << 4;
    uint16_t last = 0;
    if (pitch >= 0x4000)
    {
        // A step of 4 samples or more takes the nearest sample before each position.
        for (int i = 0; i < kSubframeSamples; i++)
        {
            last = static_cast<uint16_t>(source[position >> 16]);
            out[i] = static_cast<int16_t>(last);
            position += step;
        }
    }
    else
    {
        // A filter of 4 taps, its phase from the top 6 bits of the position's fraction.
        for (int i = 0; i < kSubframeSamples; i++)
        {
            const int16_t* s = source + (position >> 16);
            const int16_t* c = tables_.resample.data() + ((position >> 8) & 0xfc);
            const int64_t sum =
                int64_t{s[0]} * c[0] + int64_t{s[1]} * c[1] + int64_t{s[2]} * c[2] + int64_t{s[3]} * c[3];
            const int64_t rounded = RoundQ15(sum);
            out[i] = Clamp16(rounded);
            last = static_cast<uint16_t>(rounded);
            position += step;
        }
    }

    block.SetU16(DspChannelBlock::kLastSample, last);
    const int16_t* next = source + (position >> 16);
    for (int k = 0; k < 4; k++)
    {
        block.SetU16(DspChannelBlock::kHistory + 2 * static_cast<std::size_t>(k), static_cast<uint16_t>(next[k]));
    }
    block.SetU16(DspChannelBlock::kFraction, (position >> 4) & 0xfff);
}

void Dsp::ProcessAfc(DspChannelBlock& block, std::span<int16_t> source)
{
    const uint32_t advance =
        static_cast<uint32_t>(block.U16(DspChannelBlock::kPitch)) * 80 + block.U16(DspChannelBlock::kFraction);
    uint32_t leftover = block.U16(DspChannelBlock::kLeftover);
    if (block.U16(DspChannelBlock::kReset) != 0)
    {
        leftover = 0;
        block.SetU16(DspChannelBlock::kLeftover, 0);
        block.SetU32(DspChannelBlock::kAdpcmHistory2, 0);
        resets_++;
        block.SetU32(DspChannelBlock::kRemaining, block.U32(DspChannelBlock::kLoopEnd));
        block.SetU32(DspChannelBlock::kReadAddress, block.U32(DspChannelBlock::kWaveAddress));
    }

    // The samples of the saved block, the last one decoded, that the last sub-frame left.
    const auto saved = [&](uint32_t index) -> uint16_t&
    {
        return block.words[DspChannelBlock::kSavedBlock / 2 + index];
    };

    uint32_t needed = advance >> 12;
    std::size_t at = 4;
    for (;;)
    {
        if ((leftover & 0xffff) != 0)
        {
            const uint32_t first = (16 - leftover) & 0xffff;
            if (first <= 15)
            {
                for (uint32_t k = first; k < 16; k++)
                {
                    source[at++] = static_cast<int16_t>(saved(k));
                }
            }
        }

        if ((needed & 0xffff) <= (leftover & 0xffff))
        {
            block.SetU16(DspChannelBlock::kLeftover, leftover - needed);
            break;
        }

        needed -= leftover;
        const uint32_t remaining = block.U32(DspChannelBlock::kRemaining);
        if (remaining < (needed & 0xffff))
        {
            // The wave's end or its loop's: decode what's left of it, then go back to the loop's start.
            if (remaining != 0)
            {
                needed -= remaining;
                DecodeAfc(block, std::span<int16_t>(source).subspan(at), static_cast<int>((remaining + 15) >> 4));
                at += block.U16(DspChannelBlock::kRemaining);
            }

            if (block.U16(DspChannelBlock::kLoops) == 0)
            {
                block.SetU16(DspChannelBlock::kFinished, 1);
                const std::size_t count =
                    std::min<std::size_t>(needed & 0xffff, source.size() - std::min(at, source.size()));
                std::fill_n(source.begin() + static_cast<std::ptrdiff_t>(std::min(at, source.size())), count,
                            int16_t{0});
                break;
            }

            const uint32_t loop_start = block.U32(DspChannelBlock::kLoopStart);
            block.SetU32(DspChannelBlock::kReadAddress, static_cast<uint32_t>(static_cast<int32_t>(loop_start) >> 4) *
                                                                block.U16(DspChannelBlock::kFormat) +
                                                            block.U32(DspChannelBlock::kWaveAddress));
            const uint32_t loop_history = block.U32(DspChannelBlock::kLoopLast);
            block.SetU32(DspChannelBlock::kAdpcmHistory2, loop_history >> 16 | loop_history << 16);

            std::array<int16_t, 16> first_block{};
            DecodeAfc(block, first_block, 1);
            for (uint32_t k = 0; k < 16; k++)
            {
                saved(k) = static_cast<uint16_t>(first_block[k]);
            }

            leftover = 16 - (loop_start & 0xf);
            block.SetU16(DspChannelBlock::kLeftover, leftover);
            block.SetU32(DspChannelBlock::kRemaining, block.U32(DspChannelBlock::kLoopEnd) - loop_start - leftover);
            continue;
        }

        // Whole blocks, the last of them saved for the samples left over. When the wave's end or its loop's falls in
        // them, only the samples before it are left over.
        const uint32_t rounded = (needed + 15) & 0xfffffff0;
        const auto signed_rounded = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(rounded)));
        if (remaining <= signed_rounded)
        {
            block.SetU32(DspChannelBlock::kRemaining, 0);
            leftover = remaining - needed;
        }
        else
        {
            block.SetU32(DspChannelBlock::kRemaining, remaining - signed_rounded);
            leftover = rounded - needed;
        }
        block.SetU16(DspChannelBlock::kLeftover, leftover);
        const int blocks = static_cast<int16_t>(static_cast<uint16_t>(rounded)) >> 4;
        const std::size_t start = at;
        DecodeAfc(block, std::span<int16_t>(source).subspan(at), blocks);
        if (block.U16(DspChannelBlock::kLeftover) != 0)
        {
            const std::size_t end = start + static_cast<std::size_t>(static_cast<int16_t>(blocks << 4));
            for (uint32_t k = 0; k < 16; k++)
            {
                saved(k) = static_cast<uint16_t>(source[end - 16 + k]);
            }

            // At the wave's end, its last samples move to the saved block's end. The next sub-frame takes the leftover
            // from there.
            const uint32_t end_sample = block.U32(DspChannelBlock::kLoopEnd);
            if (block.U32(DspChannelBlock::kRemaining) == 0 && (end_sample & 0xffff) != 0)
            {
                const auto last = static_cast<int>((end_sample - 1) & 0xf);
                const int first = static_cast<int>(DspChannelBlock::kSavedBlock / 2);
                int k = 0;
                do
                {
                    block.words[static_cast<std::size_t>(first + 15 - k)] =
                        block.words[static_cast<std::size_t>(first + last - k)];
                    k++;
                } while (k < block.U16(DspChannelBlock::kLeftover));
            }
        }
        break;
    }
}

void Dsp::Dist(DspChannelBlock& block, int16_t* samples)
{
    int64_t previous_out = 0;
    uint32_t previous_in = 0;
    if (block.U16(DspChannelBlock::kReset) != 0)
    {
        block.SetU32(DspChannelBlock::kDistState, 0);
    }
    else
    {
        previous_out = block.S16(DspChannelBlock::kDistState);
        previous_in = block.U16(DspChannelBlock::kDistState + 2);
    }

    const int64_t amount = block.S16(DspChannelBlock::kDist);
    for (int i = 0; i < kSubframeSamples; i++)
    {
        const auto in = static_cast<uint16_t>(samples[i]);
        const int64_t difference = static_cast<int16_t>(static_cast<uint16_t>(in - previous_in));
        const int16_t out = Clamp16(((amount * difference << 9) + (previous_out << 16)) >> 16);
        samples[i] = out;
        previous_out = out;
        previous_in = in;
    }

    block.SetU16(DspChannelBlock::kDistState, static_cast<uint16_t>(previous_out));
    block.SetU16(DspChannelBlock::kDistState + 2, previous_in);
}

void Dsp::Fir(DspChannelBlock& block, int16_t* samples)
{
    // `samples` starts with the filter's history, then the 80 samples. Each sample becomes the sum over the taps from
    // it on. The 80 outputs thus end up where the history and the first samples were.
    const uint32_t taps = block.U16(DspChannelBlock::kFilterMode) & 0x1f;
    for (int i = 0; i < kSubframeSamples; i++)
    {
        int64_t sum = 0;
        for (uint32_t k = 0; k < taps; k++)
        {
            sum += int64_t{block.S16(DspChannelBlock::kFirCoefficients + 2 * k)} * samples[i + static_cast<int>(k)] * 2;
        }

        samples[i] = static_cast<int16_t>(static_cast<uint16_t>(RoundQ15(sum / 2)));
    }
}

void Dsp::Iir(DspChannelBlock& block, int16_t* samples)
{
    // y[n] from x[n-1], x[n-2], y[n-1] and y[n-2]. The output runs one sample behind the input.
    const int64_t c0 = block.S16(DspChannelBlock::kIirCoefficients);
    const int64_t c1 = block.S16(DspChannelBlock::kIirCoefficients + 2);
    const int64_t c2 = block.S16(DspChannelBlock::kIirCoefficients + 4);
    const int64_t c3 = block.S16(DspChannelBlock::kIirCoefficients + 6);
    int64_t in1 = block.S16(DspChannelBlock::kIirState);
    int64_t in2 = block.S16(DspChannelBlock::kIirState + 2);
    int64_t out1 = block.S16(DspChannelBlock::kIirState + 4);
    int64_t out2 = block.S16(DspChannelBlock::kIirState + 6);
    for (int i = 0; i < kSubframeSamples; i++)
    {
        const int64_t in = samples[i];
        const int16_t out = Clamp16((c0 * in1 + c1 * in2 + c2 * out1 + c3 * out2) >> 15);
        samples[i] = out;
        in2 = in1;
        in1 = in;
        out2 = out1;
        out1 = out;
    }

    block.SetU16(DspChannelBlock::kIirState, static_cast<uint16_t>(in1));
    block.SetU16(DspChannelBlock::kIirState + 2, static_cast<uint16_t>(in2));
    block.SetU16(DspChannelBlock::kIirState + 4, static_cast<uint16_t>(out1));
    block.SetU16(DspChannelBlock::kIirState + 6, static_cast<uint16_t>(out2));
}

void Dsp::Mix(DspChannelBlock& block, std::span<int16_t> work, int offset)
{
    // The samples at `offset` in the output. The mixers' delays read the samples that the last sub-frame kept before
    // them. The emulation reads out of bounds for a delay that reaches past the start of the output. The model reads
    // the source there. JAudio's delays are 0 in stereo.
    int16_t* buffer = work.data() + kSourceSamples;
    const auto inside = [&](int index)
    {
        return index >= -static_cast<int>(kSourceSamples) && index < static_cast<int>(work.size() - kSourceSamples);
    };
    const uint16_t kept = block.U16(DspChannelBlock::kDelayCount);
    for (int i = 0; i < kept; i++)
    {
        const int word = static_cast<int>(DspChannelBlock::kDelayHistory / 2) + 15 - i;
        if (inside(offset - 1 - i) && word >= 0)
        {
            buffer[offset - 1 - i] = static_cast<int16_t>(block.words[static_cast<std::size_t>(word)]);
        }
    }

    // A forced stop halves each mixer's volume a sub-frame. It ends the channel once they're all at 0.
    if (block.U16(DspChannelBlock::kForcedStop) != 0)
    {
        int32_t sum = 0;
        for (std::size_t k = 0; k < 6; k++)
        {
            const int16_t volume = block.S16(DspChannelBlock::kMixers + 8 * k + 4);
            block.SetU16(DspChannelBlock::kMixers + 8 * k + 2, static_cast<uint16_t>(volume >> 1));
            sum += volume;
        }
        if ((sum & 0x1ffff) == 0)
        {
            block.SetU16(DspChannelBlock::kFinished, 1);
        }
    }

    for (std::size_t k = 0; k < 6; k++)
    {
        const std::size_t mixer = DspChannelBlock::kMixers + 8 * k;
        const uint16_t delays = block.U16(mixer + 6);
        const uint32_t delay = delays & 0xff;
        if ((delays >> 8) != delay)
        {
            // The delay moves a sample a sub-frame towards its target.
            block.SetU16(mixer + 6, (delays >> 8) <= delay ? delays - 1u : delays + 1u);
        }

        const uint16_t address = block.U16(mixer);
        if (address == 0)
        {
            continue;
        }

        Bus* bus = FindBus(address);
        if (bus == nullptr)
        {
            continue;
        }

        const int32_t volume = block.S16(mixer + 4);
        const int32_t target = block.S16(mixer + 2);
        const auto step = static_cast<uint32_t>(target - volume) << 11;
        const uint32_t level = static_cast<uint32_t>(volume) << 16;
        if ((step | level) == 0)
        {
            block.SetU16(mixer + 4, 0);
            continue;
        }

        const int16_t* source = buffer + (offset - static_cast<int>(delay));
        block.SetU16(mixer + 4, Ramp(*bus, source, level, step) >> 16);
    }

    for (int i = 0; i < kept; i++)
    {
        const int word = static_cast<int>(DspChannelBlock::kDelayHistory / 2) + 15 - i;
        if (inside(offset + kSubframeSamples - 1 - i) && word >= 0)
        {
            block.words[static_cast<std::size_t>(word)] =
                static_cast<uint16_t>(buffer[offset + kSubframeSamples - 1 - i]);
        }
    }
}

void Dsp::AutoMix(DspChannelBlock& block, const int16_t* samples)
{
    using B = DspChannelBlock;
    const int32_t volume = block.S16(B::kAutoVolume);

    // A forced stop halves the volume a sub-frame. It ends the channel once the volume is 0.
    if (block.U16(B::kForcedStop) != 0)
    {
        block.SetU16(B::kAutoTarget, static_cast<uint16_t>(volume / 2));
        if (volume == 0)
        {
            block.SetU16(B::kFinished, 1);
        }
    }

    // The gains of the left and right buses and the two surround buses. The pan and the surround level each pick a
    // value of a quarter of a sine and of its mirror. The CPU side sets a pan of 127 at most. The emulation reads past
    // its table for a higher one.
    const bool wide = (flags_ & kFlagAutoMixer16) != 0;
    const int shift = wide ? 16 : 15;
    const uint16_t setting = block.U16(B::kAutoPanDolby);
    const uint32_t pan = std::min<uint32_t>(setting >> 8, 127);
    const uint32_t dolby = setting & 0x7fu;
    const auto sine = [&](uint32_t index)
    {
        return int32_t{tables_.resample[kAutoMixerSine + index]};
    };
    const auto gain = [&](int32_t a, int32_t b)
    {
        return std::min((a * b) >> shift, 0x7fff);
    };
    const std::array<int32_t, 4> gains = {
        gain(sine(dolby ^ 0x7f), sine(pan ^ 0x7f)),
        gain(sine(dolby ^ 0x7f), sine(pan)),
        gain(sine(pan ^ 0x7f), sine(dolby)),
        gain(sine(dolby), sine(pan)),
    };

    // The volume moves to its target over the first 64 samples, as a mixer's does. The sends scale each bus's volume
    // and step by the send's level. The emulation rounds the two differently.
    const int32_t fxmix = block.S16(B::kAutoFxmix);
    const int32_t send = std::min((fxmix * volume) >> 15, 0x7fff);
    const int32_t target = block.S16(B::kAutoTarget);
    const int32_t delta = std::clamp(target - volume, -0x8000, 0x7fff);
    block.SetU16(B::kAutoVolume, static_cast<uint16_t>(target));

    // The dry buses: left, right and the two surround buses. Then the sends: the buses of FX lines 2 and 3, and the two
    // surround buses that feed them through the matrix.
    static constexpr std::array<std::size_t, 4> kDry = {5, 6, 11, 4};
    static constexpr std::array<std::size_t, 4> kSends = {9, 10, 2, 3};
    std::array<uint32_t, 8> levels{};
    std::array<uint32_t, 8> steps{};
    for (std::size_t k = 0; k < 4; k++)
    {
        const int32_t dry = (gains[k] * volume) >> shift;
        const int32_t step = (gains[k] * delta) >> shift;
        const int32_t base = wide ? dry : gains[k];
        levels[k] = static_cast<uint32_t>(static_cast<uint16_t>(dry)) << 16;
        steps[k] = static_cast<uint32_t>(static_cast<int16_t>(step)) << 11;
        levels[4 + k] = (static_cast<uint32_t>(send * base) >> shift) << 16;
        steps[4 + k] = static_cast<uint32_t>(static_cast<int16_t>((step * fxmix) >> shift)) << 11;
    }
    for (std::size_t k = 0; k < 8; k++)
    {
        if ((levels[k] | steps[k]) != 0)
        {
            Ramp(buses_[k < 4 ? kDry[k] : kSends[k - 4]], samples, levels[k], steps[k]);
        }
    }
}

bool Dsp::Oscillate(DspChannelBlock& block, int16_t* out) const
{
    // A square wave and a pulse go between these levels.
    constexpr int16_t kHigh = 0x4000;

    const uint32_t pitch = block.U16(DspChannelBlock::kPitch);
    uint32_t phase = block.U16(DspChannelBlock::kFraction);
    const auto table = [this](std::size_t number)
    {
        return tables_.resample.data() + kOscillatorTables + number * kOscillatorTableSize;
    };
    switch (block.U16(DspChannelBlock::kFormat))
    {
    case 0:
        {
            // A square wave, high while the phase's top bit is set.
            uint32_t doubled = phase << 1;
            for (int i = 0; i < kSubframeSamples; i++)
            {
                out[i] = (doubled & 0x10000) != 0 ? kHigh : static_cast<int16_t>(-kHigh);
                doubled += pitch;
            }
            phase = doubled >> 1;
            break;
        }

    case 1:
        // A sawtooth: each sample is the phase.
        for (int i = 0; i < kSubframeSamples; i++)
        {
            out[i] = static_cast<int16_t>(static_cast<uint16_t>(phase));
            phase += pitch >> 1;
        }
        break;

    case 3:
        {
            // A pulse, high while the phase's top 2 bits are set.
            uint32_t quadrupled = phase << 2;
            for (int i = 0; i < kSubframeSamples; i++)
            {
                out[i] = (quadrupled & 0x30000) == 0x30000 ? kHigh : static_cast<int16_t>(-kHigh);
                quadrupled += pitch << 1;
            }
            phase = quadrupled >> 2;
            break;
        }

    case 6:
        // The pitch itself. The phase stays.
        std::fill_n(out, kSubframeSamples, static_cast<int16_t>(pitch));
        break;

    case 7:
        phase = ReadOscillatorTable(table(0), phase, pitch, out); // the sine
        break;

    case 4:
        phase = ReadOscillatorTable(table(1), phase, pitch, out); // the triangle
        break;

    case 11:
        phase = ReadOscillatorTable(table(2), phase, pitch, out);
        break;

    case 12:
        phase = ReadOscillatorTable(table(3), phase, pitch, out);
        break;

    default:
        return false;
    }

    block.SetU16(DspChannelBlock::kFraction, phase);
    return true;
}

void Dsp::ProcessChannel(uint16_t index)
{
    const uint32_t address = (channel_blocks_ + index * static_cast<uint32_t>(DspChannelBlock::kBytes)) & 0x3fffffc;
    DspChannelBlock block;
    for (std::size_t i = 0; i < block.words.size(); i++)
    {
        block.words[i] = memory_.Native16(address + static_cast<uint32_t>(2 * i));
    }

    if (block.U16(DspChannelBlock::kActive) == 0 || block.U16(DspChannelBlock::kFinished) != 0)
    {
        return;
    }

    std::array<int16_t, kWorkSamples> work{};
    const std::span<int16_t> source = std::span<int16_t>(work).first(kSourceSamples);
    int16_t* out = work.data() + kSourceSamples;
    int16_t* output = out + 32;
    if (block.U16(DspChannelBlock::kPause) != 0)
    {
        std::fill_n(output, kSubframeSamples, block.S16(DspChannelBlock::kLastSample));
    }
    else
    {
        switch (block.U16(DspChannelBlock::kFormat))
        {
        case 5:
        case 9:
            if ((flags_ & kFlagFastPaths) == 0 || (flags_ & kFlagDirectFetch) == 0)
            {
                return;
            }
            ProcessAfc(block, source);
            Resample(block, source.data(), output);
            break;

        case 16:
            if ((flags_ & kFlagDirectFetch) == 0)
            {
                return;
            }
            FetchPcm(block, source.data() + 4, 2);
            Resample(block, source.data(), output);
            break;

        case 8:
            if ((flags_ & kFlagDirectFetch) == 0)
            {
                return;
            }
            FetchPcm(block, source.data() + 4, 1);
            Resample(block, source.data(), output);
            break;

        default:
            if (!Oscillate(block, output))
            {
                return;
            }
            break;
        }
    }

    if (block.U16(DspChannelBlock::kDist) != 0)
    {
        Dist(block, output);
    }

    const uint16_t mode = block.U16(DspChannelBlock::kFilterMode);
    int offset = 32;
    if ((mode & 0x1f) != 0)
    {
        const uint32_t taps = mode & 0x1f;
        offset = 32 - static_cast<int>(taps);
        int16_t* history = out + offset;
        for (uint32_t k = 0; k < taps; k++)
        {
            history[k] = static_cast<int16_t>(block.words[DspChannelBlock::kFirHistory / 2 + k]);
        }
        Fir(block, history);
        for (uint32_t k = 0; k < taps; k++)
        {
            block.words[DspChannelBlock::kFirHistory / 2 + k] = static_cast<uint16_t>(history[k]);
        }
    }

    const uint64_t iir = uint64_t{block.U32(DspChannelBlock::kIirCoefficients)} |
                         uint64_t{block.U32(DspChannelBlock::kIirCoefficients + 4)} << 32;
    if ((mode & 0x20) != 0 && iir != 0x7fff)
    {
        Iir(block, out + offset);
    }

    if ((flags_ & kFlagCopyAutoMixer) != 0)
    {
        block.SetU16(DspChannelBlock::kAutoMixer + 2, block.U16(DspChannelBlock::kAutoMixer));
    }
    if ((flags_ & kFlagAutoMixer) != 0 && block.U16(DspChannelBlock::kAutoMixer) != 0)
    {
        AutoMix(block, out + offset);
    }
    else
    {
        Mix(block, work, offset);
    }

    if (block.U16(DspChannelBlock::kPause) == 0)
    {
        block.SetU16(DspChannelBlock::kReset, 0);
    }
    for (std::size_t i = 0; i < DspChannelBlock::kWrittenBytes / 2; i++)
    {
        memory_.SetNative16(address + static_cast<uint32_t>(2 * i), block.words[i]);
    }
}

} // namespace hsf::jaudio
