# hsf

HSF is a PSF-family music rip format for Nintendo Switch sequenced audio: the music that Switch games play from
NintendoWare sound archives (`.bfsar`). It's the Switch counterpart of 3SF (3DS), WSF (Wii) and CSF (Wii U).

A rip holds a game's sound archive. HSF's player plays it with a model of the SDK's sound library (`nn::atk`) and of the
console's audio renderer. The renderer mixes the sound outside the game, in the system. The model does the same
arithmetic as the console. It also runs the effects that a game puts on the sound library's aux buses, such as an I3DL2
reverb. `docs/hsf.md` specifies the format.

HSF comes with a ripper (`hsfrip`), a command-line renderer (`hsfplay`) and a foobar2000 component.

## Tools

| Tool | Description |
|---|---|
| `hsfrip [options] <input>...` | Rips each input into an `.hsflib` and a `.minihsf` for each sequence. Each `.minihsf` is tagged with the sequence's length and fade. The rip goes in a folder next to the input, with the input's name and `_hsf` added. `-o` names another folder. An input is a Switch game's files or a sound archive (`.bfsar`). The game's files are a folder (its RomFS, once it's extracted) or a Bezel Engine archive (`.bea`). Each of a game's sound archives gets a folder of its own. `--list` lists a game's sound archives and what each holds, `--only REGEX` rips the sounds whose labels match, `--bgm` rips the music, and `--help` shows the rest. Sequences that make no sound (such as placeholders, and sequences that only set values for the game) are listed and skipped. |
| `hsfplay [options] <file>...` | Renders each `.minihsf` at 48,000 Hz to a WAV file next to it. The render lasts for the tagged length and fade. `-o FILE` writes FILE instead. The file's extension selects WAV or FLAC. `--length` and `--fade` override the tags. `--info` shows a file's tags and the files it loads. |
| `foo_input_hsf` | The foobar2000 component, for foobar2000 on Windows and macOS. The component plays `.minihsf` and `.hsf` files, and shows and edits their tags. Rendering ahead during playback makes seeking quick. `plugins/foobar2000/README.md` describes how to build it. |

`hsfrip` reads a game's files once they're extracted from the game: its RomFS folder, or the archives and sound archives
in it. Games on Bezel Engine keep nearly all their files, sound archives included, in `.bea` archives. `hsfrip` reads
the files inside them. For a Bezel Engine game, `hsfrip` also takes what the game's files set for its sound: the voice
count in `boot.lua`, and the reverb that the game's scenes put on aux bus A. A game comes as an NSP or an XCI file. Such
a file holds the game's files encrypted, and `hsfrip` doesn't read it.

Some games keep groups of their sounds in group files (`.bfgrp`) outside the sound archive. The archive lists those
group files by name. `hsfrip` looks for them by those names among the game's files. For a sound archive given on its
own, `hsfrip` looks in the archive's folder and the folders below it. Sequences whose banks or waves are in a group file
that isn't found are skipped.

Both tools accept several inputs and continue if one fails. A path that doesn't exist stops the command before it writes
any files. For example, `hsfrip romfs` writes `romfs_hsf/`. That folder holds a folder for each of the game's sound
archives. Then `hsfplay romfs_hsf/*/*.minihsf` renders every sequence in it. `--version` shows a tool's version.

### Downloads

The Releases page has `hsfrip` and `hsfplay` for 64-bit and 32-bit Windows, and the foobar2000 component for 64-bit and
32-bit foobar2000 on Windows. On other systems, build HSF from source (see "Building").

### Drag and drop

- **Windows:** drop files or folders on `hsfrip.exe`, or files on `hsfplay.exe`. A console window shows the results and
  stays open until you press Enter.
- **macOS:** the build also makes `hsfrip.app` and `hsfplay.app` in the build folder. Drop files on one, or open it and
  choose them. The app shows the results for each file in a dialog, with a button that opens the output folders.

As with the command line, the results go next to the files. File and folder names can use any characters. On Windows,
that requires Windows 10 version 1903 or later.

## Building

You need Git, CMake 3.20 or later, and C and C++20 compilers. Zstandard, zlib and libFLAC are downloaded and built along
with HSF when they aren't installed.

- **Windows:** Visual Studio 2022 or later with the "Desktop development with C++" workload and its "C++ Clang tools for
  Windows" component. The workload includes CMake. The component adds clang-cl. Configure with `cmake -S . -B build -T
  ClangCL` in place of the configure command below. Run the commands in a Developer PowerShell or Developer Command
  Prompt.
- **macOS:** the Xcode command line tools (`xcode-select --install`) and CMake (`brew install cmake`). `brew install
  zstd flac` saves building Zstandard and libFLAC. A universal build (two architectures in `CMAKE_OSX_ARCHITECTURES`)
  always builds them.
- **Linux:** GCC or Clang, CMake and Git from your distribution. The Zstandard, zlib and FLAC development packages (on
  Debian and Ubuntu, `libzstd-dev`, `zlib1g-dev` and `libflac-dev`) save building them.

MinGW, GCC and GNU-style Clang aren't supported on Windows. GCC and Clang aren't supported for 32-bit x86 either. In
those configurations, the floating-point arithmetic gives different results from the other builds. CMake rejects them.

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

`CMakePresets.json` has `release` and `debug` presets for the same steps (`cmake --preset release`, `cmake --build
--preset release` and `ctest --preset release`). The presets require CMake 3.21 or later.

The sound archive test (`tests/fsar_test.cpp`) builds sound archives, sequences, banks with every kind of region table,
wave archives, waves, and the group files that a game keeps outside its archive. Each file is built in the Switch's byte
order and in the Wii U's byte order. The test checks what the readers read from each file, and that the readers refuse
damaged files.

The model tests (`tests/atk_test.cpp` and `tests/audio_test.cpp`) check the models of the sound library and the
renderer against their formulas on made-up input. The engine test (`tests/atk_engine_test.cpp`) plays notes from small
archives through the sound library's model, with and without a reverb on an aux bus.

The format test (`tests/format_test.cpp`) checks the PSF container, tags, times, chunks, the descriptor and the loading
of libraries. The player test (`tests/player_test.cpp`) plays sets made from a small archive and checks lengths, fades,
seeks, volume, group files and a reverb's tail. The render-ahead test (`tests/render_ahead_test.cpp`) renders ahead from
a stand-in source and checks reads, seeks, the buffer's limit, errors and stopping. The ripper's test
(`tests/rip_test.cpp`) measures a sequence of each kind that the length analysis distinguishes. The game test
(`tests/game_test.cpp`) builds a Bezel Engine archive, a data sheet with a sound effect list and a `boot.lua`. It checks
what `hsfrip` finds in a folder of them and in the archive alone, and that the readers refuse damaged files. The audio
writer's test (`tests/audio_out_test.cpp`) writes a WAV file and a FLAC file and checks that both hold the samples it
wrote.

## Layout

```
docs/         the format's specification (hsf.md)
src/common/   small helpers shared by the rest (byte order, ASCII)
src/fsar/     the sound formats: sound archives (FSAR), sequences, banks, wave archives and waves
src/atk/      the model of the Switch's sound library, nn::atk: its arithmetic, envelopes, voices, sequences and
              aux effects
src/audio/    the model of the Switch's audio renderer: voices, mixes, aux effects and sinks, the frames it renders,
              and the I3DL2 reverb that games run on aux effects
src/game/     a Switch game's files: folders, and Bezel Engine's archives and data sheets
src/hsf/      the HSF format, the player, playback and rendering ahead
src/rip/      the ripper: sets and the length analysis
src/cli/      hsfrip and hsfplay, and their WAV and FLAC writer and console
plugins/      the foobar2000 component
platform/     the Windows manifest and version resource, and the macOS droplet apps of hsfrip and hsfplay
tests/        ctest suites
```

## Licence

HSF is under the MIT licence (`LICENSE`). `THIRD-PARTY-NOTICES.md` lists the libraries HSF uses and what a binary
distribution must include. Game data is not covered by this licence. Rips contain a game's sound data, and they are for
personal use.
