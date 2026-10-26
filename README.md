# hsf

HSF is a PSF-family music rip format for Nintendo Switch sequenced audio: the music that Switch games play from
NintendoWare sound archives (`.bfsar`), and the sequenced sound of the Switch releases of the Super Mario Galaxy games,
Pikmin and Pikmin 2.

A rip holds a game's sound archive. HSF's player plays it with a model of the SDK's sound library (`nn::atk`) and of the
console's audio renderer. The renderer mixes the sound outside the game, in the system. The model does the same
arithmetic as the console. It also runs the effects that a game puts on the sound library's aux buses, such as an I3DL2
reverb. `docs/hsf.md` specifies the format.

The Super Mario Galaxy games play their sound with JAudio2, the sound library of Nintendo's GameCube and Wii games. Their
Switch releases run it natively, with an emulation of its DSP program. A rip of one of them holds the game's JAudio
files, and the player plays them on a model of JAudio2 and of the emulation. These rips are version 2 of the format.
Pikmin 2's Switch release, in Pikmin 1+2, plays its sound the same way with JAudio1, the version of JAudio before
JAudio2. The player plays its rips on a model of JAudio1. Pikmin's Switch release, also in Pikmin 1+2, plays its sound
with the C JAudio, the version before JAudio1. The player plays its rips on a model of the C JAudio.

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
count in `boot.lua`, and the effects that the game puts on the aux buses. WarioWare: Get It Together! puts a reverb on
aux bus A. WarioWare: Move It! puts a delay on aux bus A, a reverb on aux bus B and a light limiter on aux bus C. A game
comes as an NSP or an XCI file. Such a file holds the game's files encrypted, and `hsfrip` doesn't read it.

WarioWare: Move It! shortens its delay when the game speeds up: it divides the delay by the game's speed. A rip has the
delay at the game's normal speed. The game also turns its output down on a TV. It plays at half of full scale there,
and at full scale on the console's speaker, on headphones and on a USB device. The console applies these levels after
the game's mix. A rip plays the mix at full scale. It plays about 6 dB louder than the game on a TV. HSF 1.2.0 and
earlier players refuse the game's rips with a message: they don't have the delay or the light limiter.

Some games keep groups of their sounds in group files (`.bfgrp`) outside the sound archive. The archive lists those
group files by name. `hsfrip` looks for them by those names among the game's files. For a sound archive given on its
own, `hsfrip` looks in the archive's folder and the folders below it. Sequences whose banks or waves are in a group file
that isn't found are skipped.

For Super Mario Galaxy and Super Mario Galaxy 2, give `hsfrip` the game's RomFS folder. `hsfrip` finds the game's
JAudio files in its `AudioRes` folder. It rips each sound that plays a sequence, the music and the sound effects, into
one folder. `--bgm` rips the music. The levels, the effects and the states that each piece of music gives its tracks as
it starts come from the game's code. HSF holds them for the two games and recognises each game by its files. For
another JAudio2 game, `hsfrip` warns and takes JAudio2's defaults. Some things differ from the game:

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

For Pikmin 2, give `hsfrip` the game's RomFS folder. `hsfrip` finds the game's sound files in its `AudioRes` folder, and
the game's list of its music and its track maps beside them. It rips each piece of music and each sound effect into one
folder. `--bgm` rips the music. The settings of each piece come from the game's code and those files. Some things differ
from the game:

- The game plays the music of its caves with a conductor that picks the phrases at random. Each rip of a cave's piece
  plays it with one conductor. The rip has the conductor's name. `hsfrip` rips every conductor of the game. The game
  plays only some of them: the conductors whose names end in `_0` (but not `caveglass_10_0`), and `new_00_1`,
  `new_08_1`, `cavemetal_05_4`, `cavemetal_10_4`, `cavesoil_10_3`, `caverelax` and `cavekeyget`. It never plays the
  others: `caveglass_10_0`, `caveboss`, `iron` and the other variants of `caveconc`, `cavemetal` and `cavesoil`. A
  cave's piece never repeats. Its rip plays for 10 minutes, then fades. Each play of a rip draws the same phrases. The
  game draws others each time it plays the piece.
- The game never plays some of the pieces in its files: `2pbattle`, `a_zawinul01`, `c_result`, the six `ff_` jingles,
  `id_test`, `result` and `u_result`. `hsfrip` rips all of them but `result`. That piece plays programs that its bank
  lacks. It makes no sound. With `--no-length`, the rip holds it, silent. No track map names `a_zawinul01` or the
  pieces that `caveboss` and `iron` play. Their rips play every track.
- The game fades some tracks of a piece in and out as it goes on: the layers for events, treasure, enemies nearby,
  battles and the ground. A rip plays those layers muted, as each piece starts in the game.
- The game plays most effects at a place in its world, and moves their levels as the camera and the sound's source move.
  A rip plays each effect at the listener, with no place in the game's world. Some effects play until the game stops
  them. Their rips loop, then fade.
- The game turns its output down on the console: to 0.3 of full scale on a TV, and to 0.6 on the console's speaker, on
  headphones and on a USB device. A rip plays the mix at full scale. It plays about 10.5 dB louder than the game on a
  TV, and 4.4 dB louder than on the console's speaker.

Players without Pikmin 2's support, such as HSF 1.2.0, refuse its rips with a message.

For Pikmin, give `hsfrip` a folder that holds the game's RomFS and ExeFS, as `romfs` and `exefs`. `hsfrip` finds the
game's sound files in the RomFS's `SndData` folder: the bank file, `Banks/pikibank.bx`, the wave archives beside it and
the sequence archive, `Seqs/pikiseq.arc`. The game keeps the rest of its sound settings in its code, the ExeFS's `main`:
the sequence archive's index, the settings of the music and the effects, the FX lines and the cutscenes' table.
`hsfrip` reads them from the `main` of the game's version 1.0.0. For another build of `main`, or without it, `hsfrip`
stops with a message. `hsfrip` rips each piece of music, each piece of the cutscenes' music and each sound effect into
one folder. `--bgm` rips the music. Some things differ from the game:

- The game never plays `d_end2`, and the bank file has no wave system for it. `hsfrip` leaves it out with a warning.
- The game plays most effects at a place in its world, and moves their levels as the camera and the sound's source move.
  A rip plays each effect at the listener. Some effects play until the game stops them. Their rips loop, then fade.
- The game sets some effects as it goes on: the formation's sound by how far the stick is pushed, and the crowd of
  Pikmin by its size. The formation's rip plays the sound with the stick pushed all the way. A Pikmin's cry as it's
  plucked is one of several. The rip of the pluck plays one of them.
- The Switch's release plays nothing for the crowd of Pikmin, or for the system's effect `SYSSE_JUNGLE`. Their notes
  play waves that the bank file lacks. `hsfrip` skips them as sounds that make no sound. With `--no-length`, the rip
  holds a silent `.minihsf` for each of them: `SYSSE_JUNGLE` and the crowd's 29 sizes, `crowd-01` to `crowd-29`.
- A cutscene plays its sounds at the points of its timeline. Each sound of a cutscene gets a rip of its own, such as
  `cutscene-004-01` for sound 1 of cutscene 4.
- A cutscene can stop one of its sounds with a later event. The rip of such a sound plays on until the cutscene
  finishes.
- The game turns its output down on the console: to 0.3 of full scale on a TV, and to 0.6 on the console's speaker, on
  headphones and on a USB device. A rip plays the mix at full scale. It plays about 10.5 dB louder than the game on a
  TV, and 4.4 dB louder than on the console's speaker.

Players without Pikmin's support refuse its rips with a message too.

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
(`tests/player_test.cpp`) plays sets made from a small archive and checks lengths, fades, seeks, volume, group files, a
reverb's tail and a delay's echoes. The ripper's test (`tests/rip_test.cpp`) measures a sequence of each kind that the
length analysis distinguishes. The game test (`tests/game_test.cpp`) builds a Bezel Engine archive, a data sheet with a
sound effect list and a `boot.lua`. It checks what `hsfrip` finds in a folder of them, in the archive alone and in a
title's folder that holds the archive unpacked. The test also checks that the readers refuse damaged files.

## Layout

```
docs/         the format's specification (hsf.md)
src/game/     a Switch game's files: folders, and Bezel Engine's archives and data sheets
src/hsf/      the HSF format and the player
src/jaudio/   the model of JAudio2 and of the emulation of its DSP program (version 2)
src/jaudio1/  the model of JAudio1 and of Pikmin 2's sound code (version 2, library 2)
src/jac/      the model of the C JAudio and of Pikmin's sound code (version 2, library 3)
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
