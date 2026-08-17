# XPCog

[![CI](https://github.com/losnoco/XPCog/actions/workflows/ci.yml/badge.svg)](https://github.com/losnoco/XPCog/actions/workflows/ci.yml)

A cross-platform Qt 6 port of [Cog](https://cog.losno.co/), the macOS audio player by
Vincent Spader and Christopher Snowhill. XPCog targets **Windows, macOS and Linux**
from a single codebase.

> **Status: milestone 1a — it plays audio.** `xpcog-cli play song.flac` decodes
> and plays through the system audio device on macOS, Linux and Windows. Decoded
> output is byte-exact against `flac -d`. The Qt application is still a shell.
> See [the roadmap](#roadmap).

## Why a port rather than a fork

Cog is excellent and thoroughly macOS-shaped. Its UI is 12 Cocoa XIBs driven by 190
bindings, persistence is Core Data, audio output is AUHAL, every DSP kernel is
Accelerate/vDSP, and decoders are Objective-C bundles discovered at runtime. None of
that survives a move off Apple platforms.

What *does* survive is the part that matters: Cog's decoder contract
(`Audio/Plugin.h`) is a narrow six-protocol interface that maps almost one-to-one onto
C++ abstract base classes, and the bulk of its format support lives in portable C/C++
libraries. XPCog keeps that contract and rebuilds everything around it.

## Architecture

```
xpcog-app ──┬── xpcog-platform (Qt: per-OS integration)
            └── xpcog-codecs ──┐
                               ├── xpcog-core   (NO Qt)
xpcog-cli ── core + codecs ────┘
```

Two rules do most of the structural work:

**`xpcog-core` never links Qt.** The engine, plugin registry, library and playlist model
depend only on the C++ standard library and codec libraries. This keeps them embeddable
and testable without a display. The rule enforces itself — `xpcog-cli` links no Qt at
all, so a `QtCore` leak breaks that target immediately — and a CMake check reports it
earlier with a clearer message.

**Codecs are registered at compile time, not discovered at runtime.** Each codec exposes
one registrar function; CMake generates a `RegisterAll.cpp` that calls every one of them
in a deterministic order. Self-registering statics are deliberately avoided: inside a
static library the linker only extracts an archive member that resolves an undefined
symbol, so a self-registering codec is silently dropped and fails at runtime rather than
at build time. See [`cmake/XPCogCodec.cmake`](cmake/XPCogCodec.cmake).

## Building

Requires **Qt 6.5+**, **CMake 3.24+**, **Ninja**, a **C++20** compiler, and
[**vcpkg**](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set.
On macOS you also need `pkg-config` (`brew install pkg-config`) for vcpkg's ports.

```sh
cmake --preset macos-debug          # or linux-debug / windows-debug
cmake --build --preset macos-debug
ctest --preset macos-debug
```

Presets read Qt from `$HOME/Qt/6.11.1/macos` by default; override with the
`XPCOG_QT_ROOT` environment variable, or set `CMAKE_PREFIX_PATH` yourself.

To build the engine without Qt at all:

```sh
cmake --preset macos-headless && cmake --build --preset macos-headless
./build/macos-headless/bin/xpcog-cli codecs
```

## Trying it

```sh
xpcog-cli info  song.flac        # format, duration, ReplayGain, tags
xpcog-cli decode song.flac out.raw   # headerless native-endian PCM
xpcog-cli play  song.flac        # to the default audio device
```

`decode` output is byte-identical to `flac -d` with the WAV header stripped, which
is how the decoder is regression-tested.

### Real-time audio

The audio callback reads from a lock-free SPSC ring, applies an atomic gain, and
zeroes any tail it could not fill. That is the whole callback: no lock, no
allocation, no `std::function`, no logging. A feeder thread does the decoding and
writes into the ring.

This is deliberately stricter than Cog, whose callback
(`Audio/Output/OutputCoreAudio.m:877`) takes an `NSLock` and enters an
`@autoreleasepool` on the real-time thread. `xpcog-cli play` reports underruns
separately for playback and for the post-stream drain, so a genuine dropout is
never confused with the expected tail.

## Roadmap

| | Milestone | Scope |
|---|---|---|
| ✅ | **M0** | Toolchain, module layout, codec registration, Qt shell |
| ✅ | **M1a** | Walking skeleton: FLAC decode → miniaudio output |
| | **M1b** | Transport, gapless playback, 8 core formats, TagLib |
| | **M1c** | ReplayGain, LPC extrapolation, HDCD, settings |
| | **M2** | SQLite library, playlist model, shuffle/repeat/queue |
| | **M3** | The Qt application: playlist view, preferences, media keys |
| | **M4** | DSP chain: equalizer, fader, downmix, time-stretch, surround |
| | **M5** | Visualization, mini player, Windows SMTC / Linux MPRIS |
| | **M6** | Breadth: the remaining ~27 decoders, DSD/DoP, HRTF, scrobbling |

Milestone 1 covers FLAC, MP3, Vorbis, Opus, AAC/ALAC, WavPack, APE and Musepack. Cog
recognises around 900 file extensions across ~35 decoders; reaching that is M6 and
beyond, and the architecture is sized for it — each additional decoder is one
`xpcog_add_codec()` call, never a refactor.

### Deliberately out of scope

The Mac App Store sandbox (`SandboxBroker`, security-scoped bookmarks), AudioUnit MIDI
instrument hosting, AppleScript, Spotlight integration and the MCP server are macOS-only
and are not being ported. A no-op `IFileAccess` seam preserves the sandbox call sites in
case that changes.

## Relationship to upstream Cog

XPCog is a derivative work and tracks Cog's behaviour closely, including quirks worth
preserving. Where it deliberately differs, the difference is documented — for example,
Cog's shuffle and next/previous operate on the *sorted* playlist order, whereas XPCog
keeps playback order canonical and treats sorting as display-only.

Upstream Cog: <https://github.com/losnoco/Cog>

## License

GPL-2.0-or-later, following upstream Cog. See [COPYING](COPYING).

Cog is copyright Vincent Spader and Christopher Snowhill. Bundled decoding and tagging
libraries are under their own licenses.
