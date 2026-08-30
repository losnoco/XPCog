# XPCog on Arch

`PKGBUILD` builds XPCog against Arch's own libraries. It is the same trade the
`linux-repo-*` presets make — see `cmake/XPCogSystemDeps.cmake` — applied to a
distribution that has nearly all of them.

```sh
cd packaging/arch
makepkg -si
```

`.SRCINFO` is generated, never edited by hand, and has to be regenerated
whenever the `PKGBUILD` changes:

```sh
makepkg --printsrcinfo > .SRCINFO
```

## What it needs from the network

**vcpkg still runs, and it downloads.** That is worth being plain about, because
it is the one way this package is unusual for the AUR.

`cmake/XPCogSystemDeps.cmake` substitutes eighteen dependencies with the
system's, and Arch supplies most of them — but not all. `mgba` and `libvgm` have
no system path there at all, and `libogg`, `libflac`, `libvorbis` and `zlib` are
deliberately never substituted: the overlay ports are compiled against those
four, and linking a second system copy would put two of each in one process.
vcpkg fetches every port's sources itself rather than through `source=()`, so a
build in a network-isolated chroot will fail.

What the pin below removes is version drift, not the download. `_vcpkg_commit`
must equal the `builtin-baseline` in `vcpkg.json`, and `prepare()` fails the
build when it does not — two files that have to agree, and nothing else would
notice them drifting.

## Why `depends` is longer than it looks

`XPCogSystemDeps` decides *per library, at configure time* whether the system has
one good enough. That makes the dependency set a property of the build machine
unless something pins it: without `libsidplayfp` installed, vcpkg quietly builds
its own and the package links that instead.

`makepkg` installs `depends` before `build()` runs, so naming a library there is
what guarantees the system copy is the one found — and that two builds of the
same `pkgver` produce the same package. That is why `nlohmann-json`, which is
header-only and never linked, is in `makedepends`.

## Three differences from `linux-repo-release`

- **Crash reporting is off.** It is the upstream project's Sentry, and a
  distribution package is not the right thing to report from. It also drops
  sentry-native, crashpad and libunwind from the vcpkg build and frees the
  system libcurl: `XPCogSystemDeps` forces vcpkg's curl whenever `sentry` is
  asked for, so that one process cannot end up holding two of them.
- **Tests are off.** The suite is not what ships.
- **`CMAKE_INSTALL_LIBDIR` is `lib/xpcog`.** The one bundled shared library is
  vcpkg's `libvgmstream.so`, and a private copy of it in `/usr/lib` would claim
  a name a real `vgmstream` package may want. `cmake/XPCogInstallRuntime.cmake`
  derives both the install destination and the `$ORIGIN` rpath from that
  variable, so setting it is the entire change — there is nothing to patch.

## Keeping it in step

`pkgver` follows the released version, and the package builds from the GitHub
release tarball for that tag. **It cannot build a version older than 1.1.0:**
the install rules it relies on did not exist before then, and `cmake --install`
on an older tree produces the shipped assets and no player.
