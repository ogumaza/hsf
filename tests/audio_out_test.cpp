// SPDX-License-Identifier: MIT

// Tests the command-line tools' audio writer (src/cli/audio_out.h). A WAV file must hold the samples byte for byte
// after a 44-byte header. A FLAC file must decode to the same samples at the same rate. The extension must pick the
// format in any case. A file that can't be written must be reported.
//
// usage: audio_out_test

#include "cli/audio_out.h"

#include <FLAC/stream_decoder.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.h"
#include "common/bytes.h"

namespace
{

namespace fs = std::filesystem;

constexpr uint32_t kRate = 48000;

// A second and a bit of a sweep in the left channel and noise in the right. The samples reach both ends of the range.
std::vector<int16_t> Signal()
{
    std::vector<int16_t> pcm(2 * 50001);
    uint32_t noise = 1;
    for (std::size_t i = 0; i < pcm.size() / 2; i++)
    {
        pcm[2 * i] = static_cast<int16_t>(static_cast<uint16_t>(i * i / 7));
        noise = noise * 1664525u + 1013904223u;
        pcm[2 * i + 1] = static_cast<int16_t>(static_cast<uint16_t>(noise >> 16));
    }
    pcm[0] = -32768;
    pcm[1] = 32767;

    return pcm;
}

std::vector<uint8_t> ReadFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void CheckWav(const fs::path& path, const std::vector<int16_t>& pcm)
{
    const std::vector<uint8_t> file = ReadFile(path);
    if (!Check(file.size() == 44 + 2 * pcm.size(), "the WAV file holds a 44-byte header and the samples"))
    {
        return;
    }

    const uint8_t* p = file.data();
    HSF_CHECK(std::string(p, p + 4) == "RIFF" && hsf::LoadLe32(p + 4) == file.size() - 8);
    HSF_CHECK(std::string(p + 8, p + 16) == "WAVEfmt " && hsf::LoadLe32(p + 16) == 16);
    HSF_CHECK(hsf::LoadLe16(p + 20) == 1 && hsf::LoadLe16(p + 22) == 2);
    HSF_CHECK(hsf::LoadLe32(p + 24) == kRate && hsf::LoadLe32(p + 28) == kRate * 4);
    HSF_CHECK(hsf::LoadLe16(p + 32) == 4 && hsf::LoadLe16(p + 34) == 16);
    HSF_CHECK(std::string(p + 36, p + 40) == "data" && hsf::LoadLe32(p + 40) == 2 * pcm.size());
    bool same = true;
    for (std::size_t i = 0; i < pcm.size(); i++)
    {
        same = same && static_cast<int16_t>(hsf::LoadLe16(p + 44 + 2 * i)) == pcm[i];
    }
    HSF_CHECK(same);
}

// What the decoder found: the stream's format and its samples, interleaved.
struct Decoded
{
    uint32_t rate = 0;
    uint32_t channels = 0;
    uint32_t bits = 0;
    std::vector<int16_t> pcm;
    bool error = false;
};

FLAC__StreamDecoderWriteStatus OnWrite(const FLAC__StreamDecoder*, const FLAC__Frame* frame,
                                       const FLAC__int32* const buffer[], void* data)
{
    auto& d = *static_cast<Decoded*>(data);
    for (uint32_t i = 0; i < frame->header.blocksize; i++)
    {
        for (uint32_t c = 0; c < frame->header.channels; c++)
        {
            d.pcm.push_back(static_cast<int16_t>(buffer[c][i]));
        }
    }

    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void OnMetadata(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* metadata, void* data)
{
    auto& d = *static_cast<Decoded*>(data);
    if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO)
    {
        d.rate = metadata->data.stream_info.sample_rate;
        d.channels = metadata->data.stream_info.channels;
        d.bits = metadata->data.stream_info.bits_per_sample;
    }
}

void OnError(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void* data)
{
    static_cast<Decoded*>(data)->error = true;
}

void CheckFlac(const fs::path& path, const std::vector<int16_t>& pcm)
{
    Decoded d;
    FLAC__StreamDecoder* decoder = FLAC__stream_decoder_new();
    if (!Check(decoder != nullptr, "the FLAC decoder starts"))
    {
        return;
    }

    FLAC__stream_decoder_set_md5_checking(decoder, true);
    const bool opened = FLAC__stream_decoder_init_file(decoder, path.string().c_str(), OnWrite, OnMetadata, OnError,
                                                       &d) == FLAC__STREAM_DECODER_INIT_STATUS_OK;
    const bool decoded = opened && FLAC__stream_decoder_process_until_end_of_stream(decoder);
    const bool md5_matches = decoded && FLAC__stream_decoder_finish(decoder);
    FLAC__stream_decoder_delete(decoder);

    Check(decoded && !d.error, "the FLAC file decodes");
    Check(md5_matches, "the FLAC file's samples match its MD5 signature");
    HSF_CHECK(d.rate == kRate && d.channels == 2 && d.bits == 16);
    Check(d.pcm == pcm, "the FLAC file decodes to the samples written");
}

} // namespace

int main()
{
    const fs::path dir = fs::temp_directory_path() / "hsf_audio_out_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    const std::vector<int16_t> pcm = Signal();
    HSF_CHECK(!hsf::WriteAudio((dir / "out.wav").string(), pcm, kRate));
    CheckWav(dir / "out.wav", pcm);
    HSF_CHECK(!hsf::WriteAudio((dir / "out.flac").string(), pcm, kRate));
    CheckFlac(dir / "out.flac", pcm);

    // The extension picks the format in any case. Anything but .flac is WAV.
    HSF_CHECK(!hsf::WriteAudio((dir / "upper.FLAC").string(), pcm, kRate));
    CheckFlac(dir / "upper.FLAC", pcm);
    HSF_CHECK(!hsf::WriteAudio((dir / "other.raw").string(), pcm, kRate));
    CheckWav(dir / "other.raw", pcm);

    // No samples make a valid file of each kind.
    HSF_CHECK(!hsf::WriteAudio((dir / "empty.wav").string(), {}, kRate));
    CheckWav(dir / "empty.wav", {});
    HSF_CHECK(!hsf::WriteAudio((dir / "empty.flac").string(), {}, kRate));
    CheckFlac(dir / "empty.flac", {});

    // A file in a folder that doesn't exist can't be written.
    HSF_CHECK(hsf::WriteAudio((dir / "missing" / "out.wav").string(), pcm, kRate).has_value());
    HSF_CHECK(hsf::WriteAudio((dir / "missing" / "out.flac").string(), pcm, kRate).has_value());

    // A WAV file holds (2^32 - 1 - 36) / 4 frames. A FLAC file has no such limit.
    constexpr uint64_t kMaxWavFrames = (0xffffffffu - 36) / 4;
    HSF_CHECK(!hsf::CheckAudioLength("long.wav", kMaxWavFrames));
    HSF_CHECK(hsf::CheckAudioLength("long.wav", kMaxWavFrames + 1).has_value());
    HSF_CHECK(!hsf::CheckAudioLength("long.flac", kMaxWavFrames * 100));

    fs::remove_all(dir);
    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::fprintf(stderr, "audio out test passed\n");

    return 0;
}
