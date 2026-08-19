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
| 2 | mGBA → GSF, `gsf` + `minigsf` registered | **done** |
| 3 | melonDS → 2SF, `2sf` + `mini2sf` registered | **done** |
| 4 | snes9x → SNSF, `snsf` + `minisnsf` registered | **done** |
| 5 | HighlyTheoretical → SSF *and* DSF | **done** |
| 6 | SSEQPlayer → NCSF, `ncsf` + `minincsf` registered | **done** |
| 7 | HighlyExperimental → PSF *and* PSF2 | **done** |
| 8 | HighlyQuixotic → QSF, `qsf` + `miniqsf` registered | **done** — all eight |

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

Sweeps over the whole corpus, which is how each core was checked beyond the
tests: all 694 `.miniusf` open and report a duration, and all 783 `.minigsf` do,
none failing, and all 179 `.mini2sf`. The GSF sweep also collects sample rates, and every file reports
65536 Hz. That uniformity is worth re-checking after any change to the rate
probe — a sweep that suddenly reports 32768 across the board means the probe has
gone back to reading the reset default. See *From stage 2*.

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
| `0x22` | gsf, minigsf | mGBA | 989 | **done** — overlay port, `kode54/mgba` |
| `0x21` | usf, miniusf | lazyusf2 | 211 | **done** — `vendor/lazyusf2`, 52 built |
| `0x24` | 2sf, mini2sf | **melonDS**, not vio2sf | 44 built | **done** — `vendor/vio2sf` |
| `0x25` | ncsf, minincsf | SSEQPlayer | 13 built | **done** — `vendor/sseqplayer` |
| `0x23` | snsf, minisnsf | snes9x | 18 built | **done** — `vendor/snes9x` |
| `0x12` | dsf, minidsf | HighlyTheoretical | 7 built | **done** — `vendor/highlytheoretical` |
| `0x11` | ssf, minissf | HighlyTheoretical | — | **done** — same core, same decoder |
| `0x01`/`0x02` | psf, psf2 + mini | HighlyExperimental | 8 built | **done** — `vendor/highlyexperimental` |
| `0x41` | qsf, miniqsf | HighlyQuixotic | 4 built | **done** — `vendor/highlyquixotic` |

Cog's copies live in `~/Projects/Cog/Frameworks/<name>/`. Only **mGBA** and
AdPlug are git submodules with a real upstream; the rest are kode54's code
vendored into Cog's tree, which per [`ports/README.md`](../ports/README.md) makes
them `vendor/` candidates rather than overlay ports, and each needs a
hand-written `CMakeLists.txt` since upstream ships only an Xcode project.

`HighlyExperimental` also needs a PlayStation BIOS image; Cog carries
`hebios.bin` beside `HCDecoder.mm`.

## All eight

Ten formats, eight cores, and the container they share. Nothing in
HighlyComplete is left unported.

| what it is | proved by |
|---|---|
| USF | N64 rips across the corpus, seek verified sample-exact |
| GSF | 783 files swept for sample rate; a half-speed bug caught by ear |
| 2SF | Super Mario 64 DS and others |
| SNSF | Tales of Phantasia's *Yume wa Owaranai*, vocal intact |
| SSF / DSF | Sonic Shuffle and NiGHTS, 100 files |
| NCSF | Mario & Luigi: Bowser's Inside Story |
| PSF / PSF2 | soss-psf and Final Fantasy X, 184 files |
| QSF | Street Fighter Alpha 2, 59 files |

Every core was proved by listening, and for six of the eight the decisive
material came from outside the main corpus. Nothing in this tree can hear a
wrong sample rate, a starved stream, a sequence number that was ignored, or an
instrument bank resolved to the wrong samples — so a green test run has never
been the last step for any of them.

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

### From stage 8 (HighlyQuixotic → QSF)

- **`PsfProgram`'s ordering comment was wrong, and had been since stage 0.** It
  said psflib hands the chain over "highest priority first". It does not: the
  callback order is load order — `_lib` and its own chain, then the file itself,
  then `_lib2` onwards — and a core applies them in that order letting each write
  over the last, so the main file overrides its library by arriving *later*.
  Every core had the code right and only the comment wrong, which is exactly the
  kind of error that survives: nothing fails until someone believes it.
- **QSF is where believing it would have cost something.** A miniqsf's entire
  contribution is usually one byte written over the library's Z80 image, and it
  is the track number. Apply the chain backwards and every file in the set still
  opens, still reports its tags and duration, and still plays — the same track,
  fifty-nine times over. Reversing the loop deliberately confirmed this: four of
  the five QSF tests still passed, and only *two QSFs from one library play
  different tracks* caught it.
- **The Kabuki key is not in `reserved`.** This document previously said it was.
  It arrives as an optional 11-byte `KEY` section in the program image, beside
  `Z80` and `SMP`, and a rip whose ROMs were already decrypted simply omits it —
  Street Fighter Alpha 2 does. A zero key is upstream's own signal to run the
  ROM as it stands, so the interesting path here is the *absent* one.
- **The one core that boots nothing.** Every other core here starts a machine:
  a BIOS, a firmware, a cartridge header. A CPS-2 sound board is a Z80 and a
  DSP, and a QSF carries both ROMs outright, so the file is the whole machine.
  It was also the only core to build and produce correct audio on the first
  attempt.

### From stage 7 (HighlyExperimental → PSF and PSF2)

- **The BIOS is Sony's, and that is a decision rather than a detail.**
  `vendor/highlyexperimental/Core/hebios.h` is 512 KB of PlayStation 2 BIOS (5.0
  North American) stripped to the sound modules, and it is required for both
  formats. Upstream is explicit in `Core/Readme.txt`: *"The unfortunate dirty
  secret here is that Sony BIOS is used… Making this stuff 100% legal (via IOP
  kernel and PS1 BIOS HLE) is on the to-do list."* It is vendored because Cog
  vendors it and this port follows Cog. If that HLE work ever lands upstream,
  this file is what it replaces; the alternative, if it is ever wanted, is
  loading the image from a user data directory and declining without it.

- **The two formats load by completely different mechanisms.** A PSF carries a
  PS-EXE in `exe` — 0x800 bytes of header stating a load address, entry point
  and stack pointer, then code — which is uploaded into IOP RAM, and the
  *first* image the chain returns supplies the registers the machine starts
  from. A PSF2 carries no executable at all: its sections are a **filesystem**,
  and the emulator reads files out of it on demand through a callback while it
  runs. This is what `psf2fs.c` has been sitting in `vendor/psflib` for since
  stage 0. PSF is "load a program and run it"; PSF2 is "boot a machine and let
  it mount this".

- **Two consoles again, two rates.** `psx_get_state_size(1)` builds a PS1 at
  44.1 kHz and `(2)` a PS2 at 48 kHz. Same shape as HighlyTheoretical, and the
  same risk as the GSF rate bug if the version byte is ever ignored.

- **A corpus root can contain orphans, and refusing them is correct.** A
  mini-PSF separated from its library loads its *tags* fine and fails to load
  its *program*, which is exactly the contract stage 0 set. A test that takes
  the first file the directory walk returns will eventually take one of these
  and read it as a decoder bug — as one here did. Tests now try candidates until
  a chain resolves.

- **`info` does not prove a core works.** Since stage 1 the cores open lazily, so
  a sweep built on `xpcog-cli info` only proves the tag block parses — the
  program is never loaded and no audio is rendered. Sweeps that mean anything
  have to `decode`. This was noticed here, and applies retroactively to the USF,
  2SF, SNSF and NCSF sweeps recorded above.

### From stage 6 (SSEQPlayer → NCSF)

- **This one is not an emulator, and the core contract fits it anyway.** An NCSF
  holds an SDAT — the DS's standard sound archive — and SSEQPlayer *performs*
  it: parsing the SSEQ sequence, resolving the SBNK instrument bank and the
  SWAR/SWAV samples, mixing sixteen channels in software. No ARM code runs.
  Which means no save state, no ROM assembled at addresses, no CPU-APU
  handshake, and none of the section-layout care the other five need — the
  archive arrives in one piece and `reserved` is four bytes naming which
  sequence to play. The `IDecoder` shape needed no adjustment for it, which is
  the useful finding: the contract is about *audio*, not about emulation.

- **The sequence number is the only thing distinguishing a track.** Every file
  in a set shares one `.ncsflib`. Drop those four bytes and all 45 files still
  decode, still report their own titles, lengths and ReplayGain, and all play
  sequence zero. `test_ncsf.cpp` decodes two files from one set and requires
  they differ — the analogue of the wrong-overlay-order failure elsewhere, and
  equally invisible to every other check.

- **It reports failure by throwing.** SSEQPlayer signals a malformed archive, an
  absent sequence or an unresolvable bank with `std::runtime_error` where the
  emulator cores return a status. A rip is an untrusted file, so the decoder
  catches at both `start()` and render and declines. Cog wraps the same calls.

### From stage 5 (HighlyTheoretical → SSF and DSF)

- **One core, two consoles, and the version byte is the switch rather than a
  check.** `sega_get_state_size(version - 0x10)` builds a Saturn for `0x11` and
  a Dreamcast for `0x12`. They share a library because they share a sound chip:
  the Saturn's SCSP and the Dreamcast's AICA are the same Yamaha design, and
  `yam.c` is it. What differs is the processor feeding it — a 68000 on Saturn,
  an ARM7 on Dreamcast — so `codecs/sdsf` registers four extensions and builds
  whichever machine the file asks for.

- **A fourth section layout, and the only one whose origin moves.** An SSF/DSF
  section is a four-byte little-endian load address followed by data, and
  sections merge into a single image that can grow at *either* end: a section
  starting before everything merged so far shifts the existing data up,
  zero-fills the gap and rewrites the address at the head. Every other core's
  loader fixes the origin from the first section and only ever writes within it.

- **`_lib` chains go deeper than two.** The NiGHTS set names six libraries from
  a single track — `_lib` through `_lib6`, 177 `.ssflib` behind 30 `.minissf`.
  psflib handles the numbered form (`_lib%u`) and returns the images highest
  priority first; merging them in any other order assembles the track from the
  wrong overlays, which plays, and plays the wrong music. `test_sdsf.cpp`
  asserts the chain returns at least as many programs as the file names.

- **The DSP is not optional.** `sega_enable_dsp()` switches on the effects
  processor where a Saturn or Dreamcast soundtrack's reverb and filtering live;
  without it the notes play and the mix does not. Its dynamic recompiler is
  switched off, as every recompiler in this tree is.

### From stage 4 (snes9x → SNSF)

- **SNSF is why the format family needs whole machines.** The SPC700 has 64 KB
  of audio RAM, which at its own BRR encoding is about 3.6 seconds of sample
  data. Tales of Phantasia's vocal theme does not fit and never could: Wolf
  Team's driver streams sample chunks from cartridge ROM through the CPU-APU I/O
  ports as the music plays, under a sound driver computing sixteen virtual
  voices and sounding the loudest eight. An `.spc` is a snapshot of those 64 KB
  and cannot contain such a track — which is the whole reason SNSF rips the
  cartridge instead.

  So the CPU-APU handshake is what this core has to keep honest, and
  `Settings.SoundSync` is the setting that does it: it ties emulation to the
  sound output rather than to a video frame rate, so the APU is never starved by
  a frame running long.

- **A third section layout, different again.** GSF has one header per section
  and 2SF has offset/length chunks in both sections. SNSF takes the **first**
  section's offset as a base, biases every later section by it, and masks the
  result to `0x1fffffff`. Its `reserved` holds type/length records where type 0
  is save RAM, erased to `0xff`, and the payload begins with its own offset.
  Assume any of these resemble each other and the cartridge assembles at the
  wrong addresses, which boots to silence with the chain and tags all correct.

- **A rip set is not all music, and a test must not assume it is.** The first
  `.minisnsf` in a set alphabetically may be `sfx-005F` — two seconds of a sound
  effect and then 156 seconds of nothing, which is correct. A test asserting
  "there is audio 45 seconds in" fails on it while the decoder is perfect. What
  is true of every entry is that the decoder keeps *producing frames* until the
  declared length runs out, so that is what `test_snsf.cpp` asserts, with the
  content-varies check reserved for a track long enough to have content.

- **`_lib` is matched by exact name.** The set this was developed against says
  `_lib=top.snsflib` and ships `Top.snsflib`. That resolves on macOS and Windows
  and would not on a case-sensitive filesystem. psflib opens by the name given,
  and so does Cog; worth knowing before blaming a core for a set that plays on
  one machine and not another.

### From stage 3 (melonDS → 2SF)

- **"vio2sf" is melonDS.** The table above used to say the 2SF core is vio2sf,
  a DeSmuME derivative. kode54 replaced the contents of Cog's `vio2sf`
  framework with melonDS and kept the name, so Cog's decoder says
  `#import <vio2sf/NDS.h>` and gets melonDS. Every source file in the tree has
  its includes rewritten to that prefix, which is why `vendor/vio2sf` keeps a
  directory literally named `vio2sf/`. Check what a framework *contains* before
  believing what it is called.

- **Read what the build builds, not what upstream's CMakeLists says.** melonDS
  ships a perfectly good `src/CMakeLists.txt` listing 59 sources plus teakra,
  and following it produces compile errors — because Cog does not build most of
  it. Its Xcode project uses folder synchronisation with a
  `membershipExceptions` list, and that list excludes **all** the DSi sources,
  all of `DSP_HLE/`, all of teakra, both JIT backends, the GL renderers and the
  GDB stub: 64 files. Once they are gone the build is 44 sources and needs no
  teakra at all, since `DSi_DSP.cpp` was its only consumer.

  The exclusions are not arbitrary tidiness either. `DSi.cpp` references
  `args.NANDImage`, and `NANDImage` is *commented out* in kode54's `Args.h` --
  that file cannot compile in this tree, which is how the exclusion list was
  found.

- **The header layout on disk is not the layout the compiler sees.** Cog's
  framework copies every header, from both `include/` and `src/`, into one flat
  `vio2sf.framework/Headers/`, and the sources were rewritten to match:
  `#include <vio2sf/ff.h>` for a file that lives in `fatfs/`. Preserving the
  directory structure faithfully gives a tree that cannot compile. There are no
  basename collisions across the 93 headers, so flattening is lossless.

- **melonDS's JIT is built by Cog and is not built here.** `JIT_ENABLED=1` is
  unconditional in Cog's Xcode project. A recompiler needs memory that is both
  writable and executable, which on Apple Silicon means MAP_JIT,
  `pthread_jit_write_protect_np` and the `com.apple.security.cs.allow-jit`
  entitlement under hardened runtime, and on Windows is why melonDS links
  `onecore`. The interpreter runs at roughly 9x realtime in a *Debug* build,
  which is ample, so none of that packaging surface is worth taking on. The
  sources are still vendored, behind upstream's `ENABLE_JIT`.

- **2SF is the only format here that uses both sections.** `exe` is the ROM as
  offset/length chunks; `reserved` is a run of `SAVE` records, each zlib
  compressed with its own CRC *inside* the section psflib already CRC-checked.
  Cog parses the save image and then never gives it to the emulator -- only the
  cartridge is used -- and this does the same, because parsing it is what makes
  a corrupt one a refusal rather than arbitrary playback.

- **A 2SF opens with the game booting.** `_2sf_initial_frames` says how many
  frames to run and discard first, and no `.2sflib` in this corpus sets it, so
  the first second or so is a DS powering on. Cog behaves identically. This is
  not the USF silence trim and deliberately not treated as one: the rip is
  supposed to state it.

- **Two Windows failures per vendored core, not one, and neither is exotic.**
  melonDS uses `__builtin_popcount` and `__builtin_ctzll` unguarded in a few
  places while `BitSet.h` beside them has a proper `#elif defined(_MSC_VER)`
  arm -- an inconsistency nobody would notice, because Cog builds this for
  macOS only. `vendor/vio2sf/platform/MsvcBuiltins.h` supplies them as a
  force-include rather than patching the sources, since the next file to use one
  should not need another patch.

  The other is worth memorising: `std::max` failing with *"error C2589: '(':
  illegal token on right side of '::'"* is `<windows.h>`'s `max` macro, and the
  fix is `NOMINMAX`. `XPCog::warnings` already defines it, and vendored targets
  deliberately do not link `XPCog::warnings`, so **every vendored core has to
  define it again for itself**.

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

### From stage 2 (mGBA → GSF)

- **A core's public headers may not describe the library that was built.** This
  cost most of the stage and produced a crash that looks like nothing else.
  mGBA's `struct mCore` — the vtable every call goes through — declares its
  members inside `#ifdef ENABLE_VFS`, `#if defined(ENABLE_VFS) &&
  defined(ENABLE_DIRECTORIES)`, `#ifndef MINIMAL_CORE`. mGBA passes those as
  `-D` on its own targets, and `mgba-util/common.h`, which every public header
  goes through, includes the C library and none of mGBA's own configuration. So
  a consumer compiles a *different struct* — it builds, it links, and then it
  reads every function pointer from the wrong offset. Here that was address zero
  inside the first `core->init()`.

  `mgba/flags.h` is the generated header that appears to solve this and does
  not: at the pinned commit it reports `ENABLE_DIRECTORIES`, `ENABLE_VFS_FD` and
  `USE_MINIZIP` as undefined while the library is compiled with all three.
  `codecs/gsf/CMakeLists.txt` states the set on the imported target instead, and
  records how to re-derive it from the port's own `build.ninja`. **Check this
  first for every remaining core that has a real upstream.**

- **A GBA has no sample rate, and asking too early gets the reset default.** The
  sound hardware runs at `0x200 >> SOUNDBIAS.resolution` cycles per sample —
  32768 Hz out of reset, and the *game* writes that register during its own
  startup. So the answer does not exist until the ROM has been loaded, reset and
  run far enough to configure its audio, which is why GSF starts its emulator in
  `open()` while USF does not.

  The trap is that the GBA emits samples from the very first frame, at the
  32768 Hz default, before the game has touched SOUNDBIAS. A probe that runs
  "until there is audio" — the obvious formulation — therefore stops on frame
  zero and reports 32768 for everything. Super Mario Advance switches to 65536
  around frame 20. This shipped, and was caught by someone listening to a track
  and hearing it play at half speed; every automated measurement of it —
  duration, peak, fade, chain resolution — was correct throughout.

  So the probe runs until the rate has held for half a second, then resets the
  machine and clears the audio buffer. The reset is not optional: mGBA's buffer
  is a plain circle buffer with no resampler, it saturates within a few frames,
  and keeping the probe's audio would start the track somewhere inside its first
  second.

  Cog declares a constant 65536 with an `// XXX` beside it and the
  `audioSampleRate()` call left commented out. **On this corpus that constant is
  correct** — all 783 `.minigsf` across all twelve games report 65536. Reading
  it from the core is still the right thing, because 32768 rips exist and the
  constant is silently an octave out for them, but nothing here demonstrates a
  bug in Cog. An earlier revision of this document claimed a 546/237 split; that
  was the early-probe bug above measuring itself.

- **mGBA is told a power-of-two ROM size and allocated ten bytes more.** It masks
  cartridge addresses against the size rather than bounds-checking them, so a
  size that is not a power of two makes an ordinary read land outside the buffer;
  the ten bytes beyond are for the ARM7 prefetching past the end of a ROM. Cog
  allocates `rsize + 10` and passes `rsize`, and conflating the two is a SIGBUS
  rather than a wrong note.

- **An imported target assembled by hand inherits none of the library's own link
  libraries.** mGBA declares `ws2_32 shlwapi` on Win32, `-framework Foundation`
  on Apple, libm elsewhere, and zlib. Omitting them links fine anywhere another
  target happens to pull the same library in first, and fails hard where nothing
  does — Windows, on `PathRemoveFileSpecW` and `PathIsRelativeW` from shlwapi.
  Exactly the failure `codecs/gme/CMakeLists.txt` already records for libgme and
  zlib, so treat it as the default expectation for every core that ships no
  CMake config package: read the platform blocks of its build and copy them.

- **`LIBMGBA_ONLY` is the obvious switch and the wrong one.** It forces
  `DISABLE_DEPS`, which turns `USE_ZLIB` off, and mGBA without zlib compiles its
  own `crc32()` with zlib's exact signature — which collides at link time with
  the real zlib that psflib and lazyusf2 pull in. Both `set()` calls are plain
  rather than cached, so `-DUSE_ZLIB=ON` cannot win; the port sets the
  individual switches instead. Expect this from any core that bundles its
  dependencies.

- **No PSF metadata reader is registered.** `MetadataReadFn` takes only a `Url`
  and `readPsfTags()` needs a registry to resolve through. With `open()` now
  cheap there is nothing left to win, so the reader contract stays as it is.
