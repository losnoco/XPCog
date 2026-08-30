# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

XPCog is a wxWidgets audio player for Windows, macOS and Linux, ported from
[Cog](https://github.com/losnoco/Cog) (macOS, Objective-C). C++20, CMake, vcpkg.

## Versioning

**Code changes bump the version; nothing else does.** Plenty of commits here are
too small to bump, and they should not. A bump means the built player is not the
one the last version number described.

Bumps: anything under `core/`, `codecs/`, `platform/`, `app/`, `tools/`,
`vendor/`, `ports/`, or a build file that changes what comes out of the build.

Does not bump: documentation (`README.md`, `docs/`, this file), comments,
CI configuration, and test-only changes — the suite is not what ships.

Bump in the same commit as the code, never as a follow-up.

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

Which component moves is [semver](https://semver.org), read against the public
surface this project actually has: the `core` and `codecs` headers, the plugin
contract, the setting keys in `settings.def`, the CLI's commands and output, and
the CMake options and preset names.

- **PATCH** — backwards-compatible fixes. A bug fix, a build change that alters
  the output, a codec added without touching the plugin contract.
- **MINOR** — backwards-compatible additions. A user-visible feature, a new
  setting, a new option or preset, a new interface alongside the existing ones.
- **MAJOR** — incompatible changes to that surface.

**1.0.0 has been released, so all three components now move.** Until then the
version was 0.x, semver put no compatibility promise on it, and a breaking change
went in the minor; that is no longer the rule. A change that breaks the surface
listed above is a MAJOR bump and needs saying out loud, not absorbing into a
minor.

## Commit messages

The headline says what changed, in the plainest words that fit. It is one line in
`git log`, not an announcement: no "comprehensive", no "robust", no "complete
overhaul", no "significantly improved", no superlatives, no emoji. Do not claim a
fix is total or final — say what the code now does. A small change gets a small
headline, and that is not a failure to sell it.

Prefer the concrete over the grand, and the specific subject over the abstract
one — "Wrap a preference note to its column, not to the pane" rather than
"Overhaul the preferences layout system". Present tense, about 72 characters, and
whatever does not fit goes in the body.

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

On Linux there is a third, `xpcog-gui-tests`, and it is the one that needs a
screen: it opens the preferences dialog and walks its panes, which is the only
way to catch a layout handler that recurses until the stack runs out. It is
registered as a single `add_test()` rather than discovered, so it can be run
under `xvfb-run` where CMake found one; without a display it skips. Linux only
because that is where a display can be conjured — the code under test is the
same on all three.

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
`installer` on Windows (needs NSIS; use a **release** tree), and on macOS `sign`,
`dmg` and `notarize` (`packaging/macos/`; the identity comes from
`XPCOG_CODESIGN_IDENTITY` and the notary credentials from the environment only —
see the README section).

On Linux `package` builds `XPCog-<version>-<arch>.tar.gz` — CPack's `TGZ`
generator over the install rules, stripped, and the only generator enabled on
purpose (see `packaging/linux/CMakeLists.txt` for why not `DEB` or `RPM`). The
install tree is the real artefact there and `cmake --install` produces it; the
tarball is that tree compressed. `packaging/linux/` adds the
desktop integration that goes with it — a `.desktop` file, AppStream metainfo and
hicolor icons, all named for `XPCOG_DESKTOP_ID` (`co.losno.XPCog`, set in the root
`CMakeLists.txt`). Four files have to agree on that ID, one of them at run time:
MPRIS publishes it as `DesktopEntry`. `desktop-file-validate` and `appstreamcli
validate` are the checks; nothing at run time reads either file, so a mistake in
them is silent.

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

`tools/ci-watch/ci-watch.sh` watches a GitHub Actions run and prints one line per
job as it finishes — the run for the checked-out commit with no argument, or a
run id. Two details are the reason it exists rather than a poll loop written on
the spot: it parses with `gh`'s built-in `--jq`, because a standalone `jq` is not
on a stock Windows box, and a job that did not succeed names the step it died in.
Exit status is the run's, so it also reads as a plain command. Reach for it
before writing something that polls `gh`; `tools/ci-watch/README.md` has the
rest.

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

**The interface is translated; nothing below it is.** User-visible strings are
marked in `app/src` with `_()`, `wxPLURAL()` or `wxTRANSLATE()`, compiled from
`app/locale/*.po` into the binary by `cmake/CompileCatalog.cmake`, and installed
by `app/src/Localization.cpp` before the first window. There is one trap and it
is silent: `_()` converts its literal to a `wxString` *implicitly*, which on
Windows goes through the current 8-bit locale — so **a message whose English is
not pure ASCII must use `trUtf8()`** (see `app/src/Text.hpp`). Regenerating the
template with `python tools/extract-messages.py` refuses to run when that rule is
broken. `core`, `codecs` and `platform` have no catalogue and never will; the few
strings of theirs a listener reads are mapped in the app layer, which is what
`PlaylistView::heading()`'s comment is about. `app/locale/README.md` covers
adding a language and what is deliberately left untranslated.

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
UI), `tools/cli/`, `tests/`, `assets/`, `packaging/windows/`,
`packaging/macos/`, `packaging/linux/`.

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
