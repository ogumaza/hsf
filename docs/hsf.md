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
| `SND ` | `u32 sound id` | The sound to play: an `nn::atk` item id (`0x01000000 \| index` in the archive's sound table). In version 2, a JAudio sound ID. In library 1, the ID has the section in the top byte, the group in the next, and the index in the low 16 bits. Libraries 2 and 3 give their IDs in their sections. The last one in loading order takes effect. |

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
of a type other than 1 to 3, or with parameters that don't fit its type (see "Aux effects"). Output modes 0 (mono) and 2
(surround) are left for a later version.

Types 2 and 3 came with the release after HSF 1.2.0. HSF 1.2.0 and earlier players refuse a descriptor with either of
them: "an aux effect's type is 2; version 1 supports only the I3DL2 reverb (1)". A descriptor without them is the same
as before, byte for byte.

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
      u32  the library: 1 for JAudio2 as the Super Mario Galaxy games' Switch releases build it, 2 for JAudio1 as
           Pikmin 2's Switch release builds it, or 3 for the C JAudio as Pikmin's Switch release builds it
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
      library 2 only: the fields in "Library 2: Pikmin 2"
      library 3 only: the fields in "Library 3: Pikmin"
```

The paths identify `FILE` chunks. The start-up archive (BAA) holds the sound table, the sequence collection, the wave
systems and the instrument banks. Yaz0 may compress it. The sequence archive (an RARC) holds sequence files (BMS). Yaz0
may compress the archive and each of its files. An empty path names no sequence archive. A wave archive (AW) is the
`FILE` chunk at the folder's path, a slash and the name that a wave system gives the archive. A version 2 player plays
version 1 and version 2. It rejects a version 2 descriptor with flags other than 0, an output mode other than 1, a voice
count other than 64, aux effects, other than two group paths, a library other than 1 to 3, a DSP level past 65535, a
volume that isn't a finite number of 0 or more, a delay past 128 sub-frames, or a sound's volume past 255. It rejects
every other version.

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
ms) after it went in. The bus's rear channels come back unchanged, 4 frames late. A player refuses a reverb whose
parameters aren't 12 finite numbers.

**Type 2: the renderer's delay.** `nn::atk`'s delay (`EffectDelay`). The audio renderer runs it on the bus. WarioWare:
Move It! puts one on aux bus A. Its 8 parameters, in order: the longest delay and the delay, in microseconds; the in
gain, the feedback gain and the dry gain; the low-pass amount and the channel spread; and the channel mode. Like
`nn::atk`, a player holds each gain, the low-pass amount and the spread to 0 to 1, and the delay to 0 to the longest
delay. The channel mode, 0 to 3, gives the delay 1, 2, 4 or 6 of the bus's channels. 1, 2 and 4 channels are the bus's
first. 6 are its front left and right, its centre and LFE, and its rear left and right, in the renderer's order.

**Type 3: the renderer's light limiter.** `nn::atk`'s light limiter (`EffectLightLimiter`). The renderer runs it on the
bus. Move It! puts one on aux bus C. Its 8 parameters, in order: the longest look-ahead, the attack, the release and the
look-ahead, in microseconds; the threshold, the input gain and the output gain; and the channel mode. The mode works as
a delay's does. Like `nn::atk`, a player holds the longest look-ahead to 50 ms, the attack to 0 to 10 ms, the release to
0 to 5 s, the look-ahead to 0 to the longest look-ahead, the threshold to 0 to 1, and each gain to 0 to 4. The limiter
takes the bus's first channels. Its look-ahead is at least one sample.

A player refuses a delay or a light limiter unless it has 8 parameters, each a finite number. The times must be whole
numbers of microseconds from 0 to 10 s: a delay's first 2 parameters and a light limiter's first 4. The channel mode
must be 0, 1, 2 or 3.

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
game links `nn::atk` from SDK 10.4.1. The delay and the light limiter follow a study of the code of WarioWare: Move
It! and of its `sdk` module. That game links SDK 14.3. Other games may use other versions of the library, or another
number of voices. Rips of them are close but not guaranteed exact.

Version 1 plays sequence sounds only. A sound's files come from the archive, or from the group files that the descriptor
names. The ripper skips sequences whose files aren't there. The output starts with the first frame. The sound has
finished when the sequence has ended, no channel is sounding and the effects no longer ring. Without a delay, the
effects ring while a frame's output has a sample other than 0. The renderer's delay rounds its products down. Once its
line holds a negative sample, its output stays at -1 or so for ever. With a delay, the effects therefore ring until no
frame has had a sample louder than 8 (see "Ripping") for the delay's time, in whole frames rounded up, and one frame
more. The delay's time is its second parameter, held to its first, the longest delay. With more than one delay, the one
with the most frames counts. A delay's feedback is less than 1. Once the output has been quiet for a whole delay, the
echoes of what it played stay quiet.

## Version 2: JAudio

Some Switch releases of GameCube and Wii games run the games' code natively. Their sound comes from JAudio, the games'
sound library. JAudio's DSP program runs in Hagi, an emulator of the Wii's hardware that the game links. Hagi runs the
program as a high-level emulation in C++. Library 1 is JAudio2 as the Super Mario Galaxy games' Switch releases build
it, with the parts of the games' code that set up its sounds.

A version 2 player provides:

- **Hagi's emulation of the DSP program.** 64 channels decode AFC, PCM8 and PCM16 waves, resample them, filter them
  and mix them into buses, at 32,000 Hz in sub-frames of 80 samples. A frame has 7 sub-frames. The four FX lines delay
  what the channels send them, filter it and mix it into other buses. A line's delay feeds back into the line. The left
  and right buses make the output, at the descriptor's level. The arithmetic follows Hagi's: products round half to
  even at bit 15, sums are clamped to 16 bits in the decoders, the resamplers and the mixers, and they wrap in the FX
  lines.
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

## Library 2: Pikmin 2

Pikmin 2's Switch release, in Pikmin 1+2, is another such release with Hagi. Its sound library is JAudio1, the version
of JAudio before JAudio2. Above JAudio1, the game's sound code plays each piece of music through an object of one of
four classes. That code answers the requests that a piece's tracks send to the game. Library 2 is JAudio1 as the release
builds it, with the parts of the game's code that set up its sounds and answer those requests.

A library 2 descriptor keeps version 2's layout. Library 2's fields follow the sounds' volumes:

```
      u16  the effect sequence: the file of the sequence archive that plays the effects, by its number
      u8   the effect sequence's volume, 0 to 127
      u8   0
      u16  length of the conductor archive's path, then the path (UTF-8), or 0 for none
      zeros to a multiple of 4
      u32  N: the number of pieces of music, up to 65,536, then for each:
           u16  the piece's file in the sequence archive, by its number
           u8   the piece's class, 0 to 3
           u8   the piece's volume, 0 to 127
           u8   the levels that the piece's child tracks keep, bits 0 to 4
           u8   0
           u16  the ticks between the beats of a directed piece
           u16  the child tracks that the game mutes as they start: bit N for the child in slot N
           u16  the section that a piece of class 2 starts at
           u16  length of the conductor's name, then the name (UTF-8), or 0 for none
           zeros to a multiple of 4
      u32  M: the number of sounds that start after draws, then for each:
           u32  the sound's ID
           u32  the numbers that the sequences' generator draws before the sound starts, 1 to 65,535
```

Library 2 leaves some of version 2's fields unused. They hold their defaults: the categories' volumes are 1, every FX
line is off with all its fields 0, and no sound has track states or a volume. The FX lines come from the start-up
archive's first FX scene, as in the game. The DSP's level is the level of the DSP's mixer in 4.12. Pikmin 2 sets it to
3.9 (15,974). The sequence manager's volume is the music's volume in the game's options, and the effect manager's volume
is the effects' volume. Both are 1 as the game starts. The volumes of the effect sequence and the pieces come from the
game's list of its music, over 127.

The start-up archive is an AAF. It holds the sound table, the instrument banks, the wave systems, and the FX scenes
and effect scenes. The sequence archive and the conductor archive are RARCs whose file entries take 24 bytes in place
of 20. Yaz0 may compress the conductor archive. A piece's conductor is the file of that name in the conductor archive.

A `SND ` chunk's ID selects a piece of music or an effect. A piece's ID is 0x80000000 plus the piece's place in the
list. An effect's ID has 0 in its top 2 bits, the effect's category in bits 12 to 19 and its index in the category in
the low 10 bits. Bits 10 and 11 hold the game's flags. A player keeps every effect until it ends, whatever those bits
hold.

A player rejects a library 2 descriptor with other values in the unused fields, an effect sequence's volume past 127,
more than 65,536 pieces, or a piece with a class past 3, a volume past 127 or kept levels past bit 4. It also rejects a
piece of class 3 without a conductor, a piece of another class with one, and a conductor without a conductor archive.
It rejects a sound that the list of draws names twice, and draws of 0 or past 65,535.
A player that supports library 1 alone, such as HSF 1.2.0, reads version 2's fields and refuses the set with "the
archive descriptor's library is 2; this player supports library 1 (the Super Mario Galaxy games' JAudio2)". Library
1's descriptors stay the same, byte for byte.

A library 2 player provides:

- **Hagi's emulation of the DSP program and Hagi's output**, as for library 1, with the program's oscillators. A
  channel plays an oscillator in place of a wave: a square wave, a sawtooth, a pulse for a quarter of each cycle, a
  sine, a triangle, or one of two tables of 64 samples that the program recomputes at each sub-frame's start. One of
  those tables holds noise, and the other a ramp. Their samples go to the channel's filters without the resampler.
- **JAudio1's CPU side.** The sequences (the commands, tracks, ports, registers, interrupts and random numbers), the
  instrument banks, the notes' channels with their envelopes and LFOs, and the 64 DSP channels with their priorities.
  Each sub-frame, the DSP renders. The CPU side then runs the port commands that JAI has left, updates every DSP
  channel and runs the sequences' ticks. The pitches and the LFOs follow JAudio1's rate of the DSP: 32,028.5 Hz. The
  game sets the level of a channel at full volume to 0.9 (`JASDriver::setLevel`). The sequences share one generator of
  random numbers (`JASPlayer::getRandomS32`). In the game, its state as a sound starts depends on what played before.
  A player starts it in its state as the game starts. For a sound that the descriptor lists with draws, the generator
  then draws that many numbers before the first sub-frame.
- **JAI, JAudio1's interface for the game.** JAI's work for a frame of the game runs 30 times a second, between two
  sub-frames. JAI sets the levels of a piece's tracks with port commands. A port command runs at the start of the next
  sub-frame. The root track's volume is the piece's volume over 127, times 0.8 for music and the music's volume. A
  child track of the root may request its levels. JAI then gives it 1 for the volume and the pitch, 0.5 for the pan, 0
  for the surround level, and 0 or the conductor's value for the effect send. Until the port command runs, each level
  that the piece doesn't keep is 0. Bit 0 keeps the volume, bit 1 the pitch, bit 2 the effect send, bit 3 the pan and
  bit 4 the surround level. Once the root track has opened a child, JAI gives the same levels to each child that
  hasn't requested them. Those children keep every level.
- **The game's classes of music.** The class of a piece sets the answers to its tracks' requests.
  - Class 0 (`BgmSeq`) plays the sequence alone. Each request gets 0.
  - Class 1 (`DirectedBgm`) gives the tracks objects. The directors of the piece may mute some of its child tracks as
    they start. A muted child stops its notes, and starts none. When the root track requests a beat, it takes its
    first tempo again and gets the piece's beat interval.
  - Class 2 (`JumpBgmSeq`) is class 1 with a start at the piece's section. A request for a jump gets none (0xFFFF).
  - Class 3 (`AutoBgm`) is class 1 with a conductor. The conductor answers the child tracks' requests for their
    phrases.
- **The conductor.** A conductor file gives each child track a list of modules. A module sets the track's volume, pan
  and transpose. It names two of the sequence's lists: tips for the track's ports 6 and 7, and wave scenes for its
  port 8. A module plays an on cycle and an off cycle, each a number of times. An on cycle draws a tip at random. Each
  cycle draws a wave scene at random, or takes the scenes in their order. After a number of counted beats, the track
  moves on to its next module. The conductor counts every other beat. The game's sound code draws its random numbers
  from one generator (x = x × 0x19660D + 0x3C6EF35F). In the game, the generator's state as a piece starts depends on
  what played before. The model starts the generator at 0, its value as the game starts.
- **The effects.** An effect plays on the effect sequence, as in the game. That sequence plays at its volume over 127,
  times the effects' volume. JAI gives the effect the first slot track of its category in the start-up archive's
  first effect scene. The effect starts in the first of JAI's frames in which the effect sequence plays and that track
  is open. A player plays each effect at the listener. The game plays an effect that has no place in its world that way.
  JAI starts such an effect with a default actor that has no object. The effect then plays at its volume in the sound
  table, with an effect send of 0.07 and a pan of 0.5, at the centre. The camera's distance and the scene's effect send
  change only the levels of an effect with a place in the world. Flags in the sound table make some effects play quieter
  or at another pitch, at random.
- **The game's other answers.** The effects of the captains' marching horns request the horn's place in its tune and
  the wait after each note. The model's horns start at the beginning of the tune. They wait 3 ticks, the game's wait
  for a stick tilted all the way. A request for the story's flag of the paid debt gets 0, the state at the start of
  the story. Any other request gets 0 and changes nothing.

HSF's implementation (`src/jaudio1`) follows a study of the code in the release's `main`, with Hagi linked in. The
sound starts before the first sub-frame. A piece has finished when its sequence has ended, no DSP channel plays, the FX
lines' delays hold no sample louder than 8 and a sub-frame's output is silent. An effect has finished in the same way,
once JAI has released it. A line's feedback can keep quieter samples going round for ever.

## Library 3: Pikmin

Pikmin's Switch release, in Pikmin 1+2, is another such release with Hagi. Its sound library is the C JAudio, the
version of JAudio before JAudio1. As the game starts, its sound code starts a sequence on each of the sequence
player's first four handles. The first two play the effects, and the third plays the cutscenes' music. The fourth
handle's file shares its data with the first file. The four sequences keep running. A piece of music plays on another
handle. Library 3 is the C JAudio as the release builds it, with the parts of the game's code that set up its sounds.

A library 3 descriptor keeps version 2's layout. Library 3's fields follow the sounds' volumes:

```
      u16  length of the sequence index's path, then the path (UTF-8)
      zeros to a multiple of 4
      4 × u16  the files of the sequence archive that start on handles 0 to 3 as the game starts, by number
      u16  the effects' level, 0 to 32,767
      u16  the cutscenes' level, 0 to 32,767
      f32  the music's volume
      u32  L: the number of wave scenes that the game loads as it starts, then for each, in the game's order:
           u16  the wave system, by its place in the bank file
           u16  the scene, by its place in the system
           u32  1 if the game sets the scene, or 0 if it only loads it
      u32  N: the number of pieces of music, up to 65,536, then for each:
           u16  the piece's file in the sequence archive, by its number
           u16  the child tracks that play: bit N for the child in slot N
           f32  the volume of those child tracks
           u16  the wave system of the scene that the piece sets
           u16  the scene
      u32  P: the number of pieces of the cutscenes' music, up to 65,536, then for each:
           u16  the piece's number
           u16  0
           u16  the wave system of the scene that the piece sets
           u16  the scene
      u32  E: the number of effects, up to 65,536, then for each:
           u8   the effect's kind, 0 to 7
           u8   the type: the event's, 1 to 7, for kind 3, the cutscene's, 0 or 1, for kind 7, and 0 otherwise
           u16  the value
           u16  the first value, or 0xFFFF for none
           u16  for kind 7, the game's frames from the sound to its cutscene's finish, or 0 for none; 0 otherwise
```

Library 3 leaves some of version 2's fields unused. They hold their defaults: the managers' and the categories'
volumes are 1, and no sound has track states or a volume. As for library 1, the FX lines are those that the game's
code sets up. The DSP's level is the level of the DSP's mixer in 4.12. The levels and the music's volume are the game's
at the default settings of its options. The game writes the effects' level to port 0 of the root tracks on handles 0
and 1, and the cutscenes' level to port 2 of the root track on handle 2.

The start-up archive is the bank file (.bx). It holds the wave systems and the instrument banks. The sequence archive
holds the sequences (BMS) one after another. The sequence index (BARC) gives each sequence's name, offset and size in
the archive. An entry can share the data of another entry instead. The game then follows the entries that share data
until it reaches one that doesn't, and reads that entry's offset and size. A sequence's number is its place in the
index. The game keeps the index in its code. The ripper copies it into a `FILE` chunk.

A scene of a wave system has a wave archive and lists of waves. A scene can depend on other scenes of its system. The
game loads a scene's archive as it loads or sets the scene. A scene that the game sets becomes its system's current
scene. A note plays a wave only if the current scene of the wave's system, or a scene that it depends on, lists the
wave, and that scene's archive is loaded. Otherwise the note takes no channel. The descriptor lists the scenes that
the game loads as it starts and as its first title screen starts.

A `SND ` chunk's ID selects an effect, a piece of the cutscenes' music or a piece of music. Its top 2 bits are 0 for an
effect, 1 for a piece of the cutscenes' music and 2 for a piece of music. The low 16 bits give the sound's place in
its list, and the other bits are 0.

A piece of music plays its file's sequence, with the file's number as its bank. The game first sets the piece's scene.
The piece's root track plays at the music's volume. Each child track that plays gets the piece's volume, and the other
child tracks get 0. A piece of the cutscenes' music is a number that the game writes to port 0 of the root track on
handle 2, once it has set the piece's scene.

The game starts an effect through the ports of tracks of the first two sequences. A sequence's tracks register IDs,
and the game finds a track by its ID. The effect's kind gives the track and the ports:

- Kind 0, an effect of the system: the game queues the value for port 0 of track 0x10009. A player plays the effect as
  the game does in a course. The game queues values 10 and 12 only there. Value 7 plays as the game closes its C
  menu, the only time the game queues it. The game first unpauses the tracks of the events and of the captain. It then
  queues 0x23 and 0x21 for port 1 of track 0x10009.
- Kind 1, an effect of the captain: the game queues the value for port 0 of track 0x1000A. It then writes the first
  value, if any, to port 1 of that track.
- Kind 2, an effect of the Pikmin: the game writes the value to port 1 of track 0x1000A.
- Kind 3, an effect of an object: the game creates an event of the effect's type on a child track of the root track
  on handle 1, at the listener. As it creates the event, the game queues the first value, if any, for the child's port
  0. Two of the game's frames later, it queues the value. A value's top 4 bits give a slot, and its low 12 bits the
  action's command. An event of type 7 starts an action as the game creates it. An effect that is that action has no
  first value. The game then queues the value as it creates the event, and no action follows.
- Kind 4, the sound of the formation: the game writes 0 to port 2 of track 0x10007 and the value to port 3. Port 2
  takes the stick's place across, and port 3 its distance from the centre. The game then writes 1 to port 0.
- Kind 5, the crowd of Pikmin: on each of its frames, the game writes the value to port 0 of track 0x10003. That track
  plays at full volume.
- Kind 6, a footstep: the game writes the value to port 0 of track 0x10008.
- Kind 7, a sound of a cutscene: the game writes the first value, the cutscene's number, to port 0 of track 0x1000F.
  A cutscene of type 1 gets its number on port 1 too. The game then queues the value for port 2 of that track. The
  value is the sound's number among the cutscene's sounds. The cutscene's table of timed events gives the frame of the
  cutscene's finish. There, the game writes 0 to port 0 of that track. A player leaves out the game's other work at
  the finish: the music's fade, the events' pause, the formation's reset and the stop of the cutscene's stream.

A queued value reaches its port once the track has read the value before it.

A player rejects a library 3 descriptor with other values in the unused fields, no sequence index, a level past
32,767, a volume that isn't a finite number of 0 or more, or more than 65,536 pieces of music, pieces of the
cutscenes' music or effects. It also rejects a scene's flag other than 0 or 1, an effect of a kind past 7, an effect
of kind 3 whose type isn't 1 to 7, an effect of kind 7 whose type isn't 0 or 1, and an effect of another kind whose
type isn't 0. It rejects a finish on an effect of a kind other than 7 too. A player that supports library 1
alone, such as HSF 1.2.0, refuses the set by the library's number. It does the same for library 2.

HSF's implementation (`src/jac`) follows a study of the code in the release's `main`, with Hagi linked in. Hagi's DSP
renders one sub-frame of 80 samples at a time, 400 a second, and the CPU side updates after each. The game's code
runs at 30 frames a second. Its frame k runs after sub-frame n, counting from 1, once 30n reaches 400(k + 1). The
start-up runs before the first sub-frame, up to its wait for the track that registers 0x20000. The rest of the
start-up runs after the first sub-frame in which that track is there. A sound starts in the game's frame 0, and the
game's code takes the effect's steps in the frames that the list above gives.

A piece has finished when its root track has closed, no DSP channel plays, and neither the FX lines' delays nor a
sub-frame's output hold a sample louder than 8. The FX lines' feedback can hold quieter values for ever. A part or an
effect plays on tracks that keep running after it. It plays while the game's code has a step of it still to take, and
while its tracks haven't read a value that the code queued or wrote to port 0 or 1. It also plays while a track of the
trees that the code writes to has opened or started a note since the sound started. The formation's and the crowd's
sounds play for as long as the game's code runs. A part or an effect has then finished in the same way as a piece.

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
Engine game keeps its effect presets in its data (`Data/SheetBin/SoundEffectList.elf`). WarioWare: Get It Together!'s
scenes put the I3DL2 reverb named `I3DL2_Default` on aux bus A unless a scene names another, and `hsfrip` puts that
reverb on aux bus A of every rip from such a game. WarioWare: Move It! keeps its list in another form. As it starts, it
puts the delay `Delay_Waltz` on aux bus A, the I3DL2 reverb `I3DL2_Waltz` on aux bus B and the light limiter
`Limiter_Waltz` on aux bus C. `hsfrip` puts those three on every rip from a game whose list has them. `--voices` and
`--no-effects` override these settings.

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
sequence's effects count as sound. The analysis waits while the effects ring (see "The model"). Either way, the length
is at most 600 seconds. Sound means a sample louder than 8 of 32,767 (-72 dB).

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

### Pikmin 2

`hsfrip` recognises Pikmin 2's Switch release by the SHA-1 of its start-up archive. The start-up archive is a file of a
folder named `AudioRes`, and its name ends in `.aaf`. The folder also holds the sequence archive, `Seqs/Seq.arc`, the
wave archives in `Banks` and the conductor archive, `Conductor.arc`. The sounds' names are in the `.asn` file with the
start-up archive's name. The folder `user` beside `AudioRes` holds the game's list of its music, `Totaka/BgmList.txt`,
and the track maps: `trackMap_Cond_T.txt` and `trackMap_Seq_T.txt` in `Totaka`, and `trackMap_Cond_W.txt` and
`trackMap_Seq_W.txt` in `Wakai`. The rip goes in a folder with the start-up archive's name. Its `.hsflib` holds the
start-up archive, the sequence archive, every wave archive that the wave systems name and the game holds, and the
conductor archive, with a library 2 descriptor. Each `.minihsf` selects a piece of music or an effect. `--bgm` takes the
music.

The game starts a piece of music by its file's name. `hsfrip` takes every file of the sequence archive but the effect
sequence, `se.bms`. A rip of a piece is named after its file without the extension. `hsfrip` holds what the game's code
sets up for each piece: its class, its kept levels and its beat interval. The game's list of its music gives the volumes
of the pieces and of the effect sequence. The game reads each name's first entry, and the leading digits of its number.
The middle bosses start `m_boss` at section 9, and the final boss starts `l_boss` at section 1. Their directors mute
tracks 10 and 11 of `m_boss`, and tracks 14 and 15 of `l_boss`. The battle of two players mutes tracks 8 to 15 of
`battle_t`. The directors of the game's other scenes take a piece's tracks from its track map. A track map gives the
counts of the piece's basic tracks and of its layers: for events, treasure, enemies nearby, battles and the ground. The
directors mute every layer as the piece starts, and fade the layers in as the game goes on. A rip plays the layers
muted, as the game starts them.

The game plays its cave music with a conductor. It picks a conductor for each floor of a cave from lists of floors. A
conductor's name gives its piece: the piece whose name starts it (`cavesoil`, `cavemetal`, `caveconc`, `cavetile`,
`caveglass`, `cavetsumiki` or `caverelax`), or `new_NN` for `new_NN_M`. The game's code names the key's conductor,
`cavekeyget.cnd`, itself. `caveboss.cnd` plays `caveboss`, and `iron.cnd` plays `a_iron`. `hsfrip` rips each piece of
the cave music once with each conductor whose name gives the piece. Each rip is named after its conductor without the
extension. The lists of floors and the code name 28 of the archive's 55 conductors. `hsfrip` rips the other 27 too. The
game never plays them. A cave's piece never repeats. Its rip plays to the length limit of 600 seconds, then fades. The
game's generator of random numbers starts at 0 in each rip (see "Library 2: Pikmin 2"). In the game, a cave's piece
plays other phrases each time.

The game never plays `2pbattle`, `a_iron`, `a_zawinul01`, `caveboss`, `c_result`, the six `ff_` pieces, `id_test`,
`result` and `u_result`. `hsfrip` takes the class of each from the requests that its tracks send. `2pbattle` plays as
`battle_t` does. `a_zawinul01` sends the cave music's requests. No conductor names it. It plays with `iron.cnd`.
`id_test` plays as a piece of class 1 that mutes no track. The others play as class 0. No track map names `a_iron`,
`a_zawinul01` or `caveboss`. Their rips play every track.

The effects follow the pieces. An effect is named as the name file names it, or after its category and index, such as
`se4-0123`. Each rip plays one state that the game can reach, as a cave's piece plays the generator's first state. Some
effects draw a number at random, and some numbers make them play nothing. Where such an effect makes no sound in the
state as the game starts, `hsfrip` tries the states after 1 to 16 draws of the sequences' generator. The rip plays the
first of them in which the effect sounds, and the descriptor lists the effect's draws. Six sounds make no sound in any
state, and `hsfrip` skips them. `result` plays programs that its bank lacks. The effect `PSSE_PL_DEMOE00_POD02` ends at
once. `PSSE_PK_AINOUTA_RU` and `PSSE_PK_AINOUTA_RA` hold only the command that ends a track. `WIISE_DPD_SCREEN_IN` and
`WIISE_DPD_SCREEN_OUT` have a volume of 0 in the sound table. The lengths follow the rules for the JAudio games, with
two additions. A note is also held while an envelope repeats a part of its table. A vibrato and a tremolo do that. A
track that has started no note and jumps back polls the game. The test for a steady sequence takes such a track as
waiting. The effects sequence's other slot tracks poll like this. The FX lines ring while their delays hold a sample
louder than 8. The tags are those of the JAudio games.

### Pikmin

`hsfrip` recognises Pikmin's Switch release by its bank file, `pikibank.bx` in a folder named `Banks`. The folder
`Seqs` beside it holds the sequence archive, `pikiseq.arc`. The folder `Banks` holds the wave archives too. The game's
code, the ExeFS's `main`, holds the sequence index and the settings. `hsfrip` reads them from the `main` of the game's
version 1.0.0, whose build ID is `342a165cc42e997edf15023143252e9f7e5c1a8d`. It refuses another build, and an input
without `main`. The rip goes in a folder with the bank file's name. Its `.hsflib` holds the bank file, the sequence
archive, every wave archive that the wave systems name and the game holds, and the sequence index, with a library 3
descriptor. Each `.minihsf` selects a piece of music, a piece of the cutscenes' music or an effect. `--bgm` takes the
two kinds of music.

The pieces of music are the files of the sequence archive from file 2 on, but the cutscenes' music. A piece is named
after its file without the extension. Its scene is scene 0 of the wave system with its file's number. A piece whose
wave system the bank file lacks is left out with a warning. The game's code gives each piece the child tracks that play
and their volume. The pieces of the cutscenes' music are those that the cutscenes' table names, and the piece that
plays as a part of the ship appears. Such a piece is named after the cutscenes' music, a hyphen and its number, such
as `demobgm-2`. The game's code gives each of them its scene. The game sets the waves of piece 2 with the diary's
scenes as its title screen starts. That piece's scene is the diary's first scene.

The effects follow, in the order of their kinds. Each row of the game's table of effects is an effect named as the row
names it. An object's effect is an action of an event of the row's type. The game refuses some actions on a new event.
`hsfrip` leaves those out. The system's effects that the game plays as other sounds take those sounds: one of the
captain's, or the system's effect of the pause. Those that play a piece of the cutscenes' music, a stream or nothing are
left out. The effects that the game's code plays with values of its own follow. Each is named after its kind and its
value, such as `system-0x19`. Then come the formation's sound with the stick pushed all the way (`formation`), the crowd
of Pikmin at each of its 29 sizes (`crowd-01` to `crowd-29`) and the captain's footstep (`footstep`). Last come the
sounds of each cutscene, such as `cutscene-004-01` for sound 1 of cutscene 4. A cutscene's events may end with its
finish. Each of its sounds then gets the frames from the sound's first event to the finish. An effect that another one
plays the same way is left out. The lengths follow the rules for the JAudio games. A note whose envelope has no attack
table counts as held from its start. The FX lines' tails and the output count as sound while they hold a sample louder
than 8. The FX lines' feedback can hold quieter values for ever. The main track's jumps back count as loops in a
subroutine or a counted loop too. Two such jumps are passes of one loop if the levels below them match: each call's
return, and each counted loop's start and count. The jumps' targets may differ. The tags are those of the JAudio games.

The Switch's release plays nothing for the crowd or for `SYSSE_JUNGLE`. The crowd's notes play instruments 0 and 2 of
bank 1. `SYSSE_JUNGLE`'s note plays key 18 of drum set 228 in bank 0. The bank file lacks the waves of all three. The
length analysis skips those sounds as sounds that make no sound. With `--no-length`, the rip holds them, silent.

## Copyright

An `.hsflib` contains copyrighted game data. Rips are for personal use.
