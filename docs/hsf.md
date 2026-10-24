# HSF

Version 2. HSF stores sequenced music from Nintendo Switch games so that it can be played back as the game plays it. A
rip contains a game's sound files. In version 1, they're a sound archive. The player plays it with HSF's model of the
SDK's sound library (`nn::atk`) and of the console's audio renderer (`nn::audio`'s renderer), with the effects the game
puts on the sound library's aux buses. Version 2 adds games that play their sound with JAudio, the sound library of
Nintendo's GameCube and Wii games. A version 2 rip holds such a game's JAudio files. The player plays them with HSF's
model of JAudio and of the emulator that runs JAudio's DSP program in the game (see "Version 2: JAudio").

Switch games play sounds from NintendoWare sound archives (`.bfsar`, FSAR): sequences, wave sounds and streams. HSF
plays sequences. Streams are recorded audio. Other tools already play them.

HSF rips contain no program for a sound processor. The Switch mixes sound in its audio renderer. The renderer runs in
the system, outside the game's files. Every HSF player reproduces the renderer's work in code: the voices' decoding,
sample-rate conversion, filters, volumes and mixes, and the aux effects. A JAudio game runs JAudio's DSP program in an
emulator inside the game. A version 2 player reproduces the emulator's work in code too.

## Files

| Extension | Contents |
|---|---|
| `.hsf` | A self-contained rip (rarely used). |
| `.hsflib` | Shared data for one sound archive: the sound archive, any group files that its sequences require, and the archive descriptor. In version 2, a game's JAudio files and the descriptor. |
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
| `SND ` | `u32 sound id` | The sound to play: an `nn::atk` item id (`0x01000000 \| index` in the archive's sound table). In version 2, a JAudio sound ID: the section in the top byte, the group in the next, and the index in the low 16 bits. The last one in loading order takes effect. |

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
0x04  u32  descriptor version (1, or 2 for JAudio: see "Version 2's descriptor")
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

### Version 2's descriptor

Version 2 keeps version 1's layout up to the paths' padding. A version 1 player therefore reads the version and refuses
the set with "archive descriptor version 2; this player supports version 1". JAudio's settings follow the padding:

```
0x00  u32  magic "HSFA"
0x04  u32  descriptor version (2)
0x08  u32  flags: none (0)
0x0C  u32  output mode: 1 stereo
0x10  u32  the voice count: 64, the DSP program's channels
0x14  u32  0 (version 1's SDK version)
0x18  u32  0 (version 1's aux effects: version 2 has none)
      u16  length of the start-up archive's path, then the path (UTF-8)
      u16  2, then the sequence archive's path and the path of the wave archives' folder, each with its u16 length
      zeros to a multiple of 4
      u32  the library: 1 for JAudio2 as the Super Mario Galaxy games' Switch releases build it
      u32  the DSP's level, 0 to 65535, in 4.12: 4 times the level that the game gives the DSP's mixer
      f32  the sequence manager's volume
      f32  the effect manager's volume
      16 × f32  the volumes of the effects' categories
      4 FX lines of 28 bytes, each:
           u16  flags: 0 for a line that's off. Bit 0 filters the line before it feeds its buses, bit 1 after
           u16  the delay, 0 to 128 sub-frames of 80 samples
           u16  bus A: the DSP's address of a bus that the line feeds, or 0 for none
           u16  the line's level into bus A, 0x8000 for 1
           u16  bus B, the same way
           u16  the line's level into bus B
           8 × s16  the filter's coefficients
      u32  T: the number of sounds with track states, then for each:
           u32  the sound's ID
           8 bytes  the states of its first 16 child tracks, 4 bits each, track 0 in the high bits of the first byte
      u32  V: the number of sounds with volumes, then for each:
           u32  the sound's ID
           u32  the volume, 0 to 255, in place of the sound table's
```

The paths identify `FILE` chunks. The start-up archive (BAA) holds the sound table, the sequence collection, the wave
systems and the instrument banks. Yaz0 may compress it. The sequence archive (an RARC) holds sequence files (BMS). Yaz0
may compress the archive and each of its files. An empty path names no sequence archive. A wave archive (AW) is the
`FILE` chunk at the folder's path, a slash and the name that a wave system gives the archive. A version 2 player plays
version 1 and version 2. It rejects a version 2 descriptor with flags other than 0, an output mode other than 1, a voice
count other than 64, aux effects, other than two group paths, a library other than 1, a DSP level past 65535, a volume
that isn't a finite number of 0 or more, a delay past 128 sub-frames, or a sound's volume past 255. It rejects every
other version.

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

HSF's implementation (ntdlib's `nx` module, on ntdlib's model of the sequence player) follows a study of the code
of WarioWare: Get It Together!, and of the build of the renderer for the CPU that the game's `sdk` module holds. The
game links `nn::atk` from SDK 10.4.1. Other games may use other versions of the library, or another number of voices.
Rips of them are close but not guaranteed exact.

Version 1 plays sequence sounds only. A sound's files come from the archive, or from the group files that the descriptor
names. The ripper skips sequences whose files aren't there. The output starts with the first frame. The sound has
finished when the sequence has ended, no channel is sounding and a frame's output is silent. The effects' tails have
then died away.

## Version 2: JAudio

Some Switch releases of GameCube and Wii games run the games' code natively. Their sound comes from JAudio, the games'
sound library. JAudio's DSP program runs in Hagi, an emulator of the Wii's hardware that the game links. Hagi runs the
program as a high-level emulation in C++. Library 1 is JAudio2 as the Super Mario Galaxy games' Switch releases build
it, with the parts of the games' code that set up its sounds.

A version 2 player provides:

- **Hagi's emulation of the DSP program.** 64 channels decode AFC and PCM16 waves, resample them, filter them and mix
  them into buses, at 32,000 Hz in sub-frames of 80 samples. A frame has 7 sub-frames. The four FX lines delay what the
  channels send them, filter it and mix it into other buses. A line's delay feeds back into the line. The left and right
  buses make the output, at the descriptor's level. The arithmetic follows Hagi's: products round half to even at bit
  15, sums are clamped to 16 bits in the decoders, the resamplers and the mixers, and they wrap in the FX lines.
- **JAudio2's CPU side.** The sequences (the commands, tracks, ports, registers and random numbers), the instrument
  banks with their effects, the notes' channels with their envelopes and LFOs, and the 64 DSP channels with their
  priorities. Each sub-frame, the DSP renders. Then the CPU side updates every DSP channel, runs the sequences' ticks
  and steps the free-running LFO. The pitches and the LFOs follow JAudio2's rate of the DSP: 32,028.47 Hz.
- **The game's levels.** A sound plays at its volume in the sound table divided by 255. The game's code may write
  other volumes over the table's. The descriptor then gives the sound's volume. A sound of section 1, music, plays at
  the sequence manager's volume too. Other sounds play at the effect manager's volume times their category's.
  The category is the sound's group. A sound with track states starts with them. State 0 gives the track a volume of 0.
  The other states give it a volume of 1. State 2 mutes the track, and states 1 and 3 unmute it. Bit 3 mutes the track
  while its volume is 0. A piece of music reserves those 16 child tracks as it starts, and its sequence opens them in
  place. A mute at the start therefore lasts.
- **The rhythm system.** The Galaxy games take the beats of a rhythm piece from its sequence. A rhythm piece is a piece
  of music whose entry in the sound table has bit 0 of its 16-bit field at +6 set. The game mutes the piece's child
  track 0. That track holds the beats and plays no sound. The root track and child track 0 share the rhythm system's
  parser. The parser keeps the tempo of each tempo command that the two tracks read (0xE0 without registers), 120 at
  first. At each beat, a note on key 0x18 with a note ID of 1 to 7 on either track, the parser sets the root track's
  tempo to that value.
- **Hagi's output.** Hagi plays the DSP's output at 32,000 Hz. It converts it to 48,000 Hz by linear interpolation. Of
  every three outputs, the first is an input sample. The second and the third lie 2/3 and 1/3 of the way to the next
  input (fractions of 10,922 and 5,460 in 2.14, the product rounded at bit 13). The first input gives no output. The
  Switch's AudioOut then plays the result at a gain of 127/128, rounded down.
- **Output.** 16-bit stereo at 48,000 Hz. A sub-frame gives 120 stereo pairs, and the first gives 119.

HSF's implementation (`src/jaudio`) follows a study of the code in the Super Mario Galaxy games' `main`, with Hagi
linked in. The sound starts before the first sub-frame. The sound has finished when the sequence has ended, no DSP
channel plays, the FX lines' delays are empty and a sub-frame's output is silent.

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
jump goes to. A sequence also loops when its main track reaches the end of a loop that repeats for ever. A loop that
repeats a set number of times can outlast the limit. If a sequence reaches the limit inside such loops, they count as
loops that repeat for ever. Only a pass that changes the sound counts. A pass changes the sound when it plays a note,
changes a setting of its track or of the sequence, or opens a track. A tie or a monophonic note that continues a note at
the same key and velocity changes nothing. A pass that only waits, polls a variable or counts is idle. The main track is
the sequence's first track. Once that track has ended, or while it makes idle passes, the first track still playing
becomes the main track if this track played a note on the way round. Each track before the new main track must have
ended, be idle or wait for ever, or come after the first track and play no notes in its passes. A pass of a later track
that plays no note counts too if two things hold: the track holds a note that sounds until the game stops the note, on a
looping wave at its sustain level, and every other track still playing is idle or waits for ever. A track waits for ever
when it waits a negative number of ticks, waits out a held note, or waits for a note that never ends: one with no length
on a looping wave. When the loop moves to another track, or to another loop of the same track, the count starts again.
Loops that go round before the first sound don't count: a track that waits or sets values over and over while a note
fades in, or before another track starts, isn't the sound's loop. A loop that went round without a sound ends the
sequence at its last sound instead.

The fade of a looping sequence also waits for the other tracks. It starts once every other track still playing has made
two passes that change the sound, is idle, or waits for ever. Each time another track opens a track again, the track
makes such a pass. The first open isn't one. The fade therefore doesn't wait for the tracks that a looping track opens
again in each of its loops.

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

The archive in the `.hsflib` holds only the files that the ripped sequences play: each sequence's file, the files of its
banks, and the wave archives that those banks name. A group's file, in the archive or beside it, stays only when the
model reads a copy of one of them from it. The archive's header, `STRG` block and `INFO` block keep their bytes, apart
from the sizes and the places of the files. The `FILE` block holds the kept files in the archive's order. Each starts at
a multiple of 64 bytes from the archive's start. NintendoWare's tools lay out a `FILE` block the same way. A dropped
file has no place in the file table. The ripper reads each kept file back from the new archive and checks that it's the
same. When the cut fails, the ripper prints a warning. The `.hsflib` then holds the whole archive. Rips made with
earlier versions hold the whole archive.

### JAudio games

A game that plays its sound with JAudio keeps its JAudio files in a folder named `AudioRes`. `hsfrip` finds the start-up
archive there: a file of the folder with a name that ends in `.szs` or `.baa`, whose data is a BAA, compressed with Yaz0
or not. The sequence archive is the first `.arc` file in the folder's `Seqs` folder. The wave archives are in its
`Waves` folder. The rip goes in a folder with the start-up archive's name. Its `.hsflib` holds the three, with every
wave archive that the wave systems name and the game holds, and a version 2 descriptor. Each `.minihsf` selects a sound
of the sound table that plays a sequence: a piece of music or a sound effect. `--bgm` takes the sounds of section 1.

The descriptor's settings come from the game's code: the DSP's level, the managers' and the categories' volumes, the FX
lines, and the track states that the game gives its pieces of music as they start. `hsfrip` holds those settings for
Super Mario Galaxy and Super Mario Galaxy 2, and recognises each game by its start-up archive. Super Mario Galaxy 2's
code also writes the volumes of its music over the sound table's, from a table in `AudioRes/Info/BgmParam.arc`.
`hsfrip` reads that table and puts the volumes in the descriptor. For another game, it warns and takes JAudio2's
defaults: a DSP level of 0x4000, for the mixer's level of 1, volumes of 1, no FX lines, no track states and the sound
table's volumes. Some of a
game's sounds stand for pairs of other sounds, such as a piece of music and a stream. The game's code plays the pair.
`hsfrip` leaves those sounds out. The game plays some pieces of music beside a stream. Each is a layer of the stream's
music. HSF plays no streams. A rip of such a piece holds only the layer. A note on a bank that the start-up archive
doesn't load makes the game's code read through a null pointer. The length analysis lists and skips the sounds that play
such a note. The game can play such a sequence only with its tracks muted.

The lengths follow the rules above, in sub-frames of 2.5 ms, with some differences. A loop is a jump back of the main
track, outside a subroutine. The main track is the first track, in the order of the sequence's tree, that has started a
note and whose last pass started one. A track that plays only an intro therefore leaves the count to the tracks that
play in every pass. If no track's last pass started a note, the main track is the first track that has started a note.
Neither choice takes a track that waits for ever. Such a track waits for its notes to end, and each of them is held: it
sounds at its envelope's sustain level, on a looping wave. A pass that started no note counts only while the sound is
held. Each note that sounds is then held, and each track has started a note, jumped back or opened a track that is
still open. A sound effect that holds a note until the game stops it makes such passes. A sequence is steady while the
sound is held and each track without an open child track waits for its notes to end. A level that moves and a key's
sweep change the sound. The FX lines' tails count as sound. The rule for other tracks' loops doesn't apply.
`--no-length` leaves out the analysis. The sounds that play notes on banks that aren't loaded are then ripped, and those
notes are left out.

Tags written by `hsfrip`: `title` and `hsf_sound` (the sound's label), `game`, `length` and `fade`, `hsfby`, `utf8=1`,
and optionally `artist`, `year` and `copyright`.

## Copyright

An `.hsflib` contains copyrighted game data. Rips are for personal use.
