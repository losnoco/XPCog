# XPCog

[![CI](https://github.com/losnoco/XPCog/actions/workflows/ci.yml/badge.svg)](https://github.com/losnoco/XPCog/actions/workflows/ci.yml)

A cross-platform port of [Cog](https://cog.losno.co/), the macOS audio player by
Vincent Spader and Christopher Snowhill, built on wxWidgets. XPCog targets
**Windows, macOS and Linux** from a single codebase.

> **Status: milestone 6 — breadth.**
> A window with a playlist, transport, seek bar, file browser, preferences,
> undo, drag-and-drop and a persistent library. Gapless across formats *and*
> sample rates, ReplayGain, cue sheets, HDCD. A 31-band equaliser at Cog's
> frequencies, transport fades, matrix downmix/upmix and FreeSurround stereo-to-5.1.
> Media keys and Now
> Playing on all three platforms — MediaPlayer.framework, SMTC and MPRIS — plus a
> tray icon on Windows and Linux, the Dock menu on macOS, one instance per user,
> a spectrum analyser on Cog's own band frequencies, a mini player, and a taskbar
> badge and progress bar. Now playing over HTTP too — File → Open URL, internet
> radio included, with SHOUTcast stream titles live in the window as the
> station announces them, HLS for the stations that use it, and chained Ogg so a
> stream survives its own track changes. Breadth since: archives played in
> place, tracker modules, game music rips, vgmstream's console formats, the
> whole PSF family on all eight of its emulator cores, Commodore 64 tunes,
> Musepack, Monkey's Audio Link files, and MIDI on a SoundFont bank — one ships
> with it — a Sound Blaster's OPL3, or an emulated Roland SC-55 with its front
> panel.
> **842 extensions** across 23 decoders.
> The interface is wxWidgets, and there is no Qt anywhere in the tree — which is
> also why there is no dependency outside vcpkg, no environment variable pointing
> at a toolkit, and no deploy step.
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
xpcog-app ──┬── xpcog-platform (per-OS integration; NO toolkit)
            └── xpcog-codecs ──┐
                               ├── xpcog-core   (NO toolkit)
xpcog-cli ── core + codecs ────┘
```

Two rules do most of the structural work:

**Only `xpcog-app` links a UI toolkit.** The engine, plugin registry, library and
playlist view depend only on the C++ standard library and codec libraries, which keeps
them embeddable and testable without a display. `xpcog-platform` links none either: it
talks to Win32, C++/WinRT, CoreFoundation, MediaPlayer.framework and GDBus, and its
public headers name no toolkit at all.

The rule enforces itself — `xpcog-cli` links nothing, so a leak breaks that target
immediately — and `cmake/CheckNoToolkit.cmake` reports it earlier with a clearer
message. It was put to the test in earnest: the interface was moved from Qt 6 to
wxWidgets without `core/` or `codecs/` changing at all. See
[`docs/WXPORT.md`](docs/WXPORT.md).

**Codecs are registered at compile time, not discovered at runtime.** Each codec exposes
one registrar function; CMake generates a `RegisterAll.cpp` that calls every one of them
in a deterministic order. Self-registering statics are deliberately avoided: inside a
static library the linker only extracts an archive member that resolves an undefined
symbol, so a self-registering codec is silently dropped and fails at runtime rather than
at build time. See [`cmake/XPCogCodec.cmake`](cmake/XPCogCodec.cmake).

## Building

Requires **CMake 3.24+**, **Ninja**, a **C++20** compiler, and
[**vcpkg**](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set. Every
dependency, wxWidgets included, comes from vcpkg — there is nothing to install
separately and no environment variable to point at a toolkit.

```sh
cmake --preset macos-debug                   # or linux-debug / windows-debug
cmake --build --preset macos-debug
ctest --preset macos-debug
```

```bat
:: Windows, from a Developer Command Prompt
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

There is no deploy step. On Windows, vcpkg's applocal pass copies every dependent
DLL beside `XPCog.exe` as part of the build — the same pass that has always placed
FFmpeg's and TagLib's — so a freshly built binary starts from Explorer. On macOS the
triplet is static and there is nothing to copy. What remains is signing, and
`cmake --build build/macos-debug --target sign` does that when
`XPCOG_CODESIGN_IDENTITY` names a Developer ID identity.

wxWidgets is declared under a `gui` feature rather than as a plain dependency, so
a headless configuration (`-D XPCOG_BUILD_APP=OFF`) builds no toolkit at all — and
on Linux, no GTK. If you would rather use a wxWidgets your distribution already
supplies, `cmake/XPCogWx.cmake` falls back to CMake's `FindwxWidgets` when no
vcpkg one is present.

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

Forty-one tests build their fixtures by shelling out to command-line
**encoders**, and *skip silently* when those are absent — a skip is not a failure,
so the suite still reports success while the gapless, seek and cue-span tests never
run. Install them to get real coverage:

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

Watch the skip count in `ctest` output, not just the pass rate. With the encoders
installed a full run is **497 tests, 65 skipped**, and those 64 want something no
package manager can supply: rips of copyrighted game programs, a Roland's
firmware, a SoundFont bank. Point `XPCOG_PSF_CORPUS`, `XPCOG_VGM_CORPUS`,
`XPCOG_SID_CORPUS`, `XPCOG_MIDI_CORPUS`, `XPCOG_HIVELY_CORPUS`,
`XPCOG_ADPLUG_CORPUS`, `XPCOG_ORGANYA_CORPUS`, `XPCOG_SYNTRAX_CORPUS`,
`XPCOG_DSD_CORPUS`, `XPCOG_SC55_ROMS`, `XPCOG_SOUNDFONT`, `XPCOG_SHORTEN_FILE`
or `XPCOG_DSD_FILE` at one and the matching cases run. `XPCOG_VGM_CORPUS` is read
by two codecs' tests — vgmstream's and libvgm's — because a folder of game rips
holds streamed audio and chip logs side by side. Organya, Shorten and the
`silence://` track need a corpus least: most of what those three assert runs
against files the tests write themselves.
Without the encoders, 41 more go quiet.

Note that the encoders alone were not enough before the fixture commands stopped
assuming a POSIX shell: `2>/dev/null` under `cmd.exe` fails the whole command,
which every call site read as "encoder missing". See `tests/TestShell.hpp`.

To build the engine with no toolkit at all:

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

Dedicated decoders for FLAC, Ogg Vorbis, Opus, MP3 (minimp3), WavPack and
Musepack (libmpcdec), plus FFmpeg as the catch-all for AAC, ALAC, WMA, AC3, DTS,
TAK, TTA, APE, PCM and the MP4/MKV/ASF containers.

Beyond those: tracker modules (libopenmpt), chiptune rips (Game_Music_Emu),
console streamed audio (vgmstream), the PSF family on all eight of the emulator
cores behind it — USF, GSF, 2SF, SNSF, SSF/DSF, NCSF, PSF/PSF2 and QSF — and
Commodore 64 tunes (libsidplayfp). MIDI is its own thing again: a score rather
than a recording, so what it sounds like is a choice of synthesiser. Fourteen
extensions -- `.mid` and `.midi` among them, with HMI, XMI, Doom's MUS and
Loudness LDS each reaching their own parser in midi_processing -- render on a
SoundFont bank (SpessaSynth), an emulated Sound Blaster (Nuked OPL3, under two
different drivers), or a Roland SC-55mkII running its own firmware, if you have
the ROMs.

**A bank ships with it**, so MIDI plays on real instruments out of the box
rather than on an FM chip: `GeneralUserXG-SFeTest.sf3`, which is what Cog
bundles, together with the `tg300b` map that XPCog selects instead when a
sequence announces itself as GS or GM2. Point `soundFontPath` at your own bank
to replace it, or drop one beside a file — `song.sf2`, or `Album/Album.sf2` for
a folder — to override it for that music alone. An RMID that carries its own
bank inside it beats all of those, since that bank is part of the music.

Archives are a *source* rather than a format,
so a `.zip` of FLAC plays without being unpacked first, and a Monkey's Audio Link
(`.apl`) is a *range* within one -- the same shape as a cue sheet track, which is
how a single-file CD rip becomes an album.

`xpcog-cli codecs` prints what a given build claims; a default one is 23 decoders
and 842 extensions. Cog recognises around 900 across ~35 decoders.

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
| ✅ | **M4** | DSP chain: equalizer, fader, downmix/upmix, FreeSurround. Time-stretch dropped by decision |
| ✅ | **M5** | SMTC, MPRIS, tray icon / Dock menu, single instance, app icon, spectrum, mini player, taskbar badge. NSDockTile dropped by decision |
| 🚧 | **M6** | Breadth. HTTP and internet radio, HLS, chained Ogg, archive sources, tracker modules, game music rips, vgmstream, the eight PSF cores, SID, Musepack, APL, DSD, output device selection and exclusive mode, and MIDI on OPL3, SpessaSynth and an emulated SC-55 with its front panel all done; DoP output, `.dsf`/`.dff`, the remaining decoders, `cogimport`, HRTF, scrobbling and global hotkeys to come |
| ✅ | **M7** | The interface moved from Qt 6 to wxWidgets. `core/` and `codecs/` unchanged; `platform/` de-Qt'd and now links no toolkit either. Qt was the last dependency outside vcpkg, and the deploy step went with it. See [`docs/WXPORT.md`](docs/WXPORT.md) |

Picking this up on another machine? [`docs/PORTING.md`](docs/PORTING.md) ends with
**Where to pick up next** — the remaining work itemised, in order, each with where
Cog does it and what the trap is.

Milestone 1's formats were FLAC, MP3, Vorbis, Opus, AAC/ALAC and WavPack, with APE
and Musepack arriving through FFmpeg rather than their own decoders; Musepack has
its own now, and APE still does not, because Cog has none either. M6 has taken the
recognised extension count from 30-odd to **842**, against the roughly 900 Cog
recognises across ~35 decoders. Getting the rest is the remainder of M6 and beyond,
and the architecture is sized for it — each additional decoder is one
`xpcog_add_codec()` call, never a refactor, and every one added so far has cost
exactly that.

### Deliberately out of scope

The Mac App Store sandbox (`SandboxBroker`, security-scoped bookmarks), AudioUnit MIDI
instrument hosting, AppleScript, Spotlight integration and the MCP server are macOS-only
and are not being ported. A no-op `IFileAccess` seam preserves the sandbox call sites in
case that changes.

AudioUnit hosting is one of Cog's four MIDI backends, not MIDI itself — `.mid` and
its dozen relatives play here through the other three, all of which have landed:
SpessaSynth, Nuked OPL3 and Nuked SC-55. See [`docs/MIDI.md`](docs/MIDI.md).

## Relationship to upstream Cog

XPCog is a derivative work and tracks Cog's behaviour closely, including quirks worth
preserving. Where it deliberately differs, the difference is documented — for example,
Cog's shuffle and next/previous operate on the *sorted* playlist order, whereas XPCog
keeps playback order canonical and treats sorting as display-only.

The full porting plan, milestone-by-milestone progress, and the complete list of
deliberate behaviour differences live in [`docs/PORTING.md`](docs/PORTING.md).
Work that spans several commits gets its own plan beside it —
[`docs/HIGHLYCOMPLETE.md`](docs/HIGHLYCOMPLETE.md) staged the eight emulator
cores behind the PSF formats, one at a time, and is now the record of all eight.

Upstream Cog: <https://github.com/losnoco/Cog>

## License

GPL-2.0-or-later, following upstream Cog. See [COPYING](COPYING).

Cog is copyright Vincent Spader and Christopher Snowhill. Bundled decoding and tagging
libraries are under their own licenses. Interface icons are [Lucide](https://lucide.dev)
under the ISC license — see [`app/icons/lucide/LICENSE`](app/icons/lucide/LICENSE).
