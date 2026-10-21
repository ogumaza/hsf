// SPDX-License-Identifier: MIT

// hsfplay: renders .minihsf files to WAV or FLAC at the output's 48000 Hz.
//
// usage: hsfplay [options] <file> [<file> ...]
//   -o, --output FILE  the output file, WAV or FLAC by its extension (default: the input's name with .wav, next to
//                      it). It takes a single input.
//   --length T         override the length tag (seconds, m:s or h:m:s)
//   --fade T           override the fade tag
//   --default T        length when there's no length tag (default and fade: PlaybackOptions)
//   --info             print the tags and loaded files, and don't render
//   --version          print the version
//
// The files are rendered one after another. A file that fails doesn't stop the rest. A .hsflib among them is skipped.
// It's a library that the others load. Dropping files on the program gives it the files as inputs.

#include <cstdio>
#include <string>

#include "xsflib/cli/play.h"

#include "hsf/format.h"
#include "hsf/player.h"

namespace
{

// The help's description of what the tool does.
constexpr char kDescription[] =
    "Renders each .minihsf file at the output's 48000 Hz to a WAV file next to it. The render\n"
    "lasts for the file's length and fade tags. Libraries (.hsflib) are skipped: the .minihsf\n"
    "files load them. You can also drop files on the program.\n";

// What the set holds, for the summary line of --info.
std::string DescribeSet(hsf::Playback& playback)
{
    const hsf::LoadedSet& set = playback.GetPlayer().GetLoadedSet();
    char text[96];
    std::snprintf(text, sizeof(text), "%zu files, %zu aux effects, sound 0x%08x", set.files.size(),
                  set.archive ? set.archive->effects.size() : 0, static_cast<unsigned>(set.sound.value_or(0)));

    return text;
}

} // namespace

int main(int argc, char** argv)
{
    xsflib::cli::PlayTool<hsf::Player> tool;
    tool.name = "hsfplay";
    tool.version = HSF_VERSION;
    tool.description = kDescription;
    tool.library_extension = ".hsflib";
    tool.mini_extension = ".minihsf";
    tool.a_file = "an HSF file";
    tool.set_up = "setting up";
    tool.describe_set = DescribeSet;

    return xsflib::cli::RunPlayTool(tool, argc, argv);
}
