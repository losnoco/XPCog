# Building XPCog

The long form of the README's [Building](../README.md#building) section: the
Windows installer, the Linux install tree and tarball,
how a release is made, building against a distribution's libraries, and what
each platform needs installed. Moved here from the README whole; nothing was
cut.

## The build

Requires **CMake 3.24+**, **Ninja**, a **C++20** compiler, and
[**vcpkg**](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set. XPCog
builds on two platforms, and the player is written for each one's own
toolkit: **wxWidgets on Windows** (`app/`), from vcpkg like every other
dependency, and **GTK 4 and libadwaita on Linux** (`app-gtk/`), from the
distribution. On Linux the `linux-repo-*` presets also take as much of the rest
from the distribution as the machine can supply; both are below. macOS is not a
target — Cog is the player there — though a headless build of the engine and
`xpcog-cli` still configures on it.

```sh
cmake --preset linux-debug                   # Linux
cmake --build --preset linux-debug
ctest --preset linux-debug
```

```bat
:: Windows, from a Developer Command Prompt
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

There is no deploy step. On Windows, vcpkg's applocal pass copies every dependent
DLL beside `XPCog.exe` as part of the build — the same pass that has always placed
FFmpeg's and TagLib's — so a freshly built binary starts from Explorer. The one
thing the build stages itself is `crashpad_handler`, which is a second executable
rather than a library and so is not something applocal knows about; it lands
beside the binary as a post-build step, on both platforms. What remains is
packaging — an installer on Windows, an install tree and a tarball on Linux, both
below.

wxWidgets is declared under a `gui` feature rather than as a plain dependency, so
a headless configuration (`-D XPCOG_BUILD_APP=OFF`) and the Linux presets build
no wx at all.

**On Linux the toolkit comes from the distribution**, never from vcpkg, whose
gtk ports would build the whole GNOME stack from source on a machine that
already has it. `cmake/XPCogGtk.cmake` finds it through pkg-config, and the
floor is **GTK 4.22, libadwaita 1.9 and GLib 2.88** — Ubuntu 26.04, Fedora 44,
Arch, or anything on GNOME 50 or later; Ubuntu 24.04 and Debian 13 are below
it. The interface is written in [Blueprint](https://gnome.pages.gitlab.gnome.org/blueprint-compiler/),
so **blueprint-compiler 0.16 or newer** is needed at build time, with the
Gtk-4.0 and Adw-1 typelibs it checks the files against. See
[Other prerequisites](#other-prerequisites) for the package names.

## Windows: the installer

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
:: -> build\windows-release\XPCog-2.0.0-x64-setup.exe
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
XPCog-2.0.0-x64-setup.exe /S /CurrentUser /NOASSOC /D=C:\Somewhere\XPCog
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
[Last.fm credentials](#lastfm-credentials) below.

## Linux: installing

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
a panel gets from the transport it is showing to XPCog's icon and name.

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
counterpart of `installer` on Windows. It is CPack's `TGZ`
generator over the install rules above, stripped — 120 MB of executable becomes
24, and the symbols stay in the build tree where a debugger and a crash report
want them.

```sh
cmake --build build/linux-repo-release --target package
tar tzf build/linux-repo-release/XPCog-1.6.0-x86_64.tar.gz
```

**It is not an AppImage and does not pretend to be.** GTK, libadwaita and
libsecret are the distribution's, linked dynamically, so the tarball
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
`linux-release` on `ubuntu-26.04`, packages it, and attaches
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
libadwaita, libsecret and the C library. **GTK is the floor:** 4.22, with
libadwaita 1.9 — Ubuntu 26.04, Fedora 44, Arch, anything on GNOME 50 or later.
The C library is one too, since the runner is `ubuntu-26.04`, but in practice
it is the lower of the two.

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

## Releases

**A release per version bump, made from the run that built it.** Both
packages above are attached to every run as artifacts, which is where a pull
request's go and where they stay. On `main` the `Release` job takes the same
two files — downloaded, not rebuilt — creates the tag `v<version>` on the
commit that was built, and publishes them.

What decides that a bump happened is whether a release for the version in
`CMakeLists.txt` exists yet, rather than a diff against the previous commit. The
diff is the obvious reading and it loses releases quietly: a push carrying
several commits, or a squash whose bump is not the tip, leaves the version
changed and the tip's diff empty. Asking whether `v<version>` is published
answers the same question from the state that matters, does nothing when re-run
on a commit already released, and repairs itself — a version that reaches `main`
without a release gets one on the next run.

It waits on the two packaging jobs and the version check, and on nothing else.
A failing test, a broken system-libs build or a headless link error does not hold
the release: those jobs say something about the tree, while the packaging jobs say
whether there is anything to publish. Failing to *build* any one package still stops it,
because `needs` skips the job and the artifacts would not be there to download.
The trade is deliberate — a version can be released with a red run behind it. The
run says which job failed, so what this changes is who decides: a release that
went out on a known failure is something to see and yank, rather than a packaged
build nobody can have because an unrelated job broke.

The notes name both files and what each needs: the installer is unsigned, and
the tarball wants GTK 4.22 and libadwaita 1.9 from the distribution. Everything after the notes is GitHub's own
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

## Linux: building against the distribution's libraries

GTK is not the only dependency a Linux machine already has. The
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

CI builds this configuration too — without the player, whose GTK floor is
newer than the runner — on Ubuntu 24.04, where TagLib and Catch2 fall
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

## Other prerequisites

`nasm` is required on every platform for FFmpeg's assembly — vcpkg downloads it
itself on Windows, and expects the package manager to supply it on Linux.

```sh
# Debian/Ubuntu (26.04 or newer)
sudo apt install ninja-build pkg-config nasm autoconf autoconf-archive automake libtool \
                libgtk-4-dev libadwaita-1-dev gir1.2-gtk-4.0 gir1.2-adw-1 \
                blueprint-compiler libglib2.0-dev libsecret-1-dev
# Fedora (44 or newer)
sudo dnf install ninja-build pkgconf nasm autoconf autoconf-archive automake libtool \
                gtk4-devel libadwaita-devel blueprint-compiler glib2-devel libsecret-devel
# Arch
sudo pacman -S ninja pkgconf nasm autoconf autoconf-archive automake libtool \
                gtk4 libadwaita blueprint-compiler glib2 libsecret
```

`xvfb` as well, to run `xpcog-gtk-tests` — the GTK suite that opens real
widgets — without a desktop; on a desktop session it uses the display it has.

Forty-one tests build their fixtures by shelling out to command-line
**encoders**, and *skip silently* when those are absent — a skip is not a failure,
so the suite still reports success while the gapless, seek and cue-span tests never
run. Install them to get real coverage:

```sh
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
cmake --preset linux-headless && cmake --build --preset linux-headless
./build/linux-headless/bin/xpcog-cli codecs
```

## Last.fm credentials

**No API key ships with the source**, exactly as Cog ships none. Every line of
the feature is compiled either way, and a build without one says so in the pane
and takes a key from the listener instead — Preferences → Last.fm → API account,
which is also how somebody with a built-in key scrobbles under an application of
their own (see [`docs/FEATURES.md`](FEATURES.md#lastfm)). To build with a key
baked in, apply for one at
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

Configure prints `Last.fm: API key configured`, or says one will have to be
entered in preferences. CI reads the same two values out of the
`LASTFM_API_KEY` and `LASTFM_API_SECRET` repository secrets, by that same
environment path, and only in the two jobs that package something — the Windows
installer and the Linux tarball. No other job produces something a person
downloads. Each fails when configure
reports no key, because the alternative is shipping an installer that asks
every listener to apply for an API account before it scrobbles. A pull request
from a fork cannot see secrets and packages exactly such a build, deliberately. To check a key works before wiring anything up:

```
XPCOG_LASTFM_API_KEY=... XPCOG_LASTFM_API_SECRET=... xpcog-tests "[.lastfmlive]"
```

That asks the real service for a request token, which exercises the whole
signing path and touches no account. It is hidden, so an ordinary test run
never makes a network request.
