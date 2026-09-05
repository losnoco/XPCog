# XPCog

[![CI](https://github.com/losnoco/XPCog/actions/workflows/ci.yml/badge.svg)](https://github.com/losnoco/XPCog/actions/workflows/ci.yml)

XPCog is an audio player for **Windows, macOS and Linux**, built on wxWidgets from a
single codebase. It plays **842 extensions across 23 decoders**, and that list runs a
long way past the usual lossless and lossy files.

**The engine.** Gapless across formats *and* sample rates, ReplayGain, cue sheets and
HDCD. A 31-band equaliser, transport fades, matrix downmix and upmix, FreeSurround
stereo-to-5.1, and time-stretching that moves pitch and tempo independently.

**The window.** A playlist, transport and seek bar, file browser, preferences, undo and
drag-and-drop, over a persistent SQLite library. A spectrum analyser and a mini player.
English and Spanish.

**The desktop.** Media keys and Now Playing on all three platforms — MediaPlayer.framework,
SMTC and MPRIS — with a tray icon on Windows and Linux, the Dock menu on macOS, a taskbar
badge and progress bar, and one instance per user.

**The awkward formats.** Archives played without unpacking first, tracker modules, game
rips through vgmstream, the whole PSF family on all eight of its emulator cores,
Commodore 64 tunes, Musepack, Monkey's Audio Link files, and MIDI rendered on a SoundFont
bank — one ships with it — a Sound Blaster's OPL3, or an emulated Roland SC-55 with its
front panel.

**Over the network.** Internet radio, with SHOUTcast stream titles arriving in the window
as the station announces them, HLS for the stations that use it, and chained Ogg so a
stream survives its own track changes. Last.fm scrobbling, with a queue that survives an
evening offline. A [remote control](#remote-control) over HTTP — off until you switch it
on — with a generated OpenAPI document and a browser page for trying it.

**Building it.** Every dependency comes from vcpkg, so there is nothing to install
separately, no environment variable pointing at a toolkit, and no deploy step.

XPCog grew out of a port of [Cog](https://cog.losno.co/), the macOS player by Vincent
Spader and Christopher Snowhill, and owes it a great deal: the plugin contract the
design hangs off, the settings keys, and a lot of carefully chosen behaviour. It runs
on three platforms now, and has since gained things Cog does not have. Where following
Cog is still the right answer it follows Cog; where it is not, the difference is
written down — see [Relationship to Cog](#relationship-to-cog).

## Installing

Six ways, and the [latest release](https://github.com/losnoco/XPCog/releases/latest)
carries the first three.

### Prebuilt

- **Windows** — `XPCog-<version>-x64-setup.exe`. **Unsigned**, so SmartScreen will
  say so. See [Windows: the installer](#windows-the-installer) for the silent
  switches and what the installer registers.
- **macOS** — `XPCog-<version>-arm64.dmg`. Signed with a Developer ID and
  notarised, so Gatekeeper opens it without argument. **Apple silicon only** — not
  a universal binary, so an Intel Mac cannot run it. See
  [macOS: the disk image](#macos-the-disk-image).
- **Linux** — `XPCog-<version>-x86_64.tar.gz`:

  ```sh
  sudo tar xzf XPCog-<version>-x86_64.tar.gz --strip-components=1 -C /usr/local
  ```

  **Needs glibc 2.39 or newer** — Ubuntu 24.04, Debian 13, current Fedora, Arch
  and openSUSE are above that line; Ubuntu 22.04, Debian 12 and RHEL 9 are below
  it. wxWidgets and GTK come from your distribution and the codecs are linked in.
  `bin/`, `lib/` and `share/` relocate happily so long as they move together, but
  the desktop file's `Exec` is an absolute path — unpack at the prefix you mean to
  run from, or edit that one line. See [Linux: installing](#linux-installing).

### Built on your machine

- **Arch** — [`xpcog`](https://aur.archlinux.org/packages/xpcog) on the AUR:

  ```sh
  yay -S xpcog          # or paru, or a clone and `makepkg -si`
  ```

  Built against Arch's own libraries. It **reaches the network during `build()`**,
  because vcpkg fetches what has no system path, so it will not work in a
  network-isolated clean chroot. `packaging/arch/README.md` has the rest.

- **Flatpak** — runs on any distribution whatever its glibc, which is the one
  thing the tarball cannot do:

  ```sh
  flatpak run org.flatpak.Builder --force-clean --user --install \
      build-dir packaging/flatpak/co.losno.XPCog.yml
  ```

  **Not on Flathub**, for the same vcpkg reason: the manifest needs
  `--share=network`. `packaging/flatpak/README.md` lists what would remove it.

- **From source** — the developer path, and the one [Building](#building)
  documents at length:

  ```sh
  cmake --preset linux-repo-release       # or macos-release / windows-release
  cmake --build --preset linux-repo-release
  sudo cmake --install build/linux-repo-release --prefix /usr/local
  ```

### Which one

| You are on | Take |
| --- | --- |
| Windows or macOS | the installer or the disk image |
| Arch | `yay -S xpcog` |
| A current mainstream distribution | the tarball |
| Something older, or you want the sandbox | the Flatpak |
| Ubuntu 22.04, Debian 12, RHEL 9 | Flatpak or source — the tarball's glibc floor rules you out |

One thing applies to everything you build yourself: **scrobbling is off unless you
supply your own Last.fm credentials.** The key identifies the *application* to
Last.fm and cannot be published, so a build without it compiles the scrobbling
code and reports the feature as unavailable — the prebuilt downloads carry one,
and [Last.fm](#lastfm) covers applying for your own.

## Design

The player is arranged around one narrow contract, and everything else is built
outward from it. A *source* opens a URL, a *decoder* turns its bytes into PCM, a
*container* expands one URL into the several it holds — a cue sheet, a playlist, an
archive — a *metadata reader* answers with tags, and a *source wrapper* slips between
a source and a decoder when a file's bytes are not the bytes the decoder wants. Each
is a small abstract base class with a descriptor beside it; adding a format means
adding one of those, never a refactor.

The rest follows from keeping that contract clean: a lock-free audio engine, a SQLite
library, a DSP chain and an interface, all in portable C++ with no platform
assumptions baked in below the top layer.

That contract is inherited rather than invented. Cog's `Audio/Plugin.h` is a
six-protocol interface that maps almost one-to-one onto C++ abstract base classes,
and the bulk of the format support beneath it is portable C and C++ libraries in
both players. The rest of Cog does not travel: its UI is 12 Cocoa XIBs driven by 190
bindings, persistence is Core Data, audio output is AUHAL, every DSP kernel is
Accelerate/vDSP, and decoders are Objective-C bundles discovered at runtime. None of
that survives a move off Apple platforms, so all of it is built again here. That is why
none of Cog's source is in this tree: what was inherited is a design, not an
implementation.

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
separately and no environment variable to point at a toolkit. On Linux the
toolkit is the distribution's, and the `linux-repo-*` presets take as much of the
rest from it as the machine can supply; both are below.

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
triplet is static and there is nothing to copy. The one thing the build stages
itself is `crashpad_handler`, which is a second executable rather than a library
and so is not something applocal knows about; it lands beside the binary as a
post-build step. What remains is signing and packaging, which are targets of
their own on both platforms — a disk image on macOS, an installer on Windows,
both below.

wxWidgets is declared under a `gui` feature rather than as a plain dependency, so
a headless configuration (`-D XPCOG_BUILD_APP=OFF`) builds no toolkit at all.

**On Linux the toolkit comes from the distribution**, not from vcpkg — install
`libwxgtk3.2-dev` (Debian/Ubuntu), `wxGTK-devel` (Fedora) or `wxgtk3` (Arch).
vcpkg's `wxwidgets` port depends on its `gtk3` port, so asking vcpkg for wx there
builds 57 packages from source — wx, GTK and 55 more beneath them: glib, pango,
cairo, harfbuzz, fontconfig, at-spi2, dbus, seven X11 libraries — on a machine
that already has all of them. 98 packages for the Linux build against 41
without. The Linux presets therefore leave `gui` out, and
`cmake/XPCogWx.cmake` finds the system wx through CMake's `FindwxWidgets`. It
needs **wxWidgets 3.2 or newer** — the oldest release carrying `wxTaskBarIcon` and
`wxNotificationMessage` in `core` rather than in the since-merged `adv` library.
To build the toolkit through vcpkg anyway, add the feature back:
`cmake --preset linux-debug -D "VCPKG_MANIFEST_FEATURES=gui;sentry;ffmpeg;vgmstream;mgba;psf-cores;sid;musepack;adplug;libvgm"`.

### macOS: the disk image

Three targets in `packaging/macos/`, none of them built by `all` and each asked
for by name after a build:

```sh
cmake --build build/macos-release --target sign      # signs XPCog.app in place
cmake --build build/macos-release --target dmg       # signs, then packages
cmake --build build/macos-release --target notarize  # submits, waits, staples
```

`dmg` produces `build/macos-release/XPCog-<version>-arm64.dmg`: the bundle, a
symlink to `/Applications`, drag to install. It is compressed, read-only and
plain — no background picture and no scripted Finder window, which would mean
creating a writable image, mounting it, driving the Finder to arrange the icons
and converting the result, on a machine with a logged-in window server. That is
the most fragile machinery in a macOS release and it buys a prettier window.

**The bundle declares what it opens**, so the Finder's *Open With* menu, the
Dock icon and a double-click on a game-music file all reach XPCog. The
declarations are not written by hand: `xpcog-doctypes` (`tools/doctypes/`) runs
after every build, reads the codec registry, and writes `CFBundleDocumentTypes`
and `UTImportedTypeDeclarations` for every extension this build decodes; a
splice step puts them into the bundle's `Info.plist` between two markers
`app/Info.plist.in` carries, and `plutil` checks the result. Every entry ranks
XPCog as an *alternate* handler, which is the plist's way of saying what
`--register` says on Windows: XPCog is offered, and takes nothing from whatever
opened these files before. Where XPCog is the only application to declare a
type at all — most of the game-music formats — that still makes it the one a
double-click opens. To make it the default for a type it is not alone in, use
Get Info → *Open with* → *Change All…*, which is the user's step, as it is on
Windows. A handful of vgmstream's extensions are left out on purpose, because
macOS already knows them as something that is not audio (`.m` is Objective-C
source, `.svg` is an image); the build log names them.

**Signing is inside out**, and the order is the substance of it. A nested
signature is part of the bytes the enclosing one covers, so `libvgmstream.dylib`
and `crashpad_handler` are signed first, then the bundle, then the image. This
replaced a single `codesign --deep`, which Apple's own guidance calls unsuitable
for nested code: it applies the outer bundle's identity and entitlements to
everything it finds and silently skips whatever it does not recognise as code.

There is **no entitlements file**, and that is the intended state. An entitlement
is a hole in the hardened runtime, and this program wants none: not sandboxed, so
no file-access entitlement; records nothing, so no microphone; nothing JITs; and
every library in the bundle carries the same signature, so library validation has
nothing to disable. `packaging/macos/SignBundle.cmake` names the three that would
arrive if any of that changed.

Set the identity in the cache or, better, in the environment:

```sh
export XPCOG_CODESIGN_IDENTITY="Developer ID Application: NAME (TEAMID)"
```

`security find-identity -v -p codesigning` lists what the machine holds. Without
one, `dmg` packages an unsigned image and says so; `sign` refuses, because
someone who typed that target name and got a zero exit status has been told
nothing.

**Notarisation** takes an App Store Connect API key, from the environment only —
no cache variable and no `-D`, because a `.p8` is a private key that can notarise
anything under the team's name and both of those land in files that get committed
by accident:

```sh
export XPCOG_NOTARY_KEY=~/keys/AuthKey_XXXXXXXXXX.p8
export XPCOG_NOTARY_KEY_ID=XXXXXXXXXX
export XPCOG_NOTARY_ISSUER_ID=00000000-0000-0000-0000-000000000000
cmake --build build/macos-release --target notarize
```

An API key rather than an Apple ID and an app-specific password, which
`notarytool` also accepts: the password form ties releases to one person's Apple
ID and unlocks a great deal more than notarisation, while a notary key does one
thing and revokes without anyone losing anything else. The target waits for
Apple's answer, prints Apple's log on a rejection rather than leaving you to fetch
it by submission id, staples the ticket to the image and finishes by asking
`spctl` the question the user's Mac will ask. Stapling is what makes the ticket
travel with the download: without it Gatekeeper asks Apple over the network at
first launch and fails closed on a machine that is offline.

**CI builds one on every run**, and notarises on `main` and on tags. The `macOS
disk image` job configures `macos-app-release`, imports the certificate into a
keychain that exists for the length of that job, packages, and attaches
`XPCog-<version>-arm64.dmg` to the run — so a pull request that breaks the
bundle says so where it broke. Notarisation is drawn at a different line than
packaging because the costs differ in kind: packaging is a minute of the runner's
own time, notarisation is minutes of waiting on a service this project does not
control, answering a question that only matters for an image somebody downloads.
A pull request from a fork sees no secrets and produces an unsigned image, which
is the correct outcome — packaging is what it is testing.

Five repository secrets, under Settings → Secrets and variables → Actions:

| Secret | What it is |
| --- | --- |
| `MACOS_CERTIFICATE_P12` | the Developer ID Application certificate *and its private key*, exported from Keychain Access as `.p12`, then `base64 -i cert.p12 \| pbcopy` |
| `MACOS_CERTIFICATE_PASSWORD` | the password given to that export |
| `MACOS_NOTARY_KEY_P8` | the App Store Connect `.p8`, base64 the same way |
| `MACOS_NOTARY_KEY_ID` | the key's ten-character id |
| `MACOS_NOTARY_ISSUER_ID` | the issuer UUID shown above the key list |

The signing identity is *not* a sixth secret: the job reads it out of the
imported certificate, which is one less thing to keep in step and makes "that
`.p12` holds no Developer ID Application certificate" a message at import time
rather than a `codesign` error twenty minutes later. It is not secret in any
case — the team name and id are in every signature XPCog ships.

**arm64 only — not a universal binary.** The runner builds natively and an Intel
Mac cannot run the result. Configure says so in as many words, because `arm64` in
a filename is a fact about the build and "will not launch" is what that fact means
to somebody on an Intel Mac, and nothing else in the chain draws the distinction:
`codesign`, `hdiutil` and `notarytool` are all perfectly happy with a
single-architecture image. A universal one means a second vcpkg tree, a second
build and a `lipo` pass over the executable and `libvgmstream`, which is not what
this does today.

### Windows: the installer

[**NSIS**](https://nsis.sourceforge.io) 3.x builds one. It is found automatically
if it is installed; without it the target simply does not exist, because NSIS is
needed to package XPCog and not to build or run it.

[**negrutiu's fork**](https://github.com/negrutiu/nsis) is preferred when both
are present — it installs alongside the official one, under `NSIS_FORK`, and is
the only NSIS that emits a *64-bit* installer. Configuring says which it found
and which kind of installer that makes; the choice is measured by compiling a
two-line script, not by reading a version. An official NSIS builds a 32-bit
installer of the same 64-bit XPCog, which is what almost every Windows
application ships and is entirely fine — the difference is that a native
installer is on the same side of WoW64 as the program it installs, and cannot
have its registry writes redirected out from under it. A build tree configured
before the fork was installed keeps the compiler it found: `cmake -S . -B
build\windows-release -U XPCOG_MAKENSIS` makes it look again.

```bat
cmake --build build\windows-release --target installer
:: -> build\windows-release\XPCog-1.7.1-x64-setup.exe
```

Use a **release** tree. A Debug build links the debug CRT and the debug wx DLLs,
neither of which may be redistributed, and the resulting installer fails on any
machine without Visual Studio — as a missing-DLL dialog, long after the point
where it could have been explained. Configuring says so.

What it ships is whatever the build staged: `packaging/windows/Harvest.cmake`
reads the output directory and takes `XPCog.exe`, `crashpad_handler.exe`, every
DLL applocal put there, and the SoundFont and equaliser assets. Nothing lists
DLLs by hand, so the installer cannot fall behind `vcpkg.json`. The CLI, the test
binaries and the `.pdb`s are left out, and the build log names what it skipped.

The installer offers **per-machine or per-user**, writes a Start menu shortcut and
an Add/Remove Programs entry, and — as a component the user can untick — runs
XPCog's own `--register` to add it to the *Open with* lists for every format this
build understands. The uninstaller reverses all of it and leaves settings and the
library database alone. For unattended use:

```bat
XPCog-1.7.1-x64-setup.exe /S /CurrentUser /NOASSOC /D=C:\Somewhere\XPCog
```

`/NOASSOC` exists because a component page is a question and `/S` is the mode
with nobody there to answer it; without it, pushing XPCog to a fleet would
rearrange every machine's file associations on a default chosen for someone
clicking through a wizard.

**CI builds one on every run.** The `Windows installer` job installs the fork
through [`negrutiu/nsis-install`](https://github.com/negrutiu/nsis-install) at a
pinned release, configures `windows-app-release`, packages it, and attaches
`XPCog-<version>-x64-setup.exe` to the run as an artifact — so a pull request that breaks the packaging says so
where it broke rather than at release time. It is unsigned, as a locally built
one is. Its Last.fm credentials come from repository secrets; see
[Last.fm](#lastfm) below.

### Linux: installing

Linux has no installer and no disk image. The artefact is the install tree
itself, so `cmake --install` is the whole of it:

```sh
cmake --preset linux-repo-release
cmake --build --preset linux-repo-release
cmake --install build/linux-repo-release --prefix /usr/local
```

What lands is an ordinary FHS layout — `bin/XPCog` and `bin/xpcog-cli`, the
shipped SoundFont and equaliser presets under `share/xpcog/`, and the desktop
integration under `share/applications`, `share/metainfo` and
`share/icons/hicolor`. `DESTDIR=... cmake --install` stages it for a package
builder in the usual way.

The relative layout is load-bearing rather than conventional.
`core/include/xpcog/core/AssetPath.hpp` resolves the shipped assets as
`<exe dir>/../share/xpcog`, which is the same arrangement the build tree
already uses — so an installed player exercises the lookup that has been
exercised all along, and `bin/` and `share/` have to keep their relationship to
each other whatever the prefix is.

One shared library is installed beside the player: `lib/libvgmstream.so`, found
through an `$ORIGIN/../lib` rpath. It is the only dependency that is neither
the distribution's nor statically linked; `cmake/XPCogInstallRuntime.cmake` says
why the vgmstream port leaves no choice.

**Two caches are not updated, deliberately.** A package manager has a trigger
for both, and running them from an install rule would write outside `DESTDIR`,
which is what breaks packaging. After installing by hand:

```sh
update-desktop-database /usr/local/share/applications
gtk-update-icon-cache /usr/local/share/icons/hicolor
```

**The application ID is `co.losno.XPCog`**, set once as `XPCOG_DESKTOP_ID` in
`CMakeLists.txt`. Four things have to agree on it or the desktop quietly does
nothing: the `.desktop` file's basename, the AppStream `<id>`, the installed
icon's filename, and the `DesktopEntry` property MPRIS publishes — which is how
a panel gets from the transport it is showing to XPCog's icon and name. The
macOS bundle identifier beside it is the lowercase `co.losno.xpcog` and stays
that way; it is a different platform's namespace, and on macOS it is also where
a user's preferences are keyed.

The two generated files are worth validating after changing either template,
because nothing at run time reads them and a mistake is silent:

```sh
desktop-file-validate build/linux-repo-release/packaging/linux/co.losno.XPCog.desktop
appstreamcli validate build/linux-repo-release/packaging/linux/co.losno.XPCog.metainfo.xml
```

The `MimeType=` list is assembled from the enabled codec options rather than
written out once, so a build without `XPCOG_WITH_MUSEPACK` does not offer XPCog
for a Musepack file it cannot play. See `packaging/linux/CMakeLists.txt`.

**The tarball.** `cmake --build build/linux-repo-release --target package`
produces `XPCog-<version>-<arch>.tar.gz` beside the build, which is the Linux
counterpart of `installer` on Windows and `dmg` on macOS. It is CPack's `TGZ`
generator over the install rules above, stripped — 120 MB of executable becomes
24, and the symbols stay in the build tree where a debugger and a crash report
want them.

```sh
cmake --build build/linux-repo-release --target package
tar tzf build/linux-repo-release/XPCog-1.6.0-x86_64.tar.gz
```

**It is not an AppImage and does not pretend to be.** wxWidgets, GTK and most of
the codec libraries are the distribution's, linked dynamically, so the tarball
runs on the distribution release that built it or a compatible newer one. That
is a convenience for that case, not a portable binary for any Linux; the
portable answer is the Flatpak below.

The tree inside is relocatable in the two ways that took arranging — the player
finds its assets through `<exe dir>/../share/xpcog` and `libvgmstream.so`
through an `$ORIGIN/../lib` rpath, so `bin/`, `lib/` and `share/` can sit
anywhere as long as they sit together. **One file in it is not:** the desktop
file's `Exec` is the absolute path the tree was *configured* for, so unpack the
archive at that prefix, or edit the one line. Everything else works from
anywhere.

**CI builds one on every run.** The `Linux tarball` job configures
`linux-release` on `ubuntu-24.04`, packages it, and attaches
`XPCog-<version>-x86_64.tar.gz` to the run — so a pull request that breaks the
install rules says so where it broke rather than at release time. It also
validates the desktop file and the metainfo out of the *staged archive* rather
than the build tree, and extracts the tarball somewhere unrelated to any prefix
and plays a MIDI file with it: that one command proves the rpath still resolves
`libvgmstream.so` and that `<exe dir>/../share/xpcog` still finds the shipped
bank, both of which fail silently and neither of which any other job touches.

`linux-release` rather than `linux-repo-release`, and that is the point of the
choice: the system-libs presets exist for a machine that already has the
libraries, which is the opposite of a machine downloading a tarball. Building
against vcpkg's copies links seventeen of them in — `libtag`, `libavcodec`,
`libopenmpt` and the rest — so what the download needs from its host is GTK,
wxWidgets and the C library. **The C library is the floor:** built on
`ubuntu-24.04`, the tarball wants glibc 2.39 or newer, which rules out Ubuntu
22.04, Debian 12 and RHEL 9. Moving that line means moving the runner.

No `DEB` or `RPM` generator, deliberately. CPack can emit both, but a package
worth installing needs a dependency list, and `CPACK_DEBIAN_PACKAGE_SHLIBDEPS`
derives one naming the exact sonames of whichever distribution happened to build
it. That is a per-distribution job, and what a per-distribution packager needs
from this repository is `cmake --install`, which they have.

**Arch.** `packaging/arch/` holds a `PKGBUILD` that builds against Arch's own
libraries — the `linux-repo-release` trade applied to a distribution that has
nearly all of them:

```sh
cd packaging/arch && makepkg -si
```

It still runs vcpkg, and that is worth knowing before reaching for a clean
chroot: `mgba` and `libvgm` have no system path in `XPCogSystemDeps`, and the
four never-substituted libraries are compiled into the overlay ports, so vcpkg
downloads. The tree is pinned to the manifest's `builtin-baseline` and
`prepare()` fails the build if the two drift apart, so what is removed is
version drift rather than the network. `packaging/arch/README.md` covers the
three deliberate differences from the preset — no crash reporter, no tests, and
`libdir` set to `lib/xpcog` so the bundled `libvgmstream.so` does not claim a
name in `/usr/lib`.

**Flatpak.** `packaging/flatpak/co.losno.XPCog.yml` builds against
`org.gnome.Platform` 50, which makes it the only artefact here that runs on a
distribution regardless of its glibc:

```sh
flatpak run org.flatpak.Builder --force-clean --user --install \
    build-dir packaging/flatpak/co.losno.XPCog.yml
```

Nothing in it renames a file or patches an install rule: Flatpak wants the
desktop file, the metainfo and the icons named for the application ID, and
`packaging/linux/` installs them that way already.

**It is not ready for Flathub, and the obstacle is one line.** The `xpcog`
module asks for `--share=network`, because `flatpak-builder` builds offline and
vcpkg fetches port sources itself. Everything else about the manifest is
submittable. Removing vcpkg is most of the way done by the runtime — the SDK
already supplies SQLite, curl, libarchive, Ogg, Vorbis, FLAC, Opus, opusfile,
TagLib, WavPack and FFmpeg, all of which `XPCogSystemDeps` substitutes — and
what remains is listed in `packaging/flatpak/README.md`, the awkward entry
being that `codecs/flac` and `codecs/vorbis` want `find_package(... CONFIG)`
and the SDK ships those libraries without config packages. Flathub would also
want screenshots, which the metainfo has none of.

### Releases

**A release per version bump, made from the run that built it.** All three
packages above are attached to every run as artifacts, which is where a pull
request's go and where they stay. On `main` the `Release` job takes the same
three files — downloaded, not rebuilt — creates the tag `v<version>` on the
commit that was built, and publishes them.

What decides that a bump happened is whether a release for the version in
`CMakeLists.txt` exists yet, rather than a diff against the previous commit. The
diff is the obvious reading and it loses releases quietly: a push carrying
several commits, or a squash whose bump is not the tip, leaves the version
changed and the tip's diff empty. Asking whether `v<version>` is published
answers the same question from the state that matters, does nothing when re-run
on a commit already released, and repairs itself — a version that reaches `main`
without a release gets one on the next run.

It waits on the three packaging jobs and the version check, and on nothing else.
A failing test, a broken system-libs build or a headless link error does not hold
the release: those jobs say something about the tree, while the packaging jobs say
whether there is anything to publish. Failing to *build* any one package still stops it,
because `needs` skips the job and the artifacts would not be there to download.
The trade is deliberate — a version can be released with a red run behind it. The
run says which job failed, so what this changes is who decides: a release that
went out on a known failure is something to see and yank, rather than a packaged
build nobody can have because an unrelated job broke.

The notes name both files and say what was done to each: the installer is
unsigned, and the disk image says whether that run signed and notarised it rather
than asserting that it usually does. Everything after the notes is GitHub's own
list of what changed, read from the previous `v` release.

Once the release is up, the job POSTs to a Netlify build hook so that
[the download page](https://cog.losno.co/xpcog) rebuilds. That site is static and
reads this repository's releases at build time, so without the POST a release is
published and invisible until something else deploys. The hook URL lives in a
`WEBSITE_BUILD_HOOK` repository secret, because the URL *is* the credential --
holding it lets you start a deploy of that site and nothing else. It is optional:
unset, the step says so and does nothing, which is what a fork wants. It also
cannot fail the job, since the release is already public by then and a site one
version behind is fixed by the next release or by a deploy from Netlify.

The version itself is checked before any of that, by a `Version` job that runs on
pull requests too. `CMakeLists.txt` and `vcpkg.json` both carry it and are kept
identical; nothing in a build reads both, so a half-bump configures, compiles,
packages and passes every other job, and would surface only as a release tagged
one thing holding an installer named another.

### Linux: building against the distribution's libraries

wxWidgets is not the only dependency a Linux machine already has. The
`linux-repo-debug` and `linux-repo-release` presets are the ordinary Linux build
with one difference — before anything is asked of vcpkg, `pkg-config` is asked
what is installed, and every library the system has at a version this code can
use is taken from there:

```sh
sudo pacman -S ffmpeg curl libarchive taglib sqlite libsoxr opusfile wavpack \
               libopenmpt libgme catch2 libmpcdec          # Arch; similar elsewhere
paru -S vgmstream-git libspessasynth-git                    # AUR, and optional
cmake --preset linux-repo-release
cmake --build --preset linux-repo-release
```

On a machine with those installed vcpkg goes from 41 packages to 14 — 12 with
`vgmstream-git` as well, 11 with `libspessasynth-git` too — and from 643 MB of
`vcpkg_installed` to 46 MB. What is dropped is not the cheap half: FFmpeg is the
longest build in the manifest, OpenSSL is the second, and libarchive brings
bzip2, liblzma, lz4, zstd, libxml2 and libiconv with it. Nothing else changes —
same options, same codecs, same tests.

Every version floor is the oldest release carrying an API this code calls, and
[`cmake/XPCogSystemDeps.cmake`](cmake/XPCogSystemDeps.cmake) says which for each.
A library that is missing, or too old, is simply built by vcpkg as before; the
configure summary lists what was taken from the system and at what version, so
there is no guessing about which half a build came from. Two of the floors are
worth knowing about because a current distribution can fail them:

* **TagLib 2.0**, for `TagLib::Variant` — Ubuntu 24.04 ships 1.13.
* **Catch2 3.7.1**, which is where a binary whose tests all skipped began
  exiting 4 and `catch_discover_tests` began registering that with ctest. Below
  it, the many corpus-gated tests here are reported as failures rather than as
  skips — Ubuntu 24.04's 3.4.0 turns 95 of 600 green tests red.
* **libsidplayfp 2.x**, and *not* 3.0 or newer: sidplayfp 3.0 replaced the
  `play(short*, count)` this decoder is written against with a cycle-driven call
  and a separate mix step.

**vgmstream** and **SpessaSynth** are the two odd entries. Neither ships a `.pc`
file or a CMake config package, so both are found as a header and a library by
name; and what is packaged is not a release but a rolling build of a repository —
`vgmstream-git` and `libspessasynth-git`, both from the AUR, the second of them
maintained by this project's author. Neither can be held to a release number,
and each is held to the one thing its install does state about itself:

* **vgmstream** to `LIBVGMSTREAM_API_VERSION_*` in `libvgmstream.h`, read
  straight out of the header — at least 1.0, which is all this decoder calls, and
  below 2.0, which that header defines as the next set of breaking changes. A
  distribution build also has the optional codecs on where `ports/vgmstream`
  turns them all off, so the system copy decodes a superset and says so through
  `libvgmstream_get_extensions()`.
* **SpessaSynth** to its soname, at least `libspessasynth.so.11`, because its
  headers carry no version at all. Upstream commit 28a362a widened member types
  from `float` to `double` across the public headers without moving the soname
  off 10, so a package still at 10 may be either side of that change with nothing
  in the install to say which; 11 is the soname the break was finally given, and
  what `ports/spessasynth-core` is pinned past. A machine whose package predates
  the bump keeps building the port.

Along with libsidplayfp, vgmstream is one of the two entries with an upper bound.
These two are also the only ones CI cannot exercise the system half of — no
Debian or Ubuntu release packages either library — so the job below asserts the
fallback for them instead.

CI builds this configuration too, on Ubuntu 24.04, where TagLib and Catch2 fall
below their floors, vgmstream and SpessaSynth are not packaged at all, and the
other twelve do not — so one job exercises the system path and the fallback path
at once, and asserts against vcpkg's installed tree which of the two each
dependency took.

The plain `linux-debug` and `linux-release` presets are unchanged and still take
everything from vcpkg. That is what CI builds, and what to use when a build has
to come out the same on a machine other than the one that configured it — which
is also why the two sets of presets build into separate directories, and why
flipping `XPCOG_USE_SYSTEM_LIBS` inside one of them is refused rather than
obeyed.

Four libraries are never substituted, whatever is installed: `libogg`, `libflac`,
`libvorbis` and `zlib`. `codecs/flac` and `codecs/vorbis` link three of them
directly, and the overlay ports in [`ports/`](ports/README.md) build against all
four — SpessaSynth reads FLAC- and Vorbis-compressed SF3 samples, libvgm and mGBA
read gzip — so vcpkg builds the four whichever way the ports go, and linking a
second copy of any of them would buy nothing. mGBA is a deliberate omission of a
different kind: a system libmgba exists, and `struct mCore` declares its members
inside `#ifdef ENABLE_VFS` and friends, so one built with a different set of
those has different member offsets — which compiles, links, and then calls
whatever is in the slot.

### Other prerequisites

`nasm` is required on every platform for FFmpeg's assembly — vcpkg downloads it
itself on Windows, and expects the package manager to supply it elsewhere. macOS
also needs `pkg-config` for vcpkg's ports.

```sh
brew install ninja pkg-config nasm                              # macOS
sudo apt install ninja-build pkg-config nasm autoconf automake libtool \
                libwxgtk3.2-dev libglib2.0-dev                  # Debian/Ubuntu
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

Two parser bugs that corrupt real albums are fixed here rather than reproduced from
upstream:

- Upstream keeps one `artist` variable for the whole sheet and never resets it per
  track, so a single track-level `PERFORMER` mis-credits every following track.
  Track-level fields here fall back to the album value instead.
- A non-`AUDIO` `TRACK` is skipped, but upstream still lets its `INDEX` create an
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
rather than on an FM chip: `GeneralUserXG-SFeTest.sf3`, together with the `tg300b`
map that XPCog selects instead when a sequence announces itself as GS or GM2. Point `soundFontPath` at your own bank
to replace it, or drop one beside a file — `song.sf2`, or `Album/Album.sf2` for
a folder — to override it for that music alone. An RMID that carries its own
bank inside it beats all of those, since that bank is part of the music.

Archives are a *source* rather than a format,
so a `.zip` of FLAC plays without being unpacked first, and a Monkey's Audio Link
(`.apl`) is a *range* within one -- the same shape as a cue sheet track, which is
how a single-file CD rip becomes an album.

`xpcog-cli codecs` prints what a given build claims; a default one is 23 decoders
and 842 extensions.

Selection is by extension first, then MIME type, with several claimants tried in
descending priority. FFmpeg registers *below* default priority,
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
Track changes are announced when the seam becomes *audible*, not when it is
decoded.

The seam is covered by tests that run the real engine against a capturing output,
so they are deterministic and need no audio device: sample-exactness against
separately-decoded references, waveform continuity across the join, notification
ordering, and a three-track case where a per-seam off-by-one accumulates rather
than cancels. The tests were confirmed to fail when a chunk is deliberately dropped
at each seam.

A track at a **different sample rate** joins gaplessly too. The device stays at the
first track's format and later tracks are resampled into it (libsoxr),
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

That is deliberately stricter than upstream Cog, whose callback
(`Audio/Output/OutputCoreAudio.m:877`) takes an `NSLock` and enters an
`@autoreleasepool` on the real-time thread. `xpcog-cli play` reports underruns
separately for playback and for the post-stream drain, so a genuine dropout is
never confused with the expected tail.

### Crash reporting

**Off unless you turn it on.** On
the first launch XPCog asks, once, whether it may send crash reports and usage data
to <https://cog-analytics.losno.co>, and it never asks again whatever the answer
was. Preferences → General is where it is changed afterwards, and the switch takes
effect immediately in both directions — unticking it shuts the reporter down for
the running session rather than at the next launch.

Until it is ticked, no reporter is initialised, no report database is created and
nothing leaves the machine. That is deliberately stronger than the SDK's own
opt-in mode, which starts a client and then holds events back: here there is no
client. [What is collected, and what happens to
it](https://www.iubenda.com/privacy-policy/59859310).

It is [sentry-native](https://github.com/getsentry/sentry-native) underneath,
where Cog uses the Sentry Cocoa SDK, and the keys are Cog's own —
`sentryConsented` and `sentryAskedConsent` — so an answer given in Cog on macOS
carries over rather than being asked for again. The reporter lives in
`platform/`, behind a four-function header that never exposes the SDK; see
[`platform/include/xpcog/platform/CrashReporter.hpp`](platform/include/xpcog/platform/CrashReporter.hpp).

The presets build it. `-D XPCOG_WITH_SENTRY=OFF` leaves it out entirely, which is
the default for a plain `cmake` with no preset: the port builds crashpad, and that
is a lot to hand someone who only wants a player. Such a build still shows the
switch, greyed out, saying why.

## Last.fm

Scrobbling, with the **desktop** authentication flow rather than the mobile one
Cog uses: connecting opens last.fm in a browser and XPCog never sees the
password. The session key that comes back is kept in the platform's own secret
store — Credential Manager, the Keychain, the Secret Service — through
`wxSecretStore`, not in the settings.

Plays go on a durable queue before they are sent, so an evening spent offline
arrives the next time the machine has a network. Cog has no queue; a failed
submission there is simply gone.

**No API key ships with the source**, exactly as Cog ships none. Every line of
the feature is compiled either way, and a build without one shows the pane greyed
with an explanation. To build with scrobbling live, apply for a key at
[last.fm/api/account/create](https://www.last.fm/api/account/create) and
configure with:

```
cmake --preset windows-debug -D XPCOG_LASTFM_API_KEY=... -D XPCOG_LASTFM_API_SECRET=...
```

Both are also read from the environment when the cache variables are unset,
which keeps the secret out of your shell history and out of `CMakeCache.txt`.
That matters when it comes from a password manager, because `-D` copies it
straight back into plaintext beside the build:

```
op run --env-file=lastfm.env -- cmake --preset windows-debug
```

Configure prints `Last.fm: API key configured`, or says the feature will report
itself unavailable. CI reads the same two values out of the
`LASTFM_API_KEY` and `LASTFM_API_SECRET` repository secrets, by that same
environment path, and only in the two jobs that package something — the Windows
installer and the macOS disk image. No other job produces something a person
downloads. That job fails when configure
reports no key, because the alternative is shipping an installer whose Last.fm
pane is greyed with nothing to say why. A pull request from a fork cannot see
secrets and packages exactly such a build, deliberately. To check a key works before wiring anything up:

```
XPCOG_LASTFM_API_KEY=... XPCOG_LASTFM_API_SECRET=... xpcog-tests "[.lastfmlive]"
```

That asks the real service for a request token, which exercises the whole
signing path and touches no account. It is hidden, so an ordinary test run
never makes a network request.

The switch itself is `enableAudioScrobbler`, which is Cog's key — but it is the
one setting a Cog import deliberately does **not** carry across, because the
credential cannot come with it and the switch alone would claim a connection that
does not exist.

## Remote control

A REST API over the transport, the playlist, the equaliser, the settings and the
cover art, with a generated OpenAPI 3.1 document and a Swagger UI page beside it.
Preferences → Remote turns it on; `docs/REST.md` is the reference.

```sh
curl -H "Authorization: Bearer $TOKEN" localhost:7799/api/v1/status
xdg-open http://localhost:7799/docs
```

**Off in a default build, and off again at run time.** `app/src/SingleInstance.hpp`
records a decision against this program owning a listening socket at all — on Windows
the firewall asks the user to approve a *music player* wanting network access, which is
alarming and unanswerable — so the feature is opt-in twice: `XPCOG_WITH_REST` decides
whether a server is compiled (the presets turn it on), and `remoteEnable` decides
whether it ever binds. The default bind is loopback, which raises no prompt.

Every request needs a bearer token, with no exemption for local connections; the token
is kept in the system password store rather than in settings. There is no TLS and none
planned — a self-signed certificate on a LAN is theatre — so the connection is not
encrypted and the pane says so before you bind it to the network.

`xpcog-cli serve` runs the same API with no toolkit linked at all, which is what
demonstrates that none of this reached below the interface layer.

## Languages

The interface speaks **English** and **Spanish**, and follows the system by
default. Preferences → General has the picker; it applies the next time XPCog
starts, because a catalogue is chosen once, before the first window is built.

The translations live in [`app/locale`](app/locale) as ordinary gettext `.po`
files and are compiled into the binary — there is nothing to install beside the
executable and nothing that can go missing on one machine and not another. Adding
a language is one `.po`, one line of CMake and one row of a table;
[`app/locale/README.md`](app/locale/README.md) is the whole procedure, including
what is deliberately *not* translated and why.

Two conventional tools are replaced by two small ones, for the same reason:
gettext is not a dependency this project has anywhere else, and Windows is the
platform least likely to have it. `tools/extract-messages.py` stands in for
`xgettext`, and `cmake/CompileCatalog.cmake` for `msgfmt`. Neither is on the
build's critical path except the second, which is a CMake script.

`core`, `codecs` and `platform` are not translated and cannot be: they link no
toolkit, and so have no catalogue to consult. The handful of strings they produce
that a listener reads — the playlist's column headings — are mapped in the app
layer, which is what `PluginRegistry` and `PlaylistView` were already written to
allow.

## Status

What is outstanding is short, and each item is here for a reason rather than for want
of time:

- **Adopting an existing Cog installation** is most of the way there. File →
  Import from Cog reads its library, playlist order, ReplayGain and play counts;
  what is left is finding that installation without being pointed at it, which only
  matters on a Mac.
- **DoP output** waits on a DAC to verify it against, and **HRTF** is deferred.
- **Global hotkeys** are not coming: the media keys they would bind are already
  delivered by SMTC, MPRIS and MediaPlayer.framework.
- **NSDockTile** was dropped by decision.

The decoder list stands at 842 extensions across 23 decoders, up from the 30-odd the
first working build recognised. FLAC, MP3, Vorbis, Opus, AAC/ALAC and WavPack came
first, with APE and Musepack arriving through FFmpeg rather than decoders of their own;
Musepack has one now and APE does not, because FFmpeg already plays it and a second
path to the same audio would be upkeep for nothing. Extending the list is cheap by
design — a decoder is one `xpcog_add_codec()` call, never a refactor, and every one
added so far has cost exactly that.

### Deliberately out of scope

The Mac App Store sandbox (`SandboxBroker`, security-scoped bookmarks), AudioUnit MIDI
instrument hosting, AppleScript and Spotlight integration are macOS-only, and nothing
here reimplements them. A no-op `IFileAccess` seam preserves the sandbox call sites in
case that changes.

That is a record of what did not travel, not a boundary on what XPCog may do. Cog not
having something is a fact about Cog: it means the work would be new rather than
inherited, and is judged on its own terms. The [remote control](#remote-control) is the
first feature to land that way, and Cog's own MCP server sat on this list until it
did.

AudioUnit hosting is one of Cog's four MIDI backends, not MIDI itself — `.mid` and
its dozen relatives play here through the other three, all of which have landed:
SpessaSynth, Nuked OPL3 and Nuked SC-55. See [`docs/MIDI.md`](docs/MIDI.md).

## Relationship to Cog

XPCog is a derivative work of [Cog](https://github.com/losnoco/Cog), and the
[License](#license) below is not a formality about that. The debt is specific and
large: the six-protocol plugin contract the whole design hangs off, the
`NSUserDefaults` keys — kept identical, so an existing Cog plist imports verbatim —
and years of decisions about how a player of this kind should behave, down to quirks
worth preserving.

It is not only a port any more. It runs on Windows and Linux as well as macOS from one
codebase, it has a REST remote control and a translated interface, and its build
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

GPL-2.0-or-later, following upstream Cog. See [COPYING](COPYING).

Cog is copyright Vincent Spader and Christopher Snowhill. Bundled decoding and tagging
libraries are under their own licenses. Interface icons are [Lucide](https://lucide.dev)
under the ISC license — see [`app/icons/lucide/LICENSE`](app/icons/lucide/LICENSE).
