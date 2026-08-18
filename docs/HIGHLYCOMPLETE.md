# HighlyComplete: staging plan

Cog's HighlyComplete is one decoder driving **eight emulator cores** — roughly
1,600 source files, of which mGBA alone is 989. It does not land in one commit,
so it lands in stages, and this is the record of where those stages are.

Written to be picked up on another machine. Everything here is either checked in
or reproducible; nothing depends on the session that started it.

## Where it stands

| stage | what | state |
|---|---|---|
| 0 | psflib vendored, PSF container, tests | **done** — `24df743` |
| 1 | first emulator core | not started — see *Which core next* |
| 2… | the remaining seven | not started |

Stage 0 registers **nothing** with the plugin registry. That is deliberate: a
decoder that cannot decode is worse than a format the player does not claim, and
it is the same rule that keeps the OpenMPT extension list honest. `codecs/psf` is
a plain library the first core will link.

## Building and testing what exists

```
cmake --preset windows-debug -DXPCOG_WITH_PSF=ON -DXPCOG_PSF_CORPUS=<folder of rips>
```

`XPCOG_WITH_PSF` is `ON` in the presets, so CI builds it on all four jobs and it
cannot rot before a core arrives. `XPCOG_PSF_CORPUS` is unset everywhere but a
developer machine: PSF files are rips of copyrighted game programs and cannot be
committed, so the chain tests skip without it. `tests/codecs/test_psf.cpp`.

The corpus this was developed against is `D:\vgm` on the author's machine —
5,318 files, of which the PSF family is 1,656:

| extension | count | library files |
|---|---|---|
| `.minigsf` | 783 | 12 `.gsflib` |
| `.miniusf` | 694 | 7 `.usflib` |
| `.mini2sf` | 179 | 4 `.2sflib` |

Any equivalent collection works; the tests find files by extension and assert on
relationships, not on specific titles.

## What stage 0 gives a core

`codecs/psf/PsfFile.hpp`:

- `loadPsf(url, registry, allowedVersion)` — walks the `_lib` chain and returns
  the program images **highest priority first**, which is the order they must be
  applied in. Pass the core's own version byte so a mismatched file is refused
  rather than fed to the wrong emulator.
- `readPsfTags(url, registry)` — tags only, without inflating any program image.
  A `.gsflib` is megabytes and none of it is needed to answer "what is this
  track called", and the scanner asks that for every file in a library.
- `parsePsfTime(text)` — `[[hours:]minutes:]seconds[.fraction]`.
- `PsfFile::length`, `::fade`, `::volume` — from the tag block.

Libraries resolve through `PluginRegistry::makeSource()`, so a PSF inside an
archive finds its libraries in that same archive.

## What a core has to do

1. Call `loadPsf()` with its version byte.
2. Apply `programs` in the order given, each overlaying the last, into whatever
   memory image the emulator wants.
3. Emulate, producing PCM.
4. **Honour `length` and `fade` itself.** PSF has no intrinsic duration — the
   program would run for ever — so the `length` tag is the only thing that makes
   a track finite. Without it a track reports no duration and never advances the
   playlist. `volume` scales the output.
5. Register a decoder for its extensions, both the full and the `mini` form.

## The two sections

**Which section carries the program depends on the console**, and a core cannot
ignore it. This cost a debugging session already:

- **GSF** puts the GBA image in `exe`.
- **USF** leaves `exe` *empty* and keeps the N64 data in `reserved`, which is
  where lazyusf looks.
- **PSF2** is a filesystem in `reserved` rather than an executable — that is what
  `psf2fs.c` (already vendored) is for.

A chain test asserting on `exe` alone passes for GSF and fails for every USF
file while the chain is resolving perfectly.

## The cores

Version bytes verified against Cog's `HCDecoder.mm`, not recalled.

| version | formats | core | files | source |
|---|---|---|---|---|
| `0x22` | gsf, minigsf | mGBA | 989 | submodule, `github.com/kode54/mgba` |
| `0x21` | usf, miniusf | lazyusf2 | 211 | vendored in Cog |
| `0x24` | 2sf, mini2sf | vio2sf | 255 | vendored in Cog |
| `0x25` | ncsf, minincsf | SSEQPlayer | 31 | vendored in Cog |
| `0x23` | snsf, minisnsf | snes9x | 36 | vendored in Cog |
| `0x12` | dsf, minidsf | HighlyTheoretical | 29 | vendored in Cog |
| `0x11` | ssf, minissf | HighlyTheoretical | — | same core |
| `0x01`/`0x02` | psf, psf2 + mini | HighlyExperimental | 25 | vendored in Cog |
| `0x41` | qsf, miniqsf | HighlyQuixotic | 11 | vendored in Cog |

Cog's copies live in `~/Projects/Cog/Frameworks/<name>/`. Only **mGBA** and
AdPlug are git submodules with a real upstream; the rest are kode54's code
vendored into Cog's tree, which per [`ports/README.md`](../ports/README.md) makes
them `vendor/` candidates rather than overlay ports, and each needs a
hand-written `CMakeLists.txt` since upstream ships only an Xcode project.

`HighlyExperimental` also needs a PlayStation BIOS image; Cog carries
`hebios.bin` beside `HCDecoder.mm`.

## Which core next

Two reasonable first choices, and the trade is size against build risk:

- **mGBA → GSF.** The largest share of a typical corpus (783 files here), and
  the only core with a real upstream and its own CMake, so no build file to
  write. Against it: 989 files, and a heavier dependency for CI to build on four
  platforms.
- **lazyusf2 → USF.** 694 files here and a fifth the size of mGBA, but upstream
  ships only an Xcode project so the build is hand-written, and lazyusf2 carries
  a MIPS recompiler that has historically been architecture-sensitive — worth
  checking it builds for arm64 macOS before committing to it.

The smallest cores — HighlyQuixotic at 11 files, HighlyExperimental at 25 —
would prove the whole shape fastest, but neither has any files in this corpus,
so nothing could be heard at the end of it. Still the right choice if the goal is
to de-risk the core contract before taking on a big emulator.

## Lessons already paid for

Recorded here so a later stage does not rediscover them. The vgmstream port has
its own set in [`PORTING.md`](PORTING.md) and [`ports/README.md`](../ports/README.md).

- psflib's file callbacks take **no user pointer**, so the registry reaches them
  through a thread-local scoped to the one `psf_load()` call. Anything a core
  needs inside those callbacks has to arrive the same way.
- `psf2fs.h` includes `<psflib/psflib.h>`, so the vendored sources keep Cog's
  nested `psflib/psflib/` layout rather than being flattened.
- zlib is now a declared dependency in `vcpkg.json`. It had been arriving
  transitively through libgme while two places called `find_package(ZLIB
  REQUIRED)` directly.
- A test failing is not the same as the thing under test being broken — see
  *The two sections*.
