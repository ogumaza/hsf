# xmp-hsf

XMPlay input plugin for HSF rips (`.minihsf`, and the rarely used `.hsf`). It's built on the same player core as
`hsfplay`. The player core plays a sound with HSF's model of the SDK's sound library. The library plays through HSF's
model of the console's audio renderer.

XMPlay is a 32-bit program for Windows. The plugin is therefore a 32-bit DLL, `xmp-hsf.dll`, built with clang-cl. It
requires XMPlay 3.8.2 or later.

## Installing

The Releases page has `xmp-hsf-X.Y.Z.zip`, with the DLL and the licences. Put `xmp-hsf.dll` in XMPlay's folder, or in
a folder inside it, and restart XMPlay.

## Features

- Tags become XMPlay's tags: `game` as album and `year` as date. The Message info window lists every tag except
  `_lib`, `length`, `fade`, `volume` and `utf8`.
- Length and fade come from the `length` and `fade` tags. Files without a length tag play for the default length and
  fade. They stop early when the sound ends by itself.
- The plugin's options page, among the decoders in XMPlay's options: default length and fade, endless playback, and
  rendering ahead. XMPlay stores the settings.
- During playback, the plugin renders ahead on a separate thread as fast as the CPU allows. It buffers up to 10 minutes
  and 10 seconds of audio (about 112 MiB). That's enough for the longest track that `hsfrip`'s length analysis
  produces. Seeking within the buffer is instant. Seeking further ahead waits for rendering to reach the requested
  position. Rendering ahead keeps one CPU core busy until it reaches the end of the track or the buffer limit. Audio
  falls out of the buffer only in a longer track or in endless playback. Such tracks also save the player's state every
  2 seconds. Seeking back before the buffered audio restores the latest state before the target and renders from there.
  At most 128 states are kept. The intervals between them grow as the track continues. With rendering ahead turned off,
  every track saves states. Seeking ahead then renders up to the target.
- Output is 16-bit stereo at 48,000 Hz. That's the rate of the Switch's audio renderer. The output isn't resampled.
- When a track starts, the plugin loads the track's `.hsflib`. A `.hsflib` holds a whole sound archive. The tracks of a
  game with a large archive therefore take longer to start and use more memory.

## Building

**On GitHub (no Windows machine needed):** the `XMPlay plugin` workflow (`.github/workflows/xmplay.yml`) builds the DLL
and uploads it with the licences. Start it from the repository's Actions tab (Run workflow). GitHub lists a manual
workflow only once it's on the default branch (`main`). The workflow requires GitHub Actions to be enabled for the
repository. It downloads the XMPlay plugin SDK from un4seen.com (`externals/xsflib/ci/xmplay-sdk`).

**On Windows:** you need Visual Studio 2022 or later with the C++ workload and the "C++ Clang tools for Windows"
component, plus CMake 3.20 or later. Download the XMPlay plugin SDK (<https://www.un4seen.com/files/xmp-sdk.zip>) and
unpack it. Use the folder that holds `xmpin.h` as the SDK path below.

```
cmake -S . -B build-xmplay -A Win32 -T ClangCL -DXMPLAY_SDK_DIR=C:/path/to/xmp-sdk
cmake --build build-xmplay --config Release --target xmp-hsf
```

This produces `build-xmplay/plugins/xmplay/Release/xmp-hsf.dll`. Everything is built with the static C runtime (`/MT`).
The plugin therefore requires no runtime DLLs.

## Performance

Playback runs HSF's models of the sound library and the audio renderer on one CPU core. XMPlay is a 32-bit program. A
32-bit process may not have room for the largest sound archives.

## Licence

Like the rest of HSF, the plugin is MIT-licensed. It uses xsflib (MIT), ntdlib (MIT) and zlib. Ship `LICENSE` and
`THIRD-PARTY-NOTICES.md` (or their text) with a build. The XMPlay plugin SDK states no licence. The plugin uses only
the SDK's declarations of XMPlay's interface. No code from the SDK goes into it.
