# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

XPCog is an audio player for Windows and Linux, ported from
[Cog](https://github.com/losnoco/Cog) (macOS, Objective-C). One engine under two
native interfaces: wxWidgets on Windows (`app/`), GTK4 and libadwaita on Linux
(`app-gtk/`). C++20, CMake, vcpkg. There is no macOS build since 2.0.0 — Cog is
the player there.

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
Windows `FileVersion` and `ProductVersion`, and the Linux AppStream metainfo's
`<release>` comes from it too. The version
string is user-visible in the About dialog, in `xpcog-cli`'s banner and in
every `User-Agent` the program sends (`userAgent()`, beside it in `Version.hpp`).

`docs/BUILDING.md` is the one place that spells a version out by hand: the
Windows installer section shows `XPCog-<version>-x64-setup.exe` in two examples,
and those follow the bump.

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
cmake --preset linux-debug            # configure  (or windows-debug)
cmake --build --preset linux-debug    # build
ctest --preset linux-debug            # test
```

Preset families: `linux-*` and `windows-*`, each with `-debug` and `-release`.
`XPCOG_BUILD_APP` builds the player, and the toolkit follows the platform rather
than an option: wx on Windows, GTK on Linux (`cmake/XPCogOptions.cmake` derives
`XPCOG_BUILD_WX_APP` / `XPCOG_BUILD_GTK_APP` from it, and refuses elsewhere).
On Linux GTK 4.22, libadwaita 1.9 and blueprint-compiler come from the
distribution through pkg-config, never vcpkg. Variants on top of those:

- `*-app-debug` / `*-app-release` — application only, no CLI and no tests.
- `linux-headless` — `XPCOG_BUILD_APP=OFF`, no toolkit built at all. The fastest
  way to exercise `core` and `codecs`, and the shape a headless build takes on
  any other platform.
- `linux-repo-debug` / `linux-repo-release` — Linux, taking every dependency the
  distribution already has via pkg-config and asking vcpkg only for the rest
  (`XPCOG_USE_SYSTEM_LIBS=ON`, probed in `cmake/XPCogSystemDeps.cmake` **before**
  `project()`). Separate build trees on purpose; do not flip the cache variable
  inside a plain `linux-*` tree, which is refused rather than obeyed.

The presets turn on more than the option defaults do — FFmpeg, vgmstream, PSF,
SID, MIDI, AdPlug, libvgm, Sentry and the REST remote control are all `OFF` for a
bare `cmake` and `ON` in `base`. Configure with a preset unless you specifically want a minimal build.

Catch2 v3 throughout, registered with ctest through `catch_discover_tests`:
`xpcog-tests` (core and codecs) everywhere, `xpcog-uicore-tests` (the
toolkit-free application layer) wherever the player is built, and
`xpcog-app-tests` (the wx half, built as `xpcog-appcore`) on Windows.
`xpcog-uicore-tests` links `xpcog-uicore` and nothing else, so a widget
dependency creeping into `uicore/` fails to link rather than passing.

On Linux there is `xpcog-gtk-tests`, and it is the one that needs a screen: it
walks the preferences dialog page by page, measures the equaliser's scales,
runs the painted panes, and fails on any GTK warning or critical. It is
registered as a single `add_test()` rather than discovered, so it can be run
under `xvfb-run` where CMake found one; without a display it skips. With
`XPCOG_GUI_CAPTURE=<dir>` it renders widgets through GSK to PNGs there, which is
how a rendering question gets answered without driving the player; under Xvfb
on a Wayland desktop that needs `GDK_BACKEND=x11` and `WAYLAND_DISPLAY` unset,
or GTK opens the test windows on the real screen.

On Windows, where the presets now build the WinUI player beside the wx one,
`xpcog-winui-tests` is its counterpart: it opens the WinUI main window, every
pane the View menu shows, Preferences page by page, the mini player and the
About box, and fails on any XAML error. Also a single `add_test()`; it needs
the Windows App Runtime, skips without it, and fails instead under `CI`, where
the workflow installs the runtime first. It opens real windows on the desktop
for a few seconds.

```sh
ctest --preset linux-debug -R Gapless          # by ctest test name
./build/linux-debug/bin/xpcog-tests "[gapless]"  # by Catch2 tag — the usual way
./build/linux-debug/bin/xpcog-tests --list-tests
```

Tags follow the subsystem (`[dsp]`, `[playlist]`, `[library]`,
`[lastfm]`, `[listenbrainz]`, `[lrclib]`, `[scrobbler]`, `[timestretch]`,
`[gapless]`, `[hls]`, `[midi]`, `[remote]`…). `[remote][socket]` binds an ephemeral loopback
port and runs by default; `XPCOG_NO_SOCKET_TESTS=1` skips it loudly for an environment that
forbids listening.
Tags starting with a dot are hidden and only run when named: `[.lastfmlive]`
(hits the real Last.fm API), `[.ratedevice]` and `[.integerdevice]` (want real
hardware).

Other targets: `xpcog-no-toolkit` (layering check, runs as part of `ALL`), and
`installer` on Windows (needs NSIS; use a **release** tree).

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
`expand`, `decode`, `play`, `serve`. The last is the REST remote control with no
toolkit linked, which is the sharpest demonstration that the layering holds; see
`docs/REST.md` for what it deliberately cannot do.

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
xpcog-app (wx, Windows) ──┐
                          ├── xpcog-uicore ──┬── xpcog-platform (per-OS; NO toolkit)
xpcog-gtk (GTK4, Linux) ──┘                  └── xpcog-codecs ──┬── xpcog-core (NO toolkit)
xpcog-cli ── core + codecs ─────────────────────────────────────┘
```

**Only the two frontends link a UI toolkit, and each links its own.** `core`,
`codecs`, `uicore` and `platform`'s *public headers* name no toolkit at all —
`platform`'s implementations talk to Win32, C++/WinRT, GDBus and libsecret,
but nothing they do may leak into a header the app includes. `app/` (wxWidgets,
Windows) includes no GTK or GLib and `app-gtk/` (GTK4/libadwaita, Linux)
includes no wx; what both need lives in `uicore/` or `platform/`. This is
enforced by `cmake/CheckNoToolkit.cmake` (which also fails on any Qt include
anywhere), and again by `xpcog-cli` linking no toolkit and by every Linux build
having no wx at all, so a leak breaks a target. Keep it that way; it is the
rule the Qt→wxWidgets move was a test of, and `docs/GTKPORT.md` is the second
frontend's record.

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

**The remote control is a seam, not a layer.** `core/src/remote/` holds an HTTP
server, a route table that also generates the OpenAPI document, and `CallGate`,
which is how a socket thread gets an answer out of the interface thread. It calls
`IPlayerControl` (`core/include/xpcog/core/remote/PlayerControl.hpp`), which
`app/` implements over `PlaybackController` and `AppCommands` and `xpcog-cli`
implements over an `AudioEngine`. Behind `XPCOG_WITH_REST`, off at run time as
well, and `docs/REST.md` explains why both.

**Settings are an X-macro.** `core/include/xpcog/core/settings.def` is the single
source of truth — `XPCOG_SETTING(Ident, Type, "cogKey", default)` — included
several times with the macro defined differently. Keys are deliberately identical
to Cog's `NSUserDefaults` keys -- inherited, and kept because renaming one
orphans the value in every existing settings file. Add a
setting there, not in `Settings.hpp`.

**The interface is translated; nothing below it is.** User-visible strings are
marked in `app/src` with `_()`, `wxPLURAL()` or `wxTRANSLATE()` and in `uicore/src`
with the toolkit-free `tr()`, `trn()`, `trf()` and `XPCOG_TRANSLATE()` (see
`uicore/src/Translations.hpp`, and note it needs no `trUtf8()` twin because
nothing there goes near a `wxString`), compiled from
`uicore/locale/*.po` into the binary by `cmake/CompileCatalog.cmake`, and installed
by `app/src/Localization.cpp` before the first window. There is one trap and it
is silent: `_()` converts its literal to a `wxString` *implicitly*, which on
Windows goes through the current 8-bit locale — so **a message whose English is
not pure ASCII must use `trUtf8()`** (see `app/src/Text.hpp`). Regenerating the
template with `python tools/extract-messages.py` refuses to run when that rule is
broken. `core`, `codecs` and `platform` have no catalogue and never will; the few
strings of theirs a listener reads are mapped in the app layer, which is what
`PlaylistView::heading()`'s comment is about. `uicore/locale/README.md` covers
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
`platform/` (per-OS integration behind toolkit-free headers), `uicore/` (the
application layer that links no toolkit -- playback controller, command tables,
playlist edits, the remote control's player, the translation lookup, the
catalogues), `app/` (the Windows player, wxWidgets), `app-gtk/` (the Linux
player, GTK4/libadwaita, its interface in Blueprint under `app-gtk/ui/`),
`tools/cli/`, `tests/`, `assets/`, `packaging/windows/`, `packaging/linux/`,
`packaging/arch/`, `packaging/flatpak/`.

`uicore/` keeps the namespace `xpcog::app`: it names the layer, not the library.
The split is enforced by `cmake/CheckNoToolkit.cmake`, not by a name.

**`vendor/` vs `ports/`**: `ports/` holds vcpkg overlay ports for dependencies
with a real upstream release or pinned commit (vgmstream, libsidplayfp, mGBA,
libvgm, AdPlug, rubberband, spessasynth-core…) — preferred, because CI compiles
them once and restores from the binary cache. `vendor/` is for sources with no
upstream to point at, mostly the emulator cores behind the PSF family and
Cog's own small libraries. See `ports/README.md`.

## Docs

`docs/BUILDING.md` is the README's build and packaging material at full length:
the disk image and its signing, the installer, the Linux install tree and
tarball, how a release is made, the system-libraries presets, the encoders and
corpora the tests want, and Last.fm credentials. `docs/FEATURES.md` is the same
for what the player does: formats, cue sheets, gapless, HDCD, the real-time
callback, crash reporting, Last.fm, the remote control, languages. The README
keeps a paragraph on each and points here; put detail in these, not there.

`docs/PORTING.md` is the long one: the survey, the structural decisions, progress,
the **deliberate differences from Cog**, the verification strategy, known gaps,
and a *Where to pick up next* section at the end. Consult it before changing
behaviour that mirrors Cog — differences are meant to be documented, not
accidental. `docs/MIDI.md` covers the three MIDI backends, `docs/HIGHLYCOMPLETE.md`
the eight PSF emulator cores,
`docs/WXPORT.md` the Qt→wxWidgets move, and `docs/REST.md` the remote control.

Not ported, and macOS-only: the Mac App Store sandbox, AudioUnit MIDI instrument
hosting, AppleScript, Spotlight. That list records what did not travel; it is not
a boundary on what XPCog may do. Cog not having a thing is not an argument against
building it — it means the result is new work rather than port work, judged on its
own. `docs/REST.md` is the first feature to land on those terms.
