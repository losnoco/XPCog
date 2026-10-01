# XPCog

[![CI](https://github.com/losnoco/XPCog/actions/workflows/ci.yml/badge.svg)](https://github.com/losnoco/XPCog/actions/workflows/ci.yml)

XPCog is an audio player for **Windows and Linux**, one engine under two native
interfaces: WinUI 3 on Windows, GTK 4 and libadwaita on Linux. It plays **842 extensions across 23 decoders**, and that list runs a
long way past the usual lossless and lossy files.

**The engine.** Gapless across formats *and* sample rates, ReplayGain, cue sheets and
HDCD. A 31-band equaliser, transport fades, matrix downmix and upmix, FreeSurround
stereo-to-5.1, and time-stretching that moves pitch and tempo independently.

**The window.** A playlist, transport and seek bar, file browser, preferences, undo and
drag-and-drop, over a persistent SQLite library. A spectrum analyser, an oscilloscope
and a mini player.
English and Spanish.

**The desktop.** Media keys and Now Playing on both platforms — SMTC and MPRIS — a tray
icon and track notifications, a taskbar badge and progress bar on Windows, and one
instance per user.

**The awkward formats.** Archives played without unpacking first, tracker modules, game
rips through vgmstream, the whole PSF family on all eight of its emulator cores,
Commodore 64 tunes, Musepack, Monkey's Audio Link files, and MIDI rendered on a SoundFont
bank — one ships with it — a Sound Blaster's OPL3, or an emulated Roland SC-55 with its
front panel.

**Over the network.** Internet radio, with SHOUTcast stream titles arriving in the window
as the station announces them, HLS for the stations that use it, and chained Ogg so a
stream survives its own track changes. Last.fm scrobbling, with a queue that survives an
evening offline. A [remote control](docs/FEATURES.md#remote-control) over HTTP — off until you switch it
on — with a generated OpenAPI document and a browser page for trying it.

**Building it.** On Windows every dependency comes from vcpkg, so there is nothing to
install separately and no deploy step; on Linux the toolkit is the distribution's and
vcpkg fills in the rest.

XPCog grew out of a port of [Cog](https://cog.losno.co/), the macOS player by Vincent
Spader and Christopher Snowhill, and owes it a great deal: the plugin contract the
design hangs off, the settings keys, and a lot of carefully chosen behaviour. It runs
where Cog does not — Cog remains the player on the Mac — and has since gained things
Cog does not have. Where following
Cog is still the right answer it follows Cog; where it is not, the difference is
written down — see [Relationship to Cog](#relationship-to-cog).

## Installing

The [latest release](https://github.com/losnoco/XPCog/releases/latest) carries
the first two; the rest build on your machine. There is no macOS build: on a Mac,
use [Cog](https://cog.losno.co/).

- **Windows** — `XPCog-<version>-x64-setup.exe`, for **Windows 10 version 1809
  or newer, 64-bit**. It downloads Microsoft's Windows App Runtime, which the
  player runs on, only if the machine does not have it yet. Unsigned, so
  SmartScreen will say so. Silent switches and what it registers:
  [Windows: the installer](docs/BUILDING.md#windows-the-installer).
- **Linux** — `XPCog-<version>-x86_64.tar.gz`, for **GTK 4.22 and libadwaita 1.9
  or newer** (Ubuntu 26.04, Fedora 44, Arch, anything on GNOME 50 or later):

  ```sh
  sudo tar xzf XPCog-<version>-x86_64.tar.gz --strip-components=1 -C /usr/local
  ```

  GTK, libadwaita and libsecret come from your distribution; the codecs are linked in. Unpack
  at the prefix it was built for, or edit the desktop file's `Exec` line.
- **Arch** — [`xpcog`](https://aur.archlinux.org/packages/xpcog) on the AUR:
  `yay -S xpcog`. It reaches the network during `build()`, because vcpkg fetches
  what has no system path.
- **Flatpak** — runs on any distribution, whatever GTK it ships. Not on Flathub, for
  the same vcpkg reason (`packaging/flatpak/README.md`):

  ```sh
  flatpak run org.flatpak.Builder --force-clean --user --install \
      build-dir packaging/flatpak/co.losno.XPCog.yml
  ```

- **From source** — see [Building](#building).

| You are on | Take |
| --- | --- |
| Windows | the installer |
| macOS | [Cog](https://cog.losno.co/) |
| Arch | `yay -S xpcog` |
| Ubuntu 26.04, Fedora 44, or newer | the tarball |
| Something older, or you want the sandbox | the Flatpak |
| Ubuntu 24.04, Debian 13 | the Flatpak — the tarball's GTK floor rules you out |

Anything you build yourself has **scrobbling off unless you supply your own
Last.fm credentials**; the prebuilt downloads carry one. See
[Last.fm credentials](docs/BUILDING.md#lastfm-credentials).

## Building

**CMake 3.24+**, **Ninja**, a **C++20** compiler, and
[**vcpkg**](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set. On Windows
every dependency comes from vcpkg and there is no deploy step. On Linux GTK 4.22,
libadwaita 1.9 and blueprint-compiler are the distribution's — `libgtk-4-dev
libadwaita-1-dev blueprint-compiler`, or `gtk4-devel libadwaita-devel`, or `gtk4
libadwaita` — and the `linux-repo-*` presets take as much of the rest from it as
the machine has.

```sh
cmake --preset linux-repo-debug              # or linux-debug / windows-debug
cmake --build --preset linux-repo-debug
ctest --preset linux-repo-debug
```

Presets come in `-debug` and `-release`; `*-app-*` builds the application alone,
`linux-headless` builds no toolkit at all. Packaging is a target of its own:
`installer` on Windows, `package` on Linux.

Read the **skip count** of a test run, not just the pass rate: many tests build
their fixtures with `flac`, `oggenc`, `opusenc`, `lame`, `wavpack` and `ffmpeg`,
and skip silently without them.

Everything else — the installer, the Linux
install tree, how a release is made, building against a distribution's
libraries, the encoders and corpora the tests want — is in
[`docs/BUILDING.md`](docs/BUILDING.md).

## What it does

Each of the following has a longer entry in [`docs/FEATURES.md`](docs/FEATURES.md).

**Formats.** Dedicated decoders for FLAC, Ogg Vorbis, Opus, MP3, WavPack and
Musepack, with FFmpeg as the catch-all for AAC, ALAC, WMA, AC3, DTS, TAK, TTA,
APE and the MP4/MKV/ASF containers. Beyond those: tracker modules, chiptune rips,
console streamed audio, the PSF family on all eight of its emulator cores,
Commodore 64 tunes, and MIDI rendered on a SoundFont bank (one ships with it), an
emulated OPL3, or a Roland SC-55 if you have the ROMs — see
[`docs/MIDI.md`](docs/MIDI.md). Archives play without being unpacked, and a cue
sheet or a Monkey's Audio Link is a range within one file. Selection is by
extension, then MIME type, with FFmpeg deliberately last.

**Artwork.** Embedded pictures, and for a track that has none, the folder's
`cover.jpg` or `folder.png` — the usual names, any case, embedded art winning
where both exist.

**Gapless and HDCD.** The next track opens while the last is still playing and
writes into the same ring, across formats and across sample rates. HDCD is
decoded when present and is bit-transparent when it is not; both are asserted by
tests against a capturing output, not assumed.

**Internet radio.** SHOUTcast stream titles, HLS, and chained Ogg. The audio
callback takes no lock and allocates nothing, and `xpcog-cli play` reports
underruns.

**Last.fm and ListenBrainz.** Desktop authentication in a browser for Last.fm —
XPCog never sees the password — and a pasted user token for ListenBrainz, or a
server of your own that speaks its API. Credentials live in the platform's secret
store, and each service has a durable queue, so an evening offline arrives the
next day.

**Lyrics.** The Lyrics pane shows what the file carries, and can ask LRCLIB for
the words of a track that carries none — off by default, exact-match only, and
every answer kept in the library so a track is asked about once. Timed LRC
lyrics — in the tag, in a `.lrc` beside the file, or from LRCLIB — are followed
line by line while the track plays.

**Remote control.** A REST API over the transport, playlist, equaliser, settings
and cover art, with a generated OpenAPI document and a Swagger page. Off until
Preferences → Remote turns it on, loopback by default, a bearer token on every
request, no TLS. [`docs/REST.md`](docs/REST.md) is the reference; `xpcog-cli
serve` runs the same API with no toolkit.

**Visualisers.** A spectrum analyser and an oscilloscope, both docked panes fed
from the same tap before the volume, both able to show the mix, one side, or
both sides at once, both configured on Preferences → Visualizers; the choice of
channels is on each pane's right-click menu too, with the oscilloscope's
trigger, fill and scale and the spectrum's peak markers. View → Show Waveform draws the playing track's shape in
the seek bar, analysed once in the background and cached; Preferences →
Appearance has its height, colours and drawing style.

**Crash reporting.** Off unless you tick it on the first launch, and nothing is
initialised until you do. [What is collected](https://www.iubenda.com/privacy-policy/59859310).

**Languages.** English and Spanish, following the system; Preferences → General
has the picker. Adding a language is one `.po` — [`app/locale/README.md`](app/locale/README.md).

## Design

One narrow contract, inherited from Cog's `Audio/Plugin.h`: a *source* opens a
URL, a *decoder* turns bytes into PCM, a *container* expands one URL into several,
a *metadata reader* answers with tags, and a *source wrapper* sits between a
source and a decoder when the bytes are not what the decoder wants. Adding a
format is one of those plus one `xpcog_add_codec()` call, never a refactor.

```
xpcog-winui (WinUI 3, Windows) ──┐
                                 ├── xpcog-uicore ──┬── xpcog-platform (per-OS; NO toolkit)
xpcog-gtk (GTK4, Linux) ─────────┘                  └── xpcog-codecs ──┬── xpcog-core (NO toolkit)
xpcog-cli ── core + codecs ────────────────────────────────────────────┘
```

Two rules do the structural work. **Only the two frontends link a UI toolkit**,
each its own, which `xpcog-cli` proves by linking none and
`cmake/CheckNoToolkit.cmake` reports earlier — the interface moved from Qt 6 to
wxWidgets, gained a second written for GTK, and then moved on Windows from
wxWidgets to WinUI 3, without `core/` or `codecs/` changing
([`docs/WXPORT.md`](docs/WXPORT.md), [`docs/GTKPORT.md`](docs/GTKPORT.md),
[`docs/WINUIPORT.md`](docs/WINUIPORT.md)). **Codecs register at compile time**,
through a generated `RegisterAll.cpp`, because a self-registering static inside
a static library is silently dropped by the linker.

## Status

What is outstanding is short, and each item is here for a reason rather than for want
of time:

- **Importing a Cog library** was removed in 2.0.0, with the macOS port: it
  existed for moving from a Mac, which XPCog no longer runs on. Playlists Cog
  saved as XML still open.
- **DoP output** waits on a DAC to verify it against, and **HRTF** is deferred.
- **Global hotkeys** are not coming: the media keys they would bind are already
  delivered by SMTC and MPRIS.
- **macOS** was dropped in 2.0.0; Cog is the player there.

### Deliberately out of scope

The Mac App Store sandbox (`SandboxBroker`, security-scoped bookmarks), AudioUnit MIDI
instrument hosting, AppleScript and Spotlight integration are macOS-only, and nothing
here reimplements them. A no-op `IFileAccess` seam preserves the sandbox call sites in
case that changes.

That is a record of what did not travel, not a boundary on what XPCog may do. Cog not
having something is a fact about Cog: it means the work would be new rather than
inherited, and is judged on its own terms. The remote control was the first feature
to land that way; the oscilloscope and the waveform seek bar followed.

AudioUnit hosting is one of Cog's four MIDI backends, not MIDI itself — `.mid` and
its dozen relatives play here through the other three, all of which have landed:
SpessaSynth, Nuked OPL3 and Nuked SC-55. See [`docs/MIDI.md`](docs/MIDI.md).

## Relationship to Cog

XPCog is a derivative work of [Cog](https://github.com/losnoco/Cog), and the
[License](#license) below is not a formality about that. The debt is specific and
large: the six-protocol plugin contract the whole design hangs off, the
`NSUserDefaults` keys — kept identical —
and years of decisions about how a player of this kind should behave, down to quirks
worth preserving.

It is not only a port any more. It runs on Windows and Linux, where Cog does not, it has a REST remote control and a translated interface, and its build
fetches its own dependencies. Cog recognises around 900 extensions across ~35 decoders,
against 842 across 23 here.

Where the two do the same thing, XPCog follows Cog. Where it deliberately differs, the
difference is written down rather than discovered — Cog's shuffle and next/previous
operate on the *sorted* playlist order, for instance, whereas XPCog keeps playback
order canonical and treats sorting as display-only. And where Cog simply has nothing to
say, XPCog decides for itself.

[`docs/PORTING.md`](docs/PORTING.md) is the record of that work: the original survey,
the structural decisions, the complete list of deliberate behaviour differences, and
**Where to pick up next**. It began as a plan and is now mostly a history, which is the
usual fate of a good one. Work spanning several commits gets its own document beside
it — [`docs/HIGHLYCOMPLETE.md`](docs/HIGHLYCOMPLETE.md) staged the eight emulator cores
behind the PSF formats one at a time, and is now the record of all eight.

## License

GPL-3.0-or-later. See [COPYING](COPYING).

Cog is GPL-2.0-or-later, and this was too until 1.13.0. What is built here has been
GPL-3 in effect for as long as it has shipped the Syntrax and 2SF decoders (both
GPL-3-only) and the SoundFont synthesiser and the remote control's documentation
page (both Apache-2.0, which GPL-2 cannot take): the "or later" is what made those
combinations legal, and the licence now says what the binary is. Cog's own code
stays under Cog's licence; the upgrade is the one its "or later" permits.

Cog is copyright Vincent Spader and Christopher Snowhill. Bundled decoding and tagging
libraries are under their own licenses, listed in the About dialog. Interface icons
are [Lucide](https://lucide.dev) under the ISC license — see
[`app/icons/lucide/LICENSE`](app/icons/lucide/LICENSE).
