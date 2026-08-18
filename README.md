# XPCog

[![CI](https://github.com/losnoco/XPCog/actions/workflows/ci.yml/badge.svg)](https://github.com/losnoco/XPCog/actions/workflows/ci.yml)

A cross-platform Qt 6 port of [Cog](https://cog.losno.co/), the macOS audio player by
Vincent Spader and Christopher Snowhill. XPCog targets **Windows, macOS and Linux**
from a single codebase.

> **Status: milestone 5 — a player the desktop knows about.**
> A Qt window with a playlist, transport, seek bar, file browser, preferences,
> undo, drag-and-drop and a persistent library. Gapless across formats *and*
> sample rates, ReplayGain, cue sheets, HDCD. A 31-band equaliser at Cog's
> frequencies, transport fades, and matrix downmix/upmix. Media keys and Now
> Playing on all three platforms — MediaPlayer.framework, SMTC and MPRIS — plus a
> tray icon on Windows and Linux, the Dock menu on macOS, one instance per user,
> a spectrum analyser on Cog's own band frequencies, a mini player, and a taskbar
> badge and progress bar.
> See [the roadmap](#roadmap), or [`docs/PORTING.md`](docs/PORTING.md) for the
> full plan and the reasoning behind the structure.

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

Point `XPCOG_QT_ROOT` at your Qt installation — the directory containing `bin`
and `lib/cmake`, as produced by the Qt installer or aqtinstall. Qt deliberately
does not come from vcpkg, because mixing a vcpkg Qt with vcpkg's other ports is
a known source of grief.

```sh
export XPCOG_QT_ROOT=~/Qt/6.11.1/macos      # or .../gcc_64 on Linux
cmake --preset macos-debug                   # or linux-debug / windows-debug
cmake --build --preset macos-debug
ctest --preset macos-debug
```

```bat
:: Windows, from a Developer Command Prompt
set XPCOG_QT_ROOT=C:\Qt\6.11.1\msvc2022_64
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

If you would rather manage `CMAKE_PREFIX_PATH` yourself, leave `XPCOG_QT_ROOT`
unset and it stays out of the way.

The Windows presets build with MSVC, so the Qt installed must be the
**`msvc2022_64`** component and not `mingw_64`. The two are not
ABI-interchangeable, and pointing `XPCOG_QT_ROOT` at a MinGW Qt gets past the
path check in `cmake/XPCogQt.cmake` — the layout is identical — before failing
much later and much less clearly. Any MSVC from 2019 onward links Qt's
msvc2022 binaries; a VS 2026 toolset (14.5x) is fine.

`cmake --build build/windows-debug --target deploy` runs `windeployqt`, copying
the Qt runtime and plugins beside `XPCog.exe` so it can be launched from Explorer
rather than only from a shell with Qt on `PATH`. Windows has no rpath, so a
freshly built `XPCog.exe` cannot start without one or the other.

### Other prerequisites

`nasm` is required on every platform for FFmpeg's assembly — vcpkg downloads it
itself on Windows, and expects the package manager to supply it elsewhere. macOS
also needs `pkg-config` for vcpkg's ports.

```sh
brew install ninja pkg-config nasm                              # macOS
sudo apt install ninja-build pkg-config nasm autoconf automake libtool  # Debian/Ubuntu
```

macOS builds the app icon from `app/icons/xpcog.icon`, an Icon Composer package,
using `actool` from **Xcode 26 or newer** — not the Command Line Tools. Without
it the build still succeeds and falls back to a committed `.icns`, saying so as
it configures; what is lost is the icon's container and its dark and tinted
appearances, which the system composes from the layered source and cannot
recover from a bitmap.

Sixteen tests build their fixtures by shelling out to command-line **encoders**,
and *skip silently* when those are absent — a skip is not a failure, so the suite
still reports success while the gapless, seek and cue-span tests never run. Install
them to get real coverage:

```sh
brew install flac vorbis-tools opus-tools lame wavpack ffmpeg    # macOS
sudo apt install flac vorbis-tools opus-tools lame wavpack ffmpeg  # Debian/Ubuntu
```

On Windows, four of the six come from winget and the other two from their upstream
builds:

```bat
winget install Xiph.FLAC Gyan.FFmpeg LAME.LAME Mozilla.opus-tools
```

`oggenc` and `wavpack` are not packaged. Take `wavpack-5.9.0-x64.zip` from
[wavpack.com](https://www.wavpack.com/downloads.html) and `oggenc2` from
[RareWares](https://www.rarewares.org/ogg-oggenc.php), and put `wavpack.exe` and
`oggenc.exe` (renamed from `oggenc2.exe`) anywhere on `PATH`.

Watch the skip count in `ctest` output, not just the pass rate — a full run is
247 tests and **0 skipped**. Note that the encoders alone were not enough before
the fixture commands stopped assuming a POSIX shell: `2>/dev/null` under
`cmd.exe` fails the whole command, which every call site read as "encoder
missing". See `tests/TestShell.hpp`.

To build the engine without Qt at all:

```sh
cmake --preset macos-headless && cmake --build --preset macos-headless
./build/macos-headless/bin/xpcog-cli codecs
```

## Trying it

```sh
xpcog-cli codecs                     # what this build can decode
xpcog-cli info   song.flac           # format, duration, ReplayGain, tags
xpcog-cli expand album.cue           # the tracks a playlist or cue sheet holds
xpcog-cli info   album.cue#3         # one track of a single-file album
xpcog-cli decode song.flac out.raw   # headerless native-endian PCM
xpcog-cli play   a.flac b.m4a c.mp3  # gapless across the queue
```

### Cue sheets

A `.cue` expands to one URL per track (`album.cue#1`, `#2`, …). Opening one decodes
the referenced audio file, seeks to that track's `INDEX 01`, and stops at the next
track's start, so each track reports its own duration and metadata and seeks
relative to itself.

Two bugs in Cog's parser are fixed rather than reproduced, both of which corrupt
real albums:

- Cog keeps one `artist` variable for the whole sheet and never resets it per
  track, so a single track-level `PERFORMER` mis-credits every following track.
  Track-level fields here fall back to the album value instead.
- A non-`AUDIO` `TRACK` is skipped, but Cog still lets its `INDEX` create an
  entry, so a mixed-mode disc gains a bogus track that decodes to noise.

### Formats

Dedicated decoders for FLAC, Ogg Vorbis, Opus, MP3 (libmpg123) and WavPack, plus
FFmpeg as the catch-all for AAC, ALAC, WMA, AC3, DTS, TAK, TTA, APE, PCM and the
MP4/MKV/ASF containers — 30-odd extensions in total.

Selection follows Cog's rules: extension first, then MIME type, with several
claimants tried in descending priority. FFmpeg registers *below* default priority,
so a dedicated decoder always wins for formats that have one, while FFmpeg still
picks up files those decoders reject.

Every codec is checked against one asymmetric reference signal — 440 Hz in the left
channel, 660 Hz in the right, at different levels — verifying per-channel frequency
and amplitude. That catches swapped, duplicated and silent channels, which a
duration or size check would miss. Adding a codec means adding a row to that table.

`decode` output is byte-identical to `flac -d` with the WAV header stripped, which
is how the decoder is regression-tested.

### Gapless playback

When a decoder reaches end of stream the engine opens the next track immediately,
while the audio already buffered is still playing, and keeps writing into the same
ring — so a same-format handoff needs no device reconfiguration and produces no gap.
This is the shape of Cog's `-endOfInputReached:`. Track changes are announced when
the seam becomes *audible*, not when it is decoded.

The seam is covered by tests that run the real engine against a capturing output,
so they are deterministic and need no audio device: sample-exactness against
separately-decoded references, waveform continuity across the join, notification
ordering, and a three-track case where a per-seam off-by-one accumulates rather
than cancels. The tests were confirmed to fail when a chunk is deliberately dropped
at each seam.

A track at a **different sample rate** joins gaplessly too. The device stays at the
first track's format and later tracks are resampled into it (libsoxr, as in Cog),
because reconfiguring the device mid-stream cannot be seamless. The outgoing
resampler is flushed before it is reconfigured, so the few milliseconds held in its
delay line — exactly the samples that meet the seam — are not lost.

Matching rates bypass the resampler entirely, so a same-rate file is passed through
bit-exactly rather than being needlessly recomputed.

### HDCD

HDCD codes are decoded when present, expanding the extra resolution the format
carries. Because the decoder runs on *every* 16-bit 44.1 kHz stereo lossless
stream — almost all CD-sourced material, and almost none of it actually HDCD — it
has to be bit-transparent when no codes are found. It is, and that is asserted
rather than assumed.

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
| ✅ | **M1b** | Transport, gapless, seven decoders, M3U/PLS playlists and cue sheets |
| ✅ | **M1c** | ReplayGain, resampling, settings, HDCD |
| ✅ | **M2** | SQLite library, playlist model, shuffle/repeat/queue, scanner, tag reading |
| ✅ | **M3** | The Qt application: playlist view, preferences, undo, media keys |
| ✅ | **M4** | DSP chain: equalizer, fader, downmix/upmix. Time-stretch dropped by decision; FreeSurround deferred |
| 🚧 | **M5** | SMTC, MPRIS, tray icon / Dock menu, single instance, app icon, spectrum, mini player, taskbar badge; NSDockTile to come |
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

The full porting plan, milestone-by-milestone progress, and the complete list of
deliberate behaviour differences live in [`docs/PORTING.md`](docs/PORTING.md).

Upstream Cog: <https://github.com/losnoco/Cog>

## License

GPL-2.0-or-later, following upstream Cog. See [COPYING](COPYING).

Cog is copyright Vincent Spader and Christopher Snowhill. Bundled decoding and tagging
libraries are under their own licenses.
