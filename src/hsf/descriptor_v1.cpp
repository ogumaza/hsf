// SPDX-License-Identifier: MIT

// Version 1's blocks of the archive descriptor after the track volumes: the scripted starts and the user procedures.
// Also version 1's checks of them, of the track volumes and of the aux effects (see format.h and docs/hsf.md). xsflib
// writes and reads the track volumes.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/psf/descriptor.h"
#include "xsflib/psf/psf.h"
#include "xsflib/text.h"

#include "hsf/descriptor_parts.h"
#include "hsf/format.h"

namespace hsf::detail
{
namespace
{

using xsflib::psf::Put16;
using xsflib::psf::Put32;

// Version 1's scripted starts, after the track volumes: the count of the sounds, then each sound's item ID, its lead-in
// and the count of its steps, then each step's frame, kind, procedure, variable, track, value and item ID.
constexpr std::size_t kScriptBytes = 12;
constexpr std::size_t kScriptStepBytes = 16;

// Version 1's user procedures, after the scripted starts: the handler and the count of its tables, then each table's
// number and its 128 values.
constexpr std::size_t kProceduresBytes = 8;
constexpr std::size_t kProcedureTableBytes = 4 + 2 * UserProcedureTable::kValues;

// The longest a sound plays after its lead-in's last step: the length analysis's limit of 600 s, in frames of 5 ms.
constexpr uint64_t kMaxScriptFrames = 120000;

// The type of a sound in the top byte of its nn::atk item ID (ntdlib's sar::kSoundItem).
constexpr uint32_t kSoundItemType = 0x01;

// Whether one of `steps` starts `sound`.
bool StartsSound(std::span<const ScriptStep> steps, uint32_t sound)
{
    return std::any_of(steps.begin(), steps.end(),
                       [sound](const ScriptStep& s) { return s.kind == ScriptStep::kStart && s.sound == sound; });
}

// Why a version 1 player can't play the start `step` after the steps `before`, or nothing (see CheckStep).
// `where` names the step in the message.
std::optional<std::string> CheckStartStep(const ScriptStep& step, std::span<const ScriptStep> before,
                                          const std::string& where, std::size_t addons)
{
    if (step.sound >> 24 != kSoundItemType)
    {
        return "the archive descriptor's " + where + " starts item " + xsflib::Hex(step.sound, 8) +
               "; the item isn't a sound";
    }

    if (step.variable != ScriptStep::kNone && step.variable >= addons)
    {
        return "the archive descriptor's " + where + " starts a sound of add-on archive " +
               std::to_string(step.variable) + "; it lists " + std::to_string(addons) + " add-on archives";
    }

    if (step.track != ScriptStep::kNone || step.value != 0)
    {
        return "the archive descriptor's " + where + " starts a sound and sets a variable";
    }

    if (StartsSound(before, step.sound))
    {
        return "the archive descriptor's " + where + " starts sound " + xsflib::Hex(step.sound, 8) + " again";
    }

    return std::nullopt;
}

// Why a version 1 player can't play the write `step` after the steps `before`, or nothing (see CheckStep).
// `where` names the step in the message.
std::optional<std::string> CheckWriteStep(const ScriptStep& step, std::span<const ScriptStep> before,
                                          const std::string& where)
{
    const bool in_procedure = step.kind == ScriptStep::kWriteInProcedure;
    const std::string name = "variable " + std::to_string(step.variable);
    if (step.variable >= ScriptStep::kTrackEnd)
    {
        return "the archive descriptor's " + where + " sets " + name + "; the sequence variables are 0 to 47";
    }

    const bool global = step.variable >= ScriptStep::kLocalEnd && step.variable < ScriptStep::kGlobalEnd;
    const bool track_variable = step.variable >= ScriptStep::kGlobalEnd;
    if (track_variable && step.track >= SoundTrackVolumes::kTracks)
    {
        return "the archive descriptor's " + where + " sets " + name + " on track " + std::to_string(step.track) +
               "; a track's variable is on track 0 to 15";
    }

    if (!track_variable && step.track != ScriptStep::kNone)
    {
        return "the archive descriptor's " + where + " sets " + name + " on track " + std::to_string(step.track) +
               "; only variables 32 to 47 are a track's";
    }

    if (global && !in_procedure && step.sound != 0)
    {
        return "the archive descriptor's " + where + " sets the global " + name + " of sound " +
               xsflib::Hex(step.sound, 8) + "; a global variable is no sound's";
    }

    if ((!global || in_procedure) && !StartsSound(before, step.sound))
    {
        return "the archive descriptor's " + where + " sets " + name + " of sound " + xsflib::Hex(step.sound, 8) +
               "; no step before it starts that sound";
    }

    const auto same = [&step](const ScriptStep& other)
    {
        return other.kind == step.kind && other.procedure == step.procedure && other.frame == step.frame &&
               other.variable == step.variable && other.track == step.track && other.sound == step.sound;
    };
    if (std::any_of(before.begin(), before.end(), same))
    {
        return "the archive descriptor's " + where + " sets " + name + " a second time in frame " +
               std::to_string(step.frame) +
               (in_procedure ? " for user procedure " + std::to_string(step.procedure) : std::string());
    }

    return std::nullopt;
}

// Why a version 1 player can't play a step of `script`, the step at `i`, or nothing. A start starts a sound, once, from
// the set's archive or from one of `addons` add-on archives, and sets no variable. A write sets a variable below 48, of
// a sound that an earlier step starts, of the global variables or of a track from 0 to 15 of such a sound. A write of
// kind 2 waits for a procedure call of a sound that an earlier step starts. No other write of the same kind and
// procedure in the same frame sets the same variable. Only a write of kind 2 names a procedure.
std::optional<std::string> CheckStep(const ScriptedStart& script, std::size_t i, std::size_t addons)
{
    const ScriptStep& step = script.steps[i];
    const std::string where = "step " + std::to_string(i) + " of sound " + xsflib::Hex(script.sound, 8) + "'s script";
    const auto before = std::span(script.steps).first(i);

    if (i > 0 && step.frame < script.steps[i - 1].frame)
    {
        return "the archive descriptor's " + where + " comes at frame " + std::to_string(step.frame) +
               ", before the step ahead of it";
    }

    if (step.kind > ScriptStep::kWriteInProcedure)
    {
        return "the archive descriptor's " + where + " is of kind " + std::to_string(step.kind) +
               "; version 1 has only 0 (a start), 1 (a write) and 2 (a write in a user procedure)";
    }

    if (step.kind != ScriptStep::kWriteInProcedure && step.procedure != 0)
    {
        return "the archive descriptor's " + where + " names user procedure " + std::to_string(step.procedure) +
               "; only a write of kind 2 names one";
    }

    return step.kind == ScriptStep::kStart ? CheckStartStep(step, before, where, addons)
                                           : CheckWriteStep(step, before, where);
}

// Whether every one of an effect's `parameters` is a finite number.
bool AllFinite(const std::vector<float>& parameters)
{
    return std::all_of(parameters.begin(), parameters.end(), [](float parameter) { return std::isfinite(parameter); });
}

// Why a version 1 player can't play a delay or a light limiter, or nothing. Each has 8 finite parameters. A delay's
// first 2 and a light limiter's first 4 are times, each a whole number of microseconds from 0 to 10 s. The last is the
// channel mode, 0 to 3.
std::optional<std::string> CheckRendererEffect(const AuxEffect& effect)
{
    const std::string name = effect.type == AuxEffect::kDelay ? "a delay" : "a light limiter";
    const std::vector<float>& p = effect.parameters;
    if (p.size() != AuxEffect::kRendererEffectParameters)
    {
        return name + " has " + std::to_string(p.size()) + " parameters; it has 8";
    }

    if (!AllFinite(p))
    {
        return name + "'s parameter isn't a finite number";
    }

    const std::size_t times = effect.type == AuxEffect::kDelay ? 2 : 4;
    for (std::size_t i = 0; i < times; ++i)
    {
        if (p[i] < 0.0f || p[i] > AuxEffect::kMostTime || p[i] != std::trunc(p[i]))
        {
            return name + "'s time isn't a whole number of microseconds from 0 to 10 s";
        }
    }

    const float mode = p.back();
    if (mode != 0.0f && mode != 1.0f && mode != 2.0f && mode != 3.0f)
    {
        return name + "'s channel mode isn't 0, 1, 2 or 3";
    }

    return std::nullopt;
}

// Why a version 1 player can't play a reverb, or nothing. A reverb has 12 finite parameters. Its pre-delay is a whole
// number of microseconds from 0 to 300 ms, and its decay time one from 100 ms to 20 s. Its modes are 0 to 4, and its
// channel mode 0 to 3.
std::optional<std::string> CheckReverb(const AuxEffect& effect)
{
    const std::vector<float>& p = effect.parameters;
    if (p.size() != AuxEffect::kReverbParameters)
    {
        return "a reverb has " + std::to_string(p.size()) + " parameters; it has 12";
    }

    if (!AllFinite(p))
    {
        return std::string("a reverb's parameter isn't a finite number");
    }

    const auto whole = [](float value, float least, float most)
    {
        return value >= least && value <= most && value == std::trunc(value);
    };
    if (!whole(p[kReverbPredelay], 0.0f, AuxEffect::kMostPredelay))
    {
        return std::string("a reverb's pre-delay isn't a whole number of microseconds from 0 to 300 ms");
    }

    if (!whole(p[kReverbDecay], AuxEffect::kLeastDecay, AuxEffect::kMostDecay))
    {
        return std::string("a reverb's decay time isn't a whole number of microseconds from 100 ms to 20 s");
    }

    if (!whole(p[kReverbEarlyMode], 0.0f, 4.0f) || !whole(p[kReverbLateMode], 0.0f, 4.0f))
    {
        return std::string("a reverb's early or late mode isn't 0, 1, 2, 3 or 4");
    }

    if (!whole(p[kReverbChannelMode], 0.0f, 3.0f))
    {
        return std::string("a reverb's channel mode isn't 0, 1, 2 or 3");
    }

    return std::nullopt;
}

// Why a version 1 player can't play an I3DL2 reverb, or nothing. An I3DL2 reverb has 12 finite parameters.
std::optional<std::string> CheckI3dl2Reverb(const AuxEffect& effect)
{
    if (effect.parameters.size() != AuxEffect::kI3dl2Parameters)
    {
        return "an I3DL2 reverb has " + std::to_string(effect.parameters.size()) + " parameters; it has 12";
    }

    if (!AllFinite(effect.parameters))
    {
        return std::string("an I3DL2 reverb's parameter isn't a finite number");
    }

    return std::nullopt;
}

} // namespace

void PutScripts(std::vector<uint8_t>& v, const std::vector<ScriptedStart>& scripts,
                const std::vector<AddonArchive>& addons)
{
    Put32(v, static_cast<uint32_t>(scripts.size()));
    for (const ScriptedStart& script : scripts)
    {
        Put32(v, script.sound);
        Put32(v, script.lead_in);
        Put32(v, static_cast<uint32_t>(script.steps.size()));
        for (const ScriptStep& step : script.steps)
        {
            Put32(v, step.frame);
            v.push_back(step.kind);
            v.push_back(step.procedure);
            Put16(v, step.variable);
            Put16(v, step.track);
            Put16(v, static_cast<uint16_t>(step.value));
            Put32(v, step.sound);
        }
    }

    Put32(v, static_cast<uint32_t>(addons.size()));
    for (const AddonArchive& addon : addons)
    {
        xsflib::psf::PutArchivePaths(v, addon);
    }
}

bool ReadScripts(const std::vector<uint8_t>& data, std::size_t& pos, std::vector<ScriptedStart>& scripts,
                 std::vector<AddonArchive>& addons)
{
    pos = AlignTo4(pos);
    if (pos > data.size() || data.size() - pos < 4)
    {
        return false;
    }

    const uint32_t count = xsflib::LoadLe32(&data[pos]);
    pos += 4;
    if (count > (data.size() - pos) / kScriptBytes)
    {
        return false;
    }

    scripts.resize(count);
    for (ScriptedStart& script : scripts)
    {
        if (data.size() - pos < kScriptBytes)
        {
            return false;
        }

        script.sound = xsflib::LoadLe32(&data[pos]);
        script.lead_in = xsflib::LoadLe32(&data[pos + 4]);
        const uint32_t steps = xsflib::LoadLe32(&data[pos + 8]);
        pos += kScriptBytes;
        if (steps > (data.size() - pos) / kScriptStepBytes)
        {
            return false;
        }

        script.steps.resize(steps);
        for (ScriptStep& step : script.steps)
        {
            step.frame = xsflib::LoadLe32(&data[pos]);
            step.kind = data[pos + 4];
            step.procedure = data[pos + 5];
            step.variable = xsflib::LoadLe16(&data[pos + 6]);
            step.track = xsflib::LoadLe16(&data[pos + 8]);
            step.value = static_cast<int16_t>(xsflib::LoadLe16(&data[pos + 10]));
            step.sound = xsflib::LoadLe32(&data[pos + 12]);
            pos += kScriptStepBytes;
        }
    }

    if (data.size() - pos < 4)
    {
        return false;
    }

    const uint32_t addon_count = xsflib::LoadLe32(&data[pos]);
    pos += 4;
    if (addon_count > (data.size() - pos) / 4)
    {
        return false;
    }

    addons.resize(addon_count);
    for (AddonArchive& addon : addons)
    {
        pos = AlignTo4(pos);
        if (pos > data.size() || !xsflib::psf::ReadArchivePaths(data, pos, addon))
        {
            return false;
        }
    }

    return true;
}

void PutProcedures(std::vector<uint8_t>& v, const UserProcedureHandler& procedures)
{
    Put32(v, procedures.handler);
    Put32(v, static_cast<uint32_t>(procedures.tables.size()));
    for (const UserProcedureTable& table : procedures.tables)
    {
        Put32(v, table.number);
        for (const int16_t value : table.values)
        {
            Put16(v, static_cast<uint16_t>(value));
        }
    }
}

bool ReadProcedures(const std::vector<uint8_t>& data, std::size_t pos, UserProcedureHandler& procedures)
{
    pos = AlignTo4(pos);
    if (pos > data.size() || data.size() - pos < kProceduresBytes)
    {
        return false;
    }

    procedures.handler = xsflib::LoadLe32(&data[pos]);
    const uint32_t count = xsflib::LoadLe32(&data[pos + 4]);
    pos += kProceduresBytes;
    if (count > (data.size() - pos) / kProcedureTableBytes)
    {
        return false;
    }

    procedures.tables.resize(count);
    for (UserProcedureTable& table : procedures.tables)
    {
        table.number = xsflib::LoadLe32(&data[pos]);
        for (std::size_t i = 0; i < UserProcedureTable::kValues; ++i)
        {
            table.values[i] = static_cast<int16_t>(xsflib::LoadLe16(&data[pos + 4 + 2 * i]));
        }
        pos += kProcedureTableBytes;
    }

    return true;
}

std::optional<std::string> CheckTrackVolumes(const ArchiveDescriptor& d)
{
    constexpr uint32_t kFlags =
        ArchiveDescriptor::kFlagTrackVolumes | ArchiveDescriptor::kFlagScripts | ArchiveDescriptor::kFlagProcedures;
    if ((d.flags & ~kFlags) != 0)
    {
        return "the archive descriptor's flags are " + xsflib::Hex(d.flags) +
               "; version 1 has only bits 0 (track volumes), 1 (scripted starts) and 2 (user procedures)";
    }

    if ((d.flags & ArchiveDescriptor::kFlagTrackVolumes) == 0 && !d.track_volumes.empty())
    {
        return std::string("the archive descriptor lists track volumes without its flag for them (bit 0)");
    }

    return xsflib::psf::CheckTrackVolumes(d.track_volumes);
}

std::optional<std::string> CheckScripts(const ArchiveDescriptor& d)
{
    if ((d.flags & ArchiveDescriptor::kFlagScripts) == 0 && (!d.scripts.empty() || !d.addons.empty()))
    {
        return std::string("the archive descriptor lists scripted starts without its flag for them (bit 1)");
    }

    for (std::size_t i = 0; i < d.scripts.size(); ++i)
    {
        const ScriptedStart& script = d.scripts[i];
        const std::string name = "sound " + xsflib::Hex(script.sound, 8);
        const auto same_sound = [&script](const ScriptedStart& other)
        {
            return other.sound == script.sound;
        };
        if (std::any_of(d.scripts.begin() + static_cast<std::ptrdiff_t>(i) + 1, d.scripts.end(), same_sound))
        {
            return "the archive descriptor lists the scripted start of " + name + " more than once";
        }

        if (script.sound >> 24 != kSoundItemType)
        {
            return "the archive descriptor lists a scripted start for item " + xsflib::Hex(script.sound, 8) +
                   "; the item isn't a sound";
        }

        for (std::size_t k = 0; k < script.steps.size(); ++k)
        {
            if (auto err = CheckStep(script, k, d.addons.size()))
            {
                return err;
            }
        }

        const auto starts_sound = [&script](const ScriptStep& s)
        {
            return s.kind == ScriptStep::kStart && s.sound == script.sound && s.variable == ScriptStep::kNone;
        };
        const auto start = std::find_if(script.steps.begin(), script.steps.end(), starts_sound);
        if (start == script.steps.end())
        {
            return "the archive descriptor's script of " + name + " never starts the sound from the set's archive";
        }

        if (start->frame > script.lead_in)
        {
            return "the archive descriptor's script of " + name + " starts the sound at frame " +
                   std::to_string(start->frame) + ", after the lead-in of " + std::to_string(script.lead_in) +
                   " frames";
        }

        const uint64_t last = script.steps.back().frame;
        if (script.lead_in > last + kMaxScriptFrames)
        {
            return "the archive descriptor's script of " + name + " has a lead-in of " +
                   std::to_string(script.lead_in) + " frames; it ends at most 120000 frames after the last step";
        }
    }

    return std::nullopt;
}

std::optional<std::string> CheckProcedures(const ArchiveDescriptor& d)
{
    const UserProcedureHandler& procedures = d.procedures;
    if ((d.flags & ArchiveDescriptor::kFlagProcedures) == 0)
    {
        if (procedures != UserProcedureHandler{})
        {
            return std::string("the archive descriptor names user procedures without its flag for them (bit 2)");
        }

        return std::nullopt;
    }

    if (procedures.handler != UserProcedureHandler::kJamboree &&
        procedures.handler != UserProcedureHandler::kSuperstars)
    {
        return "the archive descriptor names handler " + std::to_string(procedures.handler) +
               " of user procedures; version 1 has only 1 (Super Mario Party Jamboree's) and 2 (Mario Party "
               "Superstars')";
    }

    if (procedures.handler != UserProcedureHandler::kJamboree && !procedures.tables.empty())
    {
        return std::string("the archive descriptor gives tables to a handler of user procedures that reads none");
    }

    for (std::size_t i = 0; i < procedures.tables.size(); ++i)
    {
        const uint32_t number = procedures.tables[i].number;
        if (number >= UserProcedureTable::kTables)
        {
            return "the archive descriptor lists table " + std::to_string(number) +
                   " of its user procedures; the tables are 0 to 15";
        }

        const auto same_number = [number](const UserProcedureTable& other)
        {
            return other.number == number;
        };
        if (std::any_of(procedures.tables.begin() + static_cast<std::ptrdiff_t>(i) + 1, procedures.tables.end(),
                        same_number))
        {
            return "the archive descriptor lists table " + std::to_string(number) +
                   " of its user procedures more than once";
        }
    }

    return std::nullopt;
}

std::optional<std::string> CheckEffects(const ArchiveDescriptor& d)
{
    for (const AuxEffect& effect : d.effects)
    {
        if (effect.bus > 2)
        {
            return "an aux effect is on bus " + std::to_string(effect.bus) + "; the aux buses are 0 to 2";
        }

        std::optional<std::string> err;
        if (effect.type == AuxEffect::kDelay || effect.type == AuxEffect::kLightLimiter)
        {
            err = CheckRendererEffect(effect);
        }
        else if (effect.type == AuxEffect::kReverb)
        {
            err = CheckReverb(effect);
        }
        else if (effect.type == AuxEffect::kI3dl2Reverb)
        {
            err = CheckI3dl2Reverb(effect);
        }
        else
        {
            return "an aux effect's type is " + std::to_string(effect.type) +
                   "; version 1 supports the I3DL2 reverb (1), the delay (2), the light limiter (3) and the reverb (4)";
        }

        if (err)
        {
            return err;
        }
    }

    return std::nullopt;
}

} // namespace hsf::detail
