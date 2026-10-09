// SPDX-License-Identifier: MIT

// Bezel Engine's archives (.bea): an SCNE header, then assets (ASST), each a file of the game by its path, stored as it
// is or compressed with Zstandard. Games on the engine keep nearly all their files, sound archives included, in them.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hsf::game
{

class BeaArchive
{
public:
    struct Asset
    {
        std::string name;        // the file's path in the game
        uint8_t compression = 0; // 0 none, 2 Zstandard
        uint64_t offset = 0;     // of the stored data in the archive
        uint32_t stored_size = 0;
        uint64_t size = 0;
    };

    // Whether `data` starts as a little-endian Bezel Engine archive does.
    static bool Is(std::span<const uint8_t> data);

    // Reads the asset table. Returns an error message if the archive is damaged.
    static std::optional<std::string> Parse(std::span<const uint8_t> data, BeaArchive& out);

    const std::vector<Asset>& Assets() const
    {
        return assets_;
    }

    // The asset's file, decompressed. Returns false if it's damaged or its compression unknown.
    bool Read(std::span<const uint8_t> data, const Asset& asset, std::vector<uint8_t>& out) const;

    // The asset's first bytes, at most `count`, without decompressing the rest. Returns false if they can't be read.
    bool ReadHead(std::span<const uint8_t> data, const Asset& asset, std::size_t count,
                  std::vector<uint8_t>& out) const;

private:
    std::vector<Asset> assets_;
};

} // namespace hsf::game
