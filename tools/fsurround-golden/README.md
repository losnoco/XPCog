# FreeSurround golden capture

Captures reference output from **Cog's** FreeSurround decoder, so the C++ port in
`core/` can be held to Cog's numerics rather than to its own opinion of them.

The committed fixture is `tests/golden/fsurround-5point1.f32` and its manifest
`tests/golden/fsurround-5point1.txt`. Neither needs regenerating unless the
capture parameters change.

## Why it is not a build target

It compiles against a **Cog checkout**, not against anything in this tree, and it
only builds on macOS because FreeSurround's transform is vDSP. Wiring it into
CMake would put a Cog checkout and an Apple framework on the dependency list of
every build machine, in order to produce a file that is committed anyway.

## Running it

Needs a Cog checkout and Xcode's command line tools.

```sh
FS=~/Projects/Cog/Audio/ThirdParty/fsurround
clang++ -std=c++17 -O2 -I"$FS" \
    tools/fsurround-golden/capture.cpp \
    "$FS/freesurround_decoder.cpp" "$FS/channelmaps.cpp" \
    -framework Accelerate -o /tmp/fs-capture

cd tests/golden && /tmp/fs-capture fsurround-5point1
```

The manifest carries the shape and a per-block, per-channel RMS table, so the
capture can be sanity-checked without opening the binary. What it should show:
centre loud and rears **exactly** zero on the mono blocks, rears loud on the
anti-phase ones.

## What the fixture does not cover

- **Bass redirection.** Cog hardcodes `use_lfe(false)` in `FSurroundFilter.mm`,
  so LFE is silent for the whole capture and the redirection path is unexercised.
  Faithful to what Cog runs, and a real gap in coverage of the kernel. Capture a
  second variant if XPCog ever exposes the option.
- **Channel setups other than `cs_5point1`.** FreeSurround supports fourteen; Cog
  only ever constructs this one.
- **The soundfield transforms**, beyond the fixed values Cog sets. `circular_wrap`,
  `shift`, `depth` and `focus` are captured at Cog's settings, not swept.
