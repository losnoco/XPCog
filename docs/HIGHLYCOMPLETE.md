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
| 1 | lazyusf2 → USF, `usf` + `miniusf` registered | **done** |
| 2… | the remaining seven cores | not started — see *Which core next* |

Stage 0 registered **nothing** with the plugin registry — deliberately, since a
decoder that cannot decode is worse than a format the player does not claim.
Stage 1 is the first core behind that container, so `usf` and `miniusf` are now
claimed and play. `codecs/psf` remains a plain library; the other seven cores
link it the same way `codecs/usf` does.

## Building and testing what exists

```
cmake --preset windows-debug -DXPCOG_WITH_PSF=ON -DXPCOG_PSF_CORPUS=<folder of rips>
```

`XPCOG_WITH_PSF` is `ON` in the presets, so CI builds it on all four jobs — one
switch for HighlyComplete as a whole, container and cores, matching Cog's single
plugin. `XPCOG_PSF_CORPUS` is unset everywhere but a developer machine: PSF files
are rips of copyrighted game programs and cannot be committed, so the corpus
cases skip without it. `tests/codecs/test_psf.cpp` covers the container,
`test_usf.cpp` the core — the latter decodes, because a core can resolve the
chain perfectly and still render silence.

The corpus this was developed against is `D:\vgm` on the author's Windows
machine and `/Volumes/gigante/vgm` on the Mac — the same 5,318 files, of which
the PSF family is 1,656:

| extension | count | library files |
|---|---|---|
| `.minigsf` | 783 | 12 `.gsflib` |
| `.miniusf` | 694 | 7 `.usflib` |
| `.mini2sf` | 179 | 4 `.2sflib` |

Any equivalent collection works; the tests find files by extension and assert on
relationships, not on specific titles.

## What stage 0 gives a core

`codecs/psf/PsfFile.hpp`:

- `loadPsf(url, registry, allowedVersion, wantNestedTags)` — walks the `_lib`
  chain and returns the program images **highest priority first**, which is the
  order they must be applied in. Pass the core's own version byte so a
  mismatched file is refused rather than fed to the wrong emulator.
  `wantNestedTags` also reports the libraries' tags, which USF needs: the
  `_enablecompare` switch belongs to the game, so a set puts it in the `.usflib`
  rather than in each of its hundred `.miniusf` files. A library's tags are
  strictly a fallback — `length`, `fade`, `volume` and everything the playlist
  shows come from the outermost file whatever this is set to, or one `.usflib`
  would give all fifty of its tracks the same title.
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
| `0x21` | usf, miniusf | lazyusf2 | 211 | **done** — `vendor/lazyusf2`, 52 built |
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

Stage 1 took **lazyusf2 → USF**, and the reason it was the cheaper of the two
choices is worth recording, because this document previously argued the
opposite. See *Lessons* below: the recompiler that made lazyusf2 look risky is
not compiled at all, on any architecture Cog ships.

That leaves seven, and the same trade as before:

- **mGBA → GSF.** The largest remaining share of a typical corpus (783 files
  here), and the only core with a real upstream and its own CMake, so no build
  file to write. Against it: 989 files and 77 MB, a heavier dependency for CI to
  build on four platforms, and mGBA's CMake builds a whole frontend ecosystem
  that has to be switched off option by option.
- **vio2sf → 2SF.** 179 files here, 255 sources, hand-written build. The middle
  option in every dimension.
- The four small ones — HighlyQuixotic at 11 files, HighlyExperimental at 25,
  HighlyTheoretical at 29, SSEQPlayer at 31. None has a single file in this
  corpus, so nothing could be heard at the end of any of them, but between them
  they are five of the eight formats for the size of one mGBA. HighlyExperimental
  additionally needs a PlayStation BIOS image; Cog carries `hebios.bin` beside
  `HCDecoder.mm`.

Whichever is next, the shape is now settled and `codecs/usf` is the template:
vendor under `vendor/`, hand-write a `CMakeLists.txt`, and implement `IDecoder`
over `loadPsf()`. Nothing about the container had to change for the first core
beyond `wantNestedTags`, which is the evidence that stage 0 was cut in the right
place.

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

### From stage 1 (lazyusf2 → USF)

- **lazyusf2 has no architecture problem, because its recompiler is not built.**
  This document used to warn that lazyusf2 "carries a MIPS recompiler that has
  historically been architecture-sensitive". It does carry one, in
  `r4300/x86/` and `r4300/x86_64/`, and it is reached only when `DYNAREC` is
  defined. Cog defines it for exactly one architecture:

      "GCC_PREPROCESSOR_DEFINITIONS[arch=i386]" = ( "$(inherit)", DYNAREC );

  So x86_64 and arm64 — every architecture Cog actually ships — have been
  running the interpreter for years. `vendor/lazyusf2` takes that configuration
  and goes one step further: it compiles no source from either directory and
  builds upstream's `r4300/empty_dynarec.c` instead, which exists for precisely
  this case. Their *headers* stay: `r4300/recomp.h` picks one of the two
  `assemble_struct.h` files unconditionally to size fields in `precomp_block`.

  52 files, clean on arm64, ~28× realtime in a Debug build.

- **`usf_render_resampled()` with a null buffer is not the same as rendering and
  discarding.** The API documents the null pointer as "render these and throw
  them away", and it is tempting for seeking. It takes a different path that
  converts the frame count back into the emulator's own rate with integer
  arithmetic and skips that many *there* — so it lands somewhere near the
  requested frame rather than on it. Seeks discard through a real buffer.

- **A seek must consume what the silence trim left behind.** The leading-silence
  scan (below) leaves a partial block buffered, so the skip loop's "give me 1024
  frames" can legitimately return 813. Counting the request rather than the
  answer put every seek several hundred frames early — audible as landing on the
  wrong beat, invisible to anything short of comparing samples against a
  straight decode. That comparison is now `tests/codecs/test_usf.cpp`, and it is
  the test that caught it.

- **A USF starts silent, and how silent varies by rip.** The save state is taken
  slightly before the sound driver does anything. Cog trims it with a threshold
  of 8 on a 16-bit sample and a ten-second ceiling, and the threshold is a
  *delta* from the previous sample rather than a distance from zero — an
  emulated DAC sitting idle rests on a small non-zero DC level, and a test
  against zero finds no silence at all in a track that is plainly silent. Left
  untrimmed, `length` measures from the wrong instant on every track of a set.

- **Cog's cores have never been linked on Windows, and it shows at link time,
  not compile time.** lazyusf2's `r4300/fpu.h` sets the FPU rounding mode
  through `__control87_2()` on MSVC, which the CRT provides only for 32-bit
  x86 — it exists to set the x87 and SSE2 control words separately and x64 has
  no x87 state — so the x64 job compiled every file cleanly and then failed with
  one unresolved external. `_controlfp_s()` is the supported spelling on both.
  Cog builds these for macOS alone, where the entire `_MSC_VER` branch is
  compiled out, so expect one of these per core rather than none. Local changes
  to vendored sources are marked in place with an "XPCog local change" comment
  and listed at the top of the core's `CMakeLists.txt`, so a re-vendor does not
  drop them silently.

- **A core must not start its emulator in `open()`.** `Scanner::readMetadata`
  opens a decoder for every file it walks, purely to ask for properties, and
  every answer `properties()` gives comes from the tag block — `length` is the
  duration and the rest is fixed. Booting the machine to answer that means
  allocating it, walking the `_lib` chain and inflating a multi-megabyte library
  once per track. So `open()` reads tags and stops, and the emulator waits for
  the first `readAudio()` or `seek()`. Cog does the same: `-open:` runs the
  metadata `psf_load` and nothing else, and `-initializeDecoder` is reached from
  `-readAudio` and `-seek`. Scanning 175 files against a 4.3 MB library, three
  runs each, is 10.60 / 10.37 / 9.65 s eager against 5.69 / 3.80 / 3.61 s lazy.

  Two consequences worth knowing before copying the shape. A mini-PSF orphaned
  from its library now opens and then fails at playback rather than failing to
  open, because the tags-only path stops before psflib follows `_lib` at all —
  which is also the only way to *test* laziness, since "no emulator started" is
  otherwise a stopwatch reading. And the version byte has to be checked by hand,
  since `readPsfTags()` probes rather than enforcing.

  **GSF is the exception**, in Cog and so here too: `-open:` initialises it
  eagerly because the sample rate comes from the core rather than from a tag.

- **No PSF metadata reader is registered.** `MetadataReadFn` takes only a `Url`
  and `readPsfTags()` needs a registry to resolve through. With `open()` now
  cheap there is nothing left to win, so the reader contract stays as it is.
