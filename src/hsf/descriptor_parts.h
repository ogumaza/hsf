// SPDX-License-Identifier: MIT

// The parts of the archive descriptor that format.cpp writes, reads and checks by version: version 2's JAudio settings
// (descriptor_jaudio.cpp) and version 1's blocks after the paths (descriptor_v1.cpp). Also the helpers that their
// readers and checks share. Only format.cpp, descriptor_jaudio.cpp and descriptor_v1.cpp include this header.

#pragma once

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/bytes.h"

#include "hsf/format.h"

namespace hsf::detail
{

// The end of the zeros after a path or a block at `pos`: `pos` rounded up to a multiple of 4 bytes.
constexpr std::size_t AlignTo4(std::size_t pos)
{
    return (pos + 3) & ~std::size_t{3};
}

// Whether `volume` is 1: a volume that leaves a sound as it is.
constexpr bool IsUnity(float volume)
{
    return volume == 1.0f;
}

// Whether `volume` is a finite number of 0 or more.
inline bool IsVolume(float volume)
{
    return std::isfinite(volume) && volume >= 0.0f;
}

// Reads little-endian values in turn from a descriptor's data at `pos`, and moves `pos` past each. The caller has
// checked that the data holds every value that it reads.
class LeCursor
{
public:
    LeCursor(const std::vector<uint8_t>& data, std::size_t& pos) : data_(data), pos_(pos)
    {
    }

    // The next 16-bit value.
    uint16_t U16()
    {
        const auto value = static_cast<uint16_t>(xsflib::LoadLe16(&data_[pos_]));
        pos_ += 2;
        return value;
    }

    // The next 32-bit value.
    uint32_t U32()
    {
        const uint32_t value = xsflib::LoadLe32(&data_[pos_]);
        pos_ += 4;
        return value;
    }

    // The next 32-bit float.
    float F32()
    {
        return std::bit_cast<float>(U32());
    }

private:
    const std::vector<uint8_t>& data_;
    std::size_t& pos_;
};

// Writes version 2's fields after the paths: the JAudio settings and the library's fields.
void PutJaudio(std::vector<uint8_t>& v, const JaudioSettings& j);

// Reads version 2's fields at `pos`, after the paths and their padding. False if `data` ends first.
bool ReadJaudio(const std::vector<uint8_t>& data, std::size_t pos, JaudioSettings& j);

// Why a version 2 player can't play a version 2 descriptor, or nothing.
std::optional<std::string> CheckJaudio(const ArchiveDescriptor& d);

// Writes version 1's scripted starts and add-on archives (kFlagScripts).
void PutScripts(std::vector<uint8_t>& v, const std::vector<ScriptedStart>& scripts,
                const std::vector<AddonArchive>& addons);

// Reads version 1's scripted starts and add-on archives from `pos`, the end of the paths or of the track volumes, after
// the padding, and moves `pos` past them. False if `data` ends first.
bool ReadScripts(const std::vector<uint8_t>& data, std::size_t& pos, std::vector<ScriptedStart>& scripts,
                 std::vector<AddonArchive>& addons);

// Writes version 1's user procedures (kFlagProcedures).
void PutProcedures(std::vector<uint8_t>& v, const UserProcedureHandler& procedures);

// Reads version 1's user procedures from `pos`, the end of the blocks before them, after the padding. False if `data`
// ends first.
bool ReadProcedures(const std::vector<uint8_t>& data, std::size_t pos, UserProcedureHandler& procedures);

// Why a version 1 player can't play a descriptor's flags and track volumes, or nothing. The flags are
// kFlagTrackVolumes, kFlagScripts and kFlagProcedures. With the first, each sound is listed once, with 16 volumes that
// are finite numbers of 0 or more.
std::optional<std::string> CheckTrackVolumes(const ArchiveDescriptor& d);

// Why a version 1 player can't play a descriptor's scripted starts, or nothing. Each sound is listed once, by a sound's
// item ID. Its steps come in frame order, and one of them starts the sound from the set's archive by the end of the
// lead-in. The lead-in ends at most 600 s after the last step.
std::optional<std::string> CheckScripts(const ArchiveDescriptor& d);

// Why a version 1 player can't play a descriptor's user procedures, or nothing. The handler is Jamboree's or
// Superstars'. Only Jamboree's handler reads tables. Each table's number is below 16, and the descriptor lists it once.
std::optional<std::string> CheckProcedures(const ArchiveDescriptor& d);

// Why a version 1 player can't play a descriptor's aux effects, or nothing. Each is on bus 0, 1 or 2, and has a type
// that version 1 supports with parameters that it can play.
std::optional<std::string> CheckEffects(const ArchiveDescriptor& d);

} // namespace hsf::detail
