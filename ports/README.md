# vcpkg overlay ports

Dependencies that vcpkg does not carry upstream, or that XPCog needs a patched
version of. `vcpkg-configuration.json` registers this directory as an overlay, so
anything here resolves as an ordinary manifest dependency from `vcpkg.json`.

This directory must exist even when empty — vcpkg fails configuration with
*"Overlay path must be an existing directory"* otherwise, which is why this file is
checked in.

## When to add a port here vs. `vendor/`

Use an **overlay port** when the dependency has a real upstream with a release
tarball or a pinned commit. This is the pattern for the ~32 vendored libraries Cog
carries (vgmstream, OpenMPT, GME, libsidplayfp, the emulator cores) as they land in
M6 — each becomes one directory here with a `portfile.cmake` and a `vcpkg.json`,
plus a hand-written `CMakeLists.txt` where upstream ships only an Xcode project.

Prefer it even when the dependency is large. mGBA is the case that settled it:
989 sources, and as a port CI compiles them once per platform and restores from
the binary cache thereafter rather than rebuilding on every push, with none of
it in the tree. Cog carries mGBA as a submodule because Cog has no package
manager; that is not a reason to copy the choice.

Use [`../vendor/`](../vendor) instead when the code is a single file, has been
modified by Cog, or has no usable upstream — for example Cog's `lpc.c`,
`hdcd_decode2.c`, and the `dsd2pcm` filter extracted from `ChunkList.m`.

## One trap when adding the first port

A port's manifest is also called `vcpkg.json`. CI's `lukka/run-vcpkg` step finds
the vcpkg baseline by globbing `**/vcpkg.json`, and when that matches more than
one file it reads none of them — the job then dies at setup with *"A Git commit
id for vcpkg's baseline was not found"*, which points at the baseline, which is
present and correct. Adding `vgmstream` took all four jobs down exactly this way.
`.github/workflows/ci.yml` now passes `vcpkgJsonIgnores` for `**/ports/**`, so
further ports here need no change.

## Known additions

| Port | Why | Milestone |
|---|---|---|
| `libmpcdec` | Not in vcpkg. Note that vcpkg's `mpc` port is GNU MPC, an unrelated multiprecision library — an easy and costly mistake. Build from Cog's `Frameworks/MPCDec`. | M1b |
| `signalsmith-stretch` | Header-only, not packaged upstream. | M4 |
| `vgmstream` | Not in vcpkg. Needs one patch: on MSVC the static target and the shared target's import library are both `src/libvgmstream.lib`, which ninja refuses. | M6 |
| `libsidplayfp` | Not in vcpkg, and the first port here to carry its own `CMakeLists.txt`: upstream builds with autotools and nothing else, so a `./configure` step would mean autoconf, automake, libtool and a POSIX shell on every platform including MSVC. Two traps, both recorded in the portfile: `version.cc` is the only translation unit defining `__VERSION_CC__`, so leaving it out links everywhere except one call site in `getCredits()`; and the two MUS players are 6502 assembly built with `xa65`, which is neither in vcpkg nor on a runner, so they ship pre-assembled as Cog's own `update-generated.sh` produces them. | M6 |
| `mgba` | Not in vcpkg. The GBA core behind GSF, from kode54's fork at the commit Cog pins. Two traps, both recorded in the portfile: `LIBMGBA_ONLY` forces zlib off and mGBA then compiles its own colliding `crc32()`, and the generated `mgba/flags.h` misreports three of the feature macros that decide `struct mCore`'s layout — see `codecs/gsf/CMakeLists.txt`. | M6 |
