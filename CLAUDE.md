# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

XPCog is a wxWidgets audio player for Windows, macOS and Linux, ported from
[Cog](https://github.com/losnoco/Cog) (macOS, Objective-C). C++20, CMake, vcpkg.

## Versioning

**Any change bumps the version.** Every commit that touches this repository —
code, build files, documentation, this file included — raises the version number
as part of the same change. There is no "too small to bump".

The version lives in exactly two places, and they are kept identical:

- `CMakeLists.txt`, the `VERSION` argument to `project(XPCog ...)`
- `vcpkg.json`, the `"version"` field

Everything else derives from the first of those and must not be edited by hand:
`core/include/xpcog/core/Version.hpp.in` is configured into `Version.hpp`
(`kVersionMajor`/`Minor`/`Patch`, `kVersionString`), `app/XPCog.rc.in` takes the
Windows `FileVersion` and `ProductVersion`, and `app/CMakeLists.txt` sets the
macOS bundle's `CFBundleShortVersionString` and `CFBundleVersion`. The version
string is user-visible in the About dialog, in `xpcog-cli`'s banner and in the
HTTP source's `User-Agent`.

`README.md` is the one place that spells a version out by hand: the Windows
installer section shows `XPCog-<version>-x64-setup.exe` in two examples, and those
follow the bump.

Which component moves:

- **Patch** — the default: fixes, documentation, tests, build tweaks, a codec
  added without changing the plugin contract.
- **Minor** — a user-visible feature arrives, or a `core`/`codecs` public header
  changes shape.
- **Major** — reserved; not moved without being asked.

Bump both files in the same commit as the change itself, never as a follow-up.

## Build and test

Requires CMake 3.24+, Ninja, a C++20 compiler, and vcpkg with `VCPKG_ROOT` set.
Everything is preset-driven; each preset builds into `build/<preset-name>/`, and
binaries land in `build/<preset-name>/bin/`.

```sh
cmake --preset macos-debug            # configure  (or linux-debug / windows-debug)
cmake --build --preset macos-debug    # build
ctest --preset macos-debug            # test
```

Preset families: `macos-*`, `linux-*`, `windows-*`, each with `-debug` and
`-release`. Variants on top of those:

- `*-app-debug` / `*-app-release` — application only, no CLI and no tests.
- `macos-headless` / `linux-headless` — `XPCOG_BUILD_APP=OFF`, no toolkit built at
  all. The fastest way to exercise `core` and `codecs`.
- `linux-repo-debug` / `linux-repo-release` — Linux, taking every dependency the
  distribution already has via pkg-config and asking vcpkg only for the rest
  (`XPCOG_USE_SYSTEM_LIBS=ON`, probed in `cmake/XPCogSystemDeps.cmake` **before**
  `project()`). Separate build trees on purpose; do not flip the cache variable
  inside a plain `linux-*` tree, which is refused rather than obeyed.

The presets turn on more than the option defaults do — FFmpeg, vgmstream, PSF,
SID, MIDI, AdPlug, libvgm, Sentry are all `OFF` for a bare `cmake` and `ON` in
`base`. Configure with a preset unless you specifically want a minimal build.

Two test binaries, both Catch2 v3 and both registered with ctest through
`catch_discover_tests`: `xpcog-tests` (core and codecs) and `xpcog-app-tests`
(app-layer, built as `xpcog-appcore`).

```sh
ctest --preset macos-debug -R Gapless          # by ctest test name
./build/macos-debug/bin/xpcog-tests "[gapless]"  # by Catch2 tag — the usual way
./build/macos-debug/bin/xpcog-tests --list-tests
```

Tags follow the subsystem (`[dsp]`, `[playlist]`, `[library]`, `[cogimport]`,
`[lastfm]`, `[scrobbler]`, `[timestretch]`, `[gapless]`, `[hls]`, `[midi]`…).
Tags starting with a dot are hidden and only run when named: `[.lastfmlive]`
(hits the real Last.fm API), `[.ratedevice]` and `[.integerdevice]` (want real
hardware).

Other targets: `xpcog-no-toolkit` (layering check, runs as part of `ALL`),
`sign` on macOS (needs `XPCOG_CODESIGN_IDENTITY`), `installer` on Windows (needs
NSIS; use a **release** tree).

### Skips are the thing to watch

A large number of tests build their fixtures by shelling out to command-line
encoders (`flac`, `oggenc`, `opusenc`, `lame`, `wavpack`, `ffmpeg`) and **skip
silently** when those are missing — the suite still reports success while the
gapless, seek and cue-span tests never ran. Read the skip count, not just the
pass rate. Fixture commands must go through `tests/TestShell.hpp`; a bare
`2>/dev/null` fails under `cmd.exe` and reads as "encoder missing" everywhere.

Corpus-gated tests need material no package manager ships (game rips, ROMs, a
SoundFont) and are pointed at it by environment variable: `XPCOG_PSF_CORPUS`,
`XPCOG_VGM_CORPUS`, `XPCOG_SID_CORPUS`, `XPCOG_MIDI_CORPUS`,
`XPCOG_HIVELY_CORPUS`, `XPCOG_ADPLUG_CORPUS`, `XPCOG_ORGANYA_CORPUS`,
`XPCOG_SYNTRAX_CORPUS`, `XPCOG_DSD_CORPUS`, `XPCOG_SC55_ROMS`, `XPCOG_SOUNDFONT`,
`XPCOG_SHORTEN_FILE`, `XPCOG_DSD_FILE`.

`xpcog-cli` is the headless way to exercise the engine: `codecs`, `info`,
`expand`, `decode`, `play`.

## Architecture

```
xpcog-app ──┬── xpcog-platform (per-OS integration; NO toolkit)
            └── xpcog-codecs ──┐
                               ├── xpcog-core   (NO toolkit)
xpcog-cli ── core + codecs ────┘
```

**Only `xpcog-app` links a UI toolkit.** `core`, `codecs` and `platform`'s *public
headers* name no toolkit at all — `platform`'s implementations talk to Win32,
C++/WinRT, CoreFoundation, MediaPlayer.framework and GDBus, but nothing they do
may leak into a header the app includes. This is enforced by
`cmake/CheckNoToolkit.cmake` (which also fails on any Qt include anywhere), and
again by `xpcog-cli` linking no toolkit, so a leak breaks that target. Keep it
that way; it is the rule the Qt→wxWidgets move was a test of.

**Codecs register at compile time.** Each codec exposes one registrar function and
is declared with `xpcog_add_codec(NAME … REGISTER … SOURCES … DEPS …)`
(`cmake/XPCogCodec.cmake`); `codecs/CMakeLists.txt` generates a `RegisterAll.cpp`
calling all of them in a deterministic order. Do **not** use self-registering
statics — inside a static library the linker drops the object and the codec
vanishes at runtime instead of failing at build time. A codec that resolves a
library with `find_package()` must pass `GLOBAL` so the imported target escapes
its directory scope.

**The plugin contract** is `core/include/xpcog/core/Plugin.hpp` plus the
descriptors in `PluginRegistry.hpp`: `ISource` (opens a URL, chosen by scheme),
`IDecoder` (bytes → PCM), a *container* (expands one URL into several — cue
sheet, playlist, archive), a *metadata reader*, and a *source wrapper* (layered
over a source by extension, for files whose bytes are not what the decoder
wants). Selection is extension first, then MIME type, candidates tried in
descending `Priority`; FFmpeg deliberately registers below default priority so
dedicated decoders win. Adding a format is one `xpcog_add_codec()` call plus a
row in the conformance table in `tests/codecs/test_conformance.cpp`, which checks
every codec against one asymmetric reference signal (440 Hz left, 660 Hz right,
different levels) to catch swapped, duplicated or silent channels.

**Settings are an X-macro.** `core/include/xpcog/core/settings.def` is the single
source of truth — `XPCOG_SETTING(Ident, Type, "cogKey", default)` — included
several times with the macro defined differently. Keys are deliberately identical
to Cog's `NSUserDefaults` keys so an existing Cog plist imports verbatim. Add a
setting there, not in `Settings.hpp`.

**The audio path**: a feeder thread decodes into a lock-free SPSC ring
(`RingBuffer`), and the output callback only reads from the ring, applies an
atomic gain, and zeroes any tail — no lock, no allocation, no `std::function`,
no logging on the real-time thread. `AudioEngine` owns the DSP chain
(`AudioConverter` for rate/format/HDCD/ReplayGain/FreeSurround, then the
`chain_` of `Equalizer` and `Fader`; `TimeStretch` sits outside the chain because
it changes the frame count). Gapless means opening the next decoder while the
previous track's audio is still playing and writing into the same ring; track
changes are announced when the seam becomes *audible*, not when it is decoded.
`OfflineOutput` is what makes all of this testable without a device — but note it
cannot exercise wall-clock timing.

**Where things live**: `core/` (engine, plugin registry, SQLite library, playlist
model, settings, HTTP, scrobbling), `codecs/` (one directory per decoder),
`platform/` (per-OS integration behind toolkit-free headers), `app/` (wxWidgets
UI), `tools/cli/`, `tests/`, `assets/`, `packaging/windows/`.

**`vendor/` vs `ports/`**: `ports/` holds vcpkg overlay ports for dependencies
with a real upstream release or pinned commit (vgmstream, libsidplayfp, mGBA,
libvgm, AdPlug, rubberband, spessasynth-core…) — preferred, because CI compiles
them once and restores from the binary cache. `vendor/` is for sources with no
upstream to point at, mostly the emulator cores behind the PSF family and
Cog's own small libraries. See `ports/README.md`.

## Docs

`docs/PORTING.md` is the long one: the survey, the structural decisions, progress,
the **deliberate differences from Cog**, the verification strategy, known gaps,
and a *Where to pick up next* section at the end. Consult it before changing
behaviour that mirrors Cog — differences are meant to be documented, not
accidental. `docs/MIDI.md` covers the three MIDI backends, `docs/COGIMPORT.md` the
Cog library import, `docs/HIGHLYCOMPLETE.md` the eight PSF emulator cores, and
`docs/WXPORT.md` the Qt→wxWidgets move.

Deliberately out of scope and not to be ported: the Mac App Store sandbox,
AudioUnit MIDI instrument hosting, AppleScript, Spotlight, the MCP server.
