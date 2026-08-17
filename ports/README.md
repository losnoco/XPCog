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

Use [`../vendor/`](../vendor) instead when the code is a single file, has been
modified by Cog, or has no usable upstream — for example Cog's `lpc.c`,
`hdcd_decode2.c`, and the `dsd2pcm` filter extracted from `ChunkList.m`.

## Known additions

| Port | Why | Milestone |
|---|---|---|
| `libmpcdec` | Not in vcpkg. Note that vcpkg's `mpc` port is GNU MPC, an unrelated multiprecision library — an easy and costly mistake. Build from Cog's `Frameworks/MPCDec`. | M1b |
| `signalsmith-stretch` | Header-only, not packaged upstream. | M4 |
