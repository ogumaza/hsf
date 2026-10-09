// SPDX-License-Identifier: MIT

#include "game/bea.h"

#include <zstd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/bytes.h"

namespace hsf::game
{
namespace
{

// An asset's header: "ASST", then the compression at +0x10, the stored size at +0x14, the size at +0x18, the data's
// offset at +0x20 and the name's offset at +0x28 (a 16-bit length, then the name).
constexpr std::size_t kAssetHeader = 0x30;

bool Fits(std::span<const uint8_t> data, uint64_t offset, uint64_t size)
{
    return offset <= data.size() && size <= data.size() - offset;
}

} // namespace

bool BeaArchive::Is(std::span<const uint8_t> data)
{
    return data.size() >= 0x30 && data[0] == 'S' && data[1] == 'C' && data[2] == 'N' && data[3] == 'E' &&
           data[0xC] == 0xFF && data[0xD] == 0xFE;
}

std::optional<std::string> BeaArchive::Parse(std::span<const uint8_t> data, BeaArchive& out)
{
    out.assets_.clear();
    if (!Is(data))
    {
        return std::string("not a little-endian Bezel Engine archive");
    }

    const uint32_t count = LoadLe32(&data[0x20]);
    const uint64_t table = LoadLe64(&data[0x28]);
    if (!Fits(data, table, uint64_t{8} * count))
    {
        return std::string("its asset table runs past its end");
    }

    for (uint32_t i = 0; i < count; ++i)
    {
        const uint64_t entry = LoadLe64(&data[table + 8 * i]);
        if (!Fits(data, entry, kAssetHeader) || data[entry] != 'A' || data[entry + 1] != 'S' ||
            data[entry + 2] != 'S' || data[entry + 3] != 'T')
        {
            return "asset " + std::to_string(i) + " has no ASST header";
        }

        Asset asset;
        asset.compression = data[entry + 0x10];
        asset.stored_size = LoadLe32(&data[entry + 0x14]);
        asset.size = LoadLe64(&data[entry + 0x18]);
        asset.offset = LoadLe64(&data[entry + 0x20]);
        const uint64_t name = LoadLe64(&data[entry + 0x28]);
        if (!Fits(data, name, 2) || !Fits(data, name + 2, LoadLe16(&data[name])) ||
            !Fits(data, asset.offset, asset.compression == 0 ? asset.size : asset.stored_size))
        {
            return "asset " + std::to_string(i) + " runs past the archive's end";
        }

        const auto* text = reinterpret_cast<const char*>(&data[name + 2]);
        asset.name.assign(text, LoadLe16(&data[name]));
        out.assets_.push_back(std::move(asset));
    }

    return std::nullopt;
}

bool BeaArchive::Read(std::span<const uint8_t> data, const Asset& asset, std::vector<uint8_t>& out) const
{
    if (asset.compression == 0)
    {
        const auto first = data.begin() + static_cast<std::ptrdiff_t>(asset.offset);
        out.assign(first, first + static_cast<std::ptrdiff_t>(asset.size));
        return true;
    }
    if (asset.compression != 2 || asset.size > (uint64_t{1} << 34))
    {
        return false;
    }

    out.resize(static_cast<std::size_t>(asset.size));
    const std::size_t made =
        ZSTD_decompress(out.data(), out.size(), &data[static_cast<std::size_t>(asset.offset)], asset.stored_size);
    return !ZSTD_isError(made) && made == out.size();
}

bool BeaArchive::ReadHead(std::span<const uint8_t> data, const Asset& asset, std::size_t count,
                          std::vector<uint8_t>& out) const
{
    count = static_cast<std::size_t>(std::min<uint64_t>(count, asset.size));
    if (asset.compression == 0)
    {
        const auto first = data.begin() + static_cast<std::ptrdiff_t>(asset.offset);
        out.assign(first, first + static_cast<std::ptrdiff_t>(count));
        return true;
    }
    if (asset.compression != 2)
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
