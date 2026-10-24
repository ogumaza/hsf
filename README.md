# hsf

HSF is a PSF-family music rip format for Nintendo Switch sequenced audio: the music that Switch games play from
NintendoWare sound archives (`.bfsar`), and the sequenced sound of the Super Mario Galaxy games' Switch releases.

A rip holds a game's sound archive. HSF's player plays it with a model of the SDK's sound library (`nn::atk`) and of the
console's audio renderer. The renderer mixes the sound outside the game, in the system. The model does the same
arithmetic as the console. It also runs the effects that a game puts on the sound library's aux buses, such as an I3DL2
reverb. `docs/hsf.md` specifies the format.

The Super Mario Galaxy games play their sound with JAudio2, the sound library of Nintendo's GameCube and Wii games. Their
Switch releases run it natively, with an emulation of its DSP program. A rip of one of them holds the game's JAudio
files, and the player plays them on a model of JAudio2 and of the emulation. These rips are version 2 of the format.

HSF comes with a ripper (`hsfrip`), a command-line renderer (`hsfplay`), a foobar2000 component and an XMPlay plugin.

## Tools

| Tool | Description |
|---|---|
| `hsfrip [options] <input>...` | Rips each input into an `.hsflib` and a `.minihsf` for each sequence. Each `.minihsf` is tagged with the sequence's length and fade. The rip goes in a folder next to the input, with the input's name and `_hsf` added. `-o` names another folder. An `.hsflib` that differs from one of the same name in the folder gets the next free name, such as `<name>_2.hsflib`. The rips already in the folder then still play. To replace earlier rips, rip into an empty folder. An input is a Switch game's files or a sound archive (`.bfsar`). The game's files are a folder (its RomFS, once it's extracted) or a Bezel Engine archive (`.bea`). Each of a game's sound archives gets a folder of its own. `--list` lists a game's sound archives and what each holds, `--only REGEX` rips the sounds whose labels match, `--bgm` rips the music, and `--help` shows the rest. Sequences that make no sound (such as placeholders, and sequences that only set values for the game) are listed and skipped. |
| `hsfplay [options] <file>...` | Renders each `.minihsf` at 48,000 Hz to a WAV file next to it. The render lasts for the tagged length and fade. `-o FILE` writes FILE instead. The file's extension selects WAV or FLAC. `--length` and `--fade` override the tags. `--info` shows a file's tags and the files it loads. |
| `foo_input_hsf` | The foobar2000 component, for foobar2000 on Windows and macOS. The component plays `.minihsf` and `.hsf` files, and shows and edits their tags. Rendering ahead during playback makes seeking quick. `plugins/foobar2000/README.md` describes how to build it. |
| `xmp-hsf` | The XMPlay plugin, a 32-bit DLL for XMPlay on Windows. The plugin plays `.minihsf` and `.hsf` files, and shows their tags. Rendering ahead during playback makes seeking quick. `plugins/xmplay/README.md` describes how to build it. |

`hsfrip` reads a game's files once they're extracted from the game: its RomFS folder, or the archives and sound archives
in it. Games on Bezel Engine keep nearly all their files, sound archives included, in `.bea` archives. `hsfrip` reads
the files inside them. For a Bezel Engine game, `hsfrip` also takes what the game's files set for its sound: the voice
count in `boot.lua`, and the reverb that the game's scenes put on aux bus A. A game comes as an NSP or an XCI file. Such
a file holds the game's files encrypted, and `hsfrip` doesn't read it.

Some games keep groups of their sounds in group files (`.bfgrp`) outside the sound archive. The archive lists those
group files by name. `hsfrip` looks for them by those names among the game's files. For a sound archive given on its
own, `hsfrip` looks in the archive's folder and the folders below it. Sequences whose banks or waves are in a group file
that isn't found are skipped.

For Super Mario Galaxy and Super Mario Galaxy 2, give `hsfrip` the game's RomFS folder. `hsfrip` finds the game's
JAudio files in its `AudioRes` folder. It rips each sound that plays a sequence, the music and the sound effects, into
one folder. `--bgm` rips the music. The levels, the effects and the states that each piece of music gives its tracks as
it starts come from the game's code. HSF holds them for the two games and recognises each game by its files. For
another JAudio game, `hsfrip` warns and takes JAudio2's defaults. Some things differ from the game:

- The games play much of their music as streams: audio files that HSF doesn't play. `hsfrip` leaves the streams out.
  Some pieces of music are layers that play beside a stream. The rip of such a piece holds the layer alone. A layer
  that starts silent is skipped.
- The game changes some settings as it goes on: the levels of a scene, the effects of some of Super Mario Galaxy 2's
  stages, and the tracks that a piece of music plays. A rip has the settings that the game sets up as it starts and
  as each piece starts.
- Sounds that play notes on banks that the game doesn't load are listed and skipped.
- The games turn their output down on the console. Super Mario Galaxy plays at 0.355 of full scale on a TV, and at
  0.71 on the console's speaker, on headphones and on a USB device. Super Mario Galaxy 2 plays at 0.376 and 0.752. The
  console applies these levels after the game's mix. A rip plays the mix at full scale. It plays about 9 dB louder
  than the game on a TV, and 3 dB louder than on the console's speaker.

HSF's earlier players refuse version 2 rips with a message. In a version 2 rip, a seek back past the audio that the
foobar2000 component or the XMPlay plugin keeps plays the track again from its start.

Both tools accept several inputs and continue if one fails. A path that doesn't exist stops the command before it writes
any files. For example, `hsfrip romfs` writes `romfs_hsf/`. That folder holds a folder for each of the game's sound
archives. Then `hsfplay romfs_hsf/*/*.minihsf` renders every sequence in it. `--version` shows a tool's version.

### Downloads

The Releases page has `hsfrip` and `hsfplay` for 64-bit Windows, the foobar2000 component for 64-bit and 32-bit
foobar2000 on Windows, and the XMPlay plugin. On macOS, build HSF from source (see "Building").

To install the XMPlay plugin, unzip `xmp-hsf-X.Y.Z.zip` and put `xmp-hsf.dll` in XMPlay's folder, or in a folder
inside it. Then restart XMPlay.

### Drag and drop

- **Windows:** drop files or folders on `hsfrip.exe`, or files on `hsfplay.exe`. A console window shows the results and
  stays open until you press Enter.
- **macOS:** the build also makes `hsfrip.app` and `hsfplay.app` in the build folder. Drop files on one, or open it and
  choose them. The app shows the results for each file in a dialog, with a button that opens the output folders.

As with the command line, the results go next to the files. File and folder names can use any characters. On Windows,
that requires Windows 10 version 1903 or later.

## Building

You need Git, CMake 3.20 or later, and C and C++20 compilers. HSF builds on two libraries in submodules: xsflib, for
the players and rippers of the PSF family, in `externals/xsflib`, and ntdlib, for the hardware and formats of
Nintendo's consoles, in `externals/ntdlib`. Clone with `git clone --recursive`, or run
`git submodule update --init --recursive` in a clone. xsflib takes seqlib as a submodule. Zstandard, zlib and libFLAC
are downloaded and built along with HSF when they aren't installed.

- **Windows:** Visual Studio 2022 or later with the "Desktop development with C++" workload and its "C++ Clang tools for
  Windows" component. The workload includes CMake. The component adds clang-cl. Configure with `cmake -S . -B build -A
  x64 -T ClangCL` in place of the configure command below. Run the commands in a Developer PowerShell or Developer
  Command Prompt.
- **macOS on Apple Silicon:** the Xcode command line tools (`xcode-select --install`) and CMake (`brew install cmake`).
  `brew install zstd flac` saves building Zstandard and libFLAC.

Linux, Intel Macs, MSVC and 32-bit Windows aren't supported. The exceptions are the 32-bit foobar2000 component and the
XMPlay plugin. Build them with clang-cl, configured with `-A Win32 -T ClangCL`.

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

`CMakePresets.json` has `release` and `debug` presets for the same steps (`cmake --preset release`, `cmake --build
--preset release` and `ctest --preset release`). The presets require CMake 3.23 or later. They don't select clang-cl.
On Windows, use the commands above.

The models of the sound library and the renderer are in ntdlib's `nx` module, with their tests.

The format test (`tests/format_test.cpp`) checks the descriptor and the loading of libraries. The player test
(`tests/player_test.cpp`) plays sets made from a small archive and checks lengths, fades, seeks, volume, group files and
a reverb's tail. The ripper's test (`tests/rip_test.cpp`) measures a sequence of each kind that the length analysis
distinguishes. The game test (`tests/game_test.cpp`) builds a Bezel Engine archive, a data sheet with a sound effect
list and a `boot.lua`. It checks what `hsfrip` finds in a folder of them, in the archive alone and in a title's folder
that holds the archive unpacked. The test also checks that the readers refuse damaged files.

## Layout

```
docs/         the format's specification (hsf.md)
src/game/     a Switch game's files: folders, and Bezel Engine's archives and data sheets
src/hsf/      the HSF format and the player
src/jaudio/   the model of JAudio2 and of the emulation of its DSP program (version 2)
src/rip/      the ripper: sets, the JAudio games and the length analysis
src/cli/      hsfrip and hsfplay
plugins/      the foobar2000 component and the XMPlay plugin
tests/        ctest suites
externals/    xsflib, the code that the PSF players share, and ntdlib, for Nintendo's formats and the models of the
              Switch's sound library and audio renderer (submodules)
```

## Licence

HSF is under the MIT licence (`LICENSE`). It builds on xsflib and ntdlib, both under the MIT licence too, in the
submodules `externals/xsflib` and `externals/ntdlib`. `THIRD-PARTY-NOTICES.md` lists the libraries HSF uses and what a
binary distribution must include. Game data is not covered by this licence. Rips contain a game's sound data, and they
are for personal use.
