# HSF

Version 1. HSF stores sequenced music from Nintendo Switch games so that it can be played back as the game plays it. A
rip contains a game's sound archive. The player plays it with HSF's model of the SDK's sound library (`nn::atk`) and of
the console's audio renderer (`nn::audio`'s renderer), with the effects the game puts on the sound library's aux buses.

Switch games play sounds from NintendoWare sound archives (`.bfsar`, FSAR): sequences, wave sounds and streams. HSF
plays sequences. Streams are recorded audio. Other tools already play them.

Unlike 3SF and WSF rips, HSF rips contain no program for a sound processor. The Switch mixes sound in its audio
renderer. The renderer runs in the system, outside the game's files. Every HSF player reproduces the renderer's work in
code: the voices' decoding, sample-rate conversion, filters, volumes and mixes, and the aux effects.

## Files

| Extension | Contents |
|---|---|
| `.hsf` | A self-contained rip (rarely used). |
| `.hsflib` | Shared data for one sound archive: the sound archive, any group files that its sequences require, and the archive descriptor. |
| `.minihsf` | One sound: a small chunk that selects the sound, plus tags. It references an `.hsflib` with `_lib`. |

All three use the PSF container.

## PSF container

```
0x00  3 bytes  "PSF"
0x03  u8       version: 0x29 for HSF (see "Version byte" below)
0x04  u32      R: size of the reserved area
0x08  u32      N: size of the compressed program
0x0C  u32      CRC-32 of the compressed program
0x10  R bytes  reserved area (uncompressed)
      N bytes  program, zlib-compressed
      "[TAG]" followed by tags (optional)
```

All integers in the container, the chunk headers and the descriptor are little-endian. The CRC-32 is the one used by
zlib. The files that a rip contains are stored as the game has them, in the Switch's little-endian byte order.

### Version byte

HSF uses **0x29**. Known PSF version bytes are 0x01 (PSF1), 0x02 (PSF2), 0x11 (SSF), 0x12 (DSF), 0x13 (Sega Genesis),
0x21 (USF), 0x22 (GSF), 0x23 (SNSF), 0x24 (2SF), 0x25 (NCSF), 0x26 (3SF), 0x27 (WSF), 0x28 (CSF) and 0x41 (QSF). The
high nibble 2 marks Nintendo systems. 0x29 is the next free value after the Wii U's 0x28. The value is defined in one
place: `src/hsf/format.h`.

## Program: chunks

The decompressed program is a sequence of chunks. Each chunk is an 8-byte header followed by its payload. Zeros pad the
payload to a multiple of 4 bytes:

```
u32  type (four ASCII characters, stored as bytes in order)
u32  size of the payload in bytes (excluding padding)
...  payload
```

| Type | Payload | Meaning |
|---|---|---|
| `FILE` | `u16 path length`, the UTF-8 path (no leading `/`, `/` separators), zeros to a multiple of 4, then the file data | A file of the game, by its path among the game's files (for example `Audio/sound.bfsar`, or `Archive/Sound.bea/Audio/sound.bfsar` for a file in a Bezel Engine archive), or a file the descriptor names. |
| `SND ` | `u32 sound id` | The sound to play: an `nn::atk` item id (`0x01000000 \| index` in the archive's sound table). The last one in loading order takes effect. |

Unknown chunk types are skipped. Chunks are applied in order. For the same path, later data replaces earlier data.

### Loading order

As in other PSF formats, `_lib` is loaded recursively before the file that references it. Its path is relative to that
file. After the referencing file, `_lib2`, `_lib3` and so on are loaded in numeric order. Loading stops at the first
missing number. An empty `_lib` or `_libN` names no file. Chunks are applied in this loading order. A `.minihsf` usually
contains one `SND ` chunk.

## Reserved area: the archive descriptor

At least one loaded file must have an archive descriptor in its reserved area. The last descriptor in loading order
takes effect. The loaded files must select a sound with a `SND ` chunk.

```
0x00  u32  magic "HSFA"
0x04  u32  descriptor version (1)
0x08  u32  flags: version 1 has none (0)
0x0C  u32  output mode: 1 stereo (version 1 plays stereo only)
0x10  u32  nn::atk's voice count, 1 to 96: its voices and channels (96 unless the game changes it)
0x14  u32  the game's SDK version as major × 10,000 + minor × 100 + micro (100401 for 10.4.1), or 0 if unknown
0x18  u32  E: the number of aux effects, then for each, in order:
           u32  the aux bus: 0, 1 or 2 for buses A, B and C
           u32  the effect's type (see "Aux effects")
           u32  P: the number of parameters, then P × f32 parameters
      u16  length of the sound archive's path, then the path (UTF-8)
      u16  G: the number of group files, then for each its path's u16 length and the path (UTF-8)
      zeros to a multiple of 4
```

The paths identify `FILE` chunks containing the sound archive and the group files (`.bfgrp`) that the game keeps outside
it. The archive lists the files in those groups without data. A version 1 player rejects descriptors with a different
version, flags other than 0, an output mode other than 1, a voice count outside 1 to 96, or an effect on a bus past 2,
of a type other than 1, or with parameters that aren't 12 finite numbers. Output modes 0 (mono) and 2 (surround) are
left for a later version.

## Aux effects

`nn::atk` has three aux buses besides its main bus. A sound sends part of each voice to them, and a game puts effects on
them. With no effect on a bus, what a sound sends there reaches the output unchanged, with the main bus. The descriptor
lists the effects the game puts on each bus while the sound plays, in the order the game puts them there.

**Type 1: an I3DL2 reverb on an aux effect.** The game runs Nintendo's I3DL2 reverb in its process, on the samples that
an aux effect gives it. WarioWare: Get It Together! does this. The reverb's 12 parameters are those of `nn::audio`'s
`I3dl2ReverbParameterSet`, in the set's order: the room's gain and the room's high-frequency gain (millibels), the late
reverb's decay time (seconds) and decay ratio at high frequencies, the reflections' gain (millibels) and delay
(seconds), the reverb's gain (millibels) and the late reverb's delay (seconds), the diffusion and the density (percent),
the high-frequency reference (Hz) and the dry gain. Like the game, a player first holds the ratio to 0.1 to 2. It then
runs the reverb at 48 kHz on the bus's front left and right channels. `nn::atk`'s aux effect gives the reverb a ring of
4 frames of the bus's first 4 channels. What the reverb returns, the dry part included, therefore comes out 4 frames (20
ms) after it went in. The bus's rear channels come back unchanged, 4 frames late.

## The model

A version 1 player provides:

- **The audio renderer.** 48,000 Hz, with frames of 240 samples (5 ms). Voices decode DSP-ADPCM and PCM, convert their
  sample rate at the medium quality, filter with two biquads, and ramp their volumes and mixes across each frame. The
  final mix has six channels, and a sub-mix of four buses of six channels feeds it, each bus at 1.0. Aux effects run on
  the sub-mix's buses after its depop and before its mix. The device's output is the final mix's front left and right,
  held to 16 bits.
- **`nn::atk`**: the sequence player (sequence commands with the Switch's little-endian arguments, tracks, variables,
  random numbers), bank lookup and wave loading, envelopes, LFOs, sweep, portamento, pitch, volume and pan curves, the
  low-pass and biquad filters, the voices and their allocation with priorities, and the mixes that each channel sends to
  the main and aux buses, in stereo. The random number generator (x = x × 0x19660D + 0x3C6EF35F, from 0x12345678) steps
  once a frame. Each step comes after the channels' updates. Each frame runs the voices' statuses, the sound thread's
  frame, the voices' parameters, the aux effects, and then the renderer's frame. The sound starts in the second frame.
  That matches a game that starts the sound before a frame.
- **Output.** 16-bit stereo at 48,000 Hz.

HSF's implementation (`src/atk`, `src/audio`) was written from a study of the code of WarioWare: Get It Together!, and
of the build of the renderer for the CPU that the game's `sdk` module holds. The game links `nn::atk` from SDK 10.4.1.
Other games may use other versions of the library, or another number of voices. Rips of them are close but not
guaranteed exact.

Version 1 plays sequence sounds only. A sound's files come from the archive, or from the group files that the descriptor
names. The ripper skips sequences whose files aren't there. The output starts with the first frame. The sound has
finished when the sequence has ended, no channel is sounding and a frame's output is silent. The effects' tails have
then died away.

## End of a sound

Like any PSF player, an HSF player plays for the tagged length plus fade. Without a length tag, the player stops at
whichever comes first: the end of the sound plus half a second, or a default length and fade.

## Tags

The standard PSF tags apply: `title`, `artist`, `game`, `year`, `genre`, `comment`, `copyright`, `length`, `fade`,
`volume`, `_lib`, `_libN`. `hsfby` credits the ripper. The ripper also writes `hsf_sound` with the sound's label from
the sound archive.

`length` and `fade` count time at the output's 48,000 Hz: a player plays `length` × 48,000 stereo samples before the
fade.

## Ripping

`hsfrip` builds sets from a Switch game's files or from a sound archive (`.bfsar`). The game's files are a folder (the
game's RomFS, once it's extracted from the game) or a Bezel Engine archive (`.bea`). Games on Bezel Engine keep nearly
all their files in such archives, their sound archives included, and `hsfrip` reads the files inside them. A game's NSP,
XCI and NCA files hold its files encrypted, and are refused. `hsfrip --list` shows a game's sound archives. The rips go
in a folder next to the input, with the input's name and `_hsf` added. `-o` names another folder. Inside that folder,
each of the game's sound archives gets a folder of its own. Each of those folders has the archive's name.

The descriptor's settings come from the game's files where the files give them. A Bezel Engine game's `boot.lua` gives
the engine its voice count (`engine:SetVoiceCountMax`), and `hsfrip` takes it. Otherwise the voice count is 96. A Bezel
Engine game keeps its effect presets in its data (`Data/SheetBin/SoundEffectList.elf`). Its scenes put the I3DL2 reverb
named `I3DL2_Default` on aux bus A unless a scene names another, and `hsfrip` puts that reverb on aux bus A of every rip
from such a game. `--voices` and `--no-effects` override them.

`--only` takes the sequences whose labels match a regular expression. `--bgm` takes the music: the sequences whose
labels have BGM in them (in any case), and those that play on a music player. A music player's name has BGM, MUSIC, SONG
or FANFARE among the words that underscores separate (allowing digits after the word), and SE isn't among them.

The ripper lists and skips the sequences that make no sound. Some have a volume of 0 in the archive. The sound library
multiplies that volume into everything they play. The game can't make them heard. The length analysis finds the others
that never sound. The skipped sequences are placeholders, sequences that only set variables for the game, muted copies
of other sounds, and sequences that play only on paths that depend on what the game sets or on random numbers. A rip of
such a sequence would always take the same path. `--no-length` leaves out the analysis. Then only the sequences at a
volume of 0 are skipped.

A sequence's length comes from playing it on the model, with the descriptor's effects. A looping sequence gets two loops
plus a 10-second fade. A sequence that ends gets a length up to its last sound, plus half a second. The tails of the
sequence's effects count as sound. Either way, the length is at most 600 seconds. Sound means a sample louder than 8 of
32,767 (-72 dB).

A sequence loops when its main track jumps back to a command that the track has already played, outside a subroutine.
Inside a subroutine, such a jump counts only when the jump is unconditional and one of two things holds: the track holds
a note that sounds until the game stops the note, or the track has run no conditional command since the command that the
jump goes to. A sequence also loops when its main track reaches the end of a loop that repeats for ever. Only a pass
that changes the sound counts. A pass changes the sound when it plays a note, changes a setting of its track or of the
sequence, or opens a track. A tie or a monophonic note that continues a note at the same key and velocity changes
nothing. A pass that only waits, polls a variable or counts is idle. The main track is the sequence's first track. Once
that track has ended, or while it makes idle passes, the first track still playing becomes the main track if this track
played a note on the way round. Each track before the new main track must have ended, be idle or wait for ever, or come
after the first track and play no notes in its passes. A pass of a later track that plays no note counts too if two
things hold: the track holds a note that sounds until the game stops the note, on a looping wave at its sustain level,
and every other track still playing is idle or waits for ever. A track waits for ever when it waits a negative number of
ticks, waits out a held note, or waits for a note that never ends: one with no length on a looping wave. When the loop
moves to another track, or to another loop of the same track, the count starts again. Loops that go round before the
first sound don't count: a track that waits or sets values over and over while a note fades in, or before another track
starts, isn't the sound's loop. A loop that went round without a sound ends the sequence at its last sound instead.

A sequence can reach a state where each track waits for its notes to end, waits out a held note (one of 9,600 ticks or
more), waits for ever, or only makes still passes with no note whose length will run out. A still pass is idle and draws
no random number. It also leaves the variables that tracks read with the values that the pass before left. A variable
that no track reads, such as a beat that a track marks for the game, doesn't count. Notes on looping waves never end.
Such a sequence is steady once no note's envelope is in its attack, hold, decay or release, and nothing is moving. A
steady sequence loops when every running wave has looped twice since the sound last changed. If the sequence has made no
sound since then, it ended at its last sound instead. A sequence whose tracks all wait for ever or make still passes
that play no notes has ended when its notes have. A sequence that stays busy without a sound for 180 seconds ended where
it fell silent. The ripper skips a sequence that never sounds.

In `FILE` chunks, the `.hsflib` holds the sound archive and any group files that its sequences require. The `.hsflib`
also holds the archive descriptor. Each `.minihsf` selects one sequence with a `SND ` chunk. Sequences whose data isn't
all there are listed and skipped. Wave sounds aren't ripped.

Tags written by `hsfrip`: `title` and `hsf_sound` (the sound's label), `game`, `length` and `fade`, `hsfby`, `utf8=1`,
and optionally `artist`, `year` and `copyright`.

## Copyright

An `.hsflib` contains copyrighted game data. Rips are for personal use.
