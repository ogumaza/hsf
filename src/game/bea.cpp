// SPDX-License-Identifier: MIT

#include "game/bea.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include <zstd.h>

namespace hsf::game
{
namespace
{

// The header's +0x28 gives the offset of the asset table, in one of two layouts. Each asset has a header that starts
// with "ASST", with the compression at +0x10 and the stored size at +0x14. A name is a 16-bit length, then the name.
//
// In WarioWare: Get It Together!'s and WarioWare: Move It!'s archives (bytes 8 to 11 of the header 00 01 01 00), the
// table holds the 64-bit offsets of the assets' headers. Such a header has the size at +0x18 (64 bits), the data's
// offset at +0x20 and the name's offset at +0x28.
constexpr std::size_t kAssetHeader = 0x30;

// In Super Mario Party Jamboree's archives (00 00 06 00), the table holds the headers themselves. Such a header has the
// size at +0x18 (32 bits), a tag at +0x1C, a hash at +0x28, the data's offset at +0x38 and the name's offset at +0x40.
constexpr std::size_t kInlineAssetHeader = 0x48;

bool IsAssetHeader(std::span<const uint8_t> data, uint64_t offset)
{
    return data[offset] == 'A' && data[offset + 1] == 'S' && data[offset + 2] == 'S' && data[offset + 3] == 'T';
}

bool Fits(std::span<const uint8_t> data, uint64_t offset, uint64_t size)
{
    return offset <= data.size() && size <= data.size() - offset;
}

} // namespace

bool BeaArchive::Is(std::span<const uint8_t> data)
{
    return data.size() >= kHeaderSize && data[0] == 'S' && data[1] == 'C' && data[2] == 'N' && data[3] == 'E' &&
           data[0xC] == 0xFF && data[0xD] == 0xFE;
}

std::optional<std::string> BeaArchive::Parse(std::span<const uint8_t> data, BeaArchive& out)
{
    out.assets_.clear();
    if (!Is(data))
    {
        return std::string("not a little-endian Bezel Engine archive");
    }

    const uint32_t count = xsflib::LoadLe32(&data[0x20]);
    const uint64_t table = xsflib::LoadLe64(&data[0x28]);

    // A table of offsets can't start with "ASST" unless its first asset lies past 1.4 GB.
    const bool inline_headers = count != 0 && Fits(data, table, 4) && IsAssetHeader(data, table);
    const uint64_t stride = inline_headers ? kInlineAssetHeader : 8;
    if (!Fits(data, table, stride * count))
    {
        return std::string("its asset table runs past its end");
    }

    for (uint32_t i = 0; i < count; ++i)
    {
        const uint64_t entry = inline_headers ? table + stride * i : xsflib::LoadLe64(&data[table + 8 * i]);
        if (!Fits(data, entry, inline_headers ? kInlineAssetHeader : kAssetHeader) || !IsAssetHeader(data, entry))
        {
            return "asset " + std::to_string(i) + " has no ASST header";
        }

        Asset asset;
        asset.compression = data[entry + 0x10];
        asset.stored_size = xsflib::LoadLe32(&data[entry + 0x14]);
        asset.size = inline_headers ? xsflib::LoadLe32(&data[entry + 0x18]) : xsflib::LoadLe64(&data[entry + 0x18]);
        asset.offset = xsflib::LoadLe64(&data[entry + (inline_headers ? 0x38 : 0x20)]);
        const uint64_t name = xsflib::LoadLe64(&data[entry + (inline_headers ? 0x40 : 0x28)]);
        if (!Fits(data, name, 2) || !Fits(data, name + 2, xsflib::LoadLe16(&data[name])) ||
            !Fits(data, asset.offset, asset.compression == Asset::kStored ? asset.size : asset.stored_size))
        {
            return "asset " + std::to_string(i) + " runs past the archive's end";
        }

        const auto* text = reinterpret_cast<const char*>(&data[name + 2]);
        asset.name.assign(text, xsflib::LoadLe16(&data[name]));
        out.assets_.push_back(std::move(asset));
    }

    return std::nullopt;
}

bool BeaArchive::Read(std::span<const uint8_t> data, const Asset& asset, std::vector<uint8_t>& out)
{
    if (asset.compression == Asset::kStored)
    {
        const auto first = data.begin() + static_cast<std::ptrdiff_t>(asset.offset);
        out.assign(first, first + static_cast<std::ptrdiff_t>(asset.size));
        return true;
    }

    if (asset.compression != Asset::kZstandard || asset.size > (uint64_t{1} << 34))
    {
        return false;
    }

    out.resize(static_cast<std::size_t>(asset.size));
    const std::size_t made =
        ZSTD_decompress(out.data(), out.size(), &data[static_cast<std::size_t>(asset.offset)], asset.stored_size);
    return !ZSTD_isError(made) && made == out.size();
}

bool BeaArchive::ReadHead(std::span<const uint8_t> data, const Asset& asset, std::size_t count,
                          std::vector<uint8_t>& out)
{
    count = static_cast<std::size_t>(std::min<uint64_t>(count, asset.size));
    if (asset.compression == Asset::kStored)
    {
        const auto first = data.begin() + static_cast<std::ptrdiff_t>(asset.offset);
        out.assign(first, first + static_cast<std::ptrdiff_t>(count));
        return true;
    }

    if (asset.compression != Asset::kZstandard)
    {
        return false;
    }

    // Decompress only until the first bytes are out.
    ZSTD_DStream* stream = ZSTD_createDStream();
    if (stream == nullptr)
    {
        return false;
    }

    out.resize(count);
    ZSTD_inBuffer in{&data[static_cast<std::size_t>(asset.offset)], asset.stored_size, 0};
    ZSTD_outBuffer to{out.data(), out.size(), 0};
    bool ok = true;
    while (to.pos < to.size && in.pos < in.size)
    {
        const std::size_t result = ZSTD_decompressStream(stream, &to, &in);
        if (ZSTD_isError(result))
        {
            ok = false;
            break;
        }
    }

    ZSTD_freeDStream(stream);
    out.resize(to.pos);

    return ok && to.pos == count;
}

} // namespace hsf::game
