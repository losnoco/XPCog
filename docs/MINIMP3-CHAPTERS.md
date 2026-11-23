# MP3 Chapters Without FFmpeg — Implementation Plan

The follow-up recorded in `docs/CHAPTERS.md`, de-risked by the investigation
in `docs/MINIMP3-CHAPTER-PROBE.md` (2026-11-22): minimp3-ex seeking is
sample-exact on CBR and VBR alike, the ex layer applies LAME gapless
natively, and the lazy index costs milliseconds. **Status: landed in 4.4.0**,
branch `mp3-chapters`. This file is the record of the work, like
`docs/CHAPTERS.md`; the probe's measurements are in
`docs/MINIMP3-CHAPTER-PROBE.md`.

## Goal and scope

In builds without FFmpeg (`XPCOG_WITH_FFMPEG=OFF`, the option's default),
chaptered MP3s currently neither expand into rows nor play as chapters,
while the tag editor still writes them. Close that gap for the MPEG audio
family minimp3 decodes (mp3, mp2, mpa, m2a): a CHAP/CTOC reader, a chapter
expander, and span playback in `Mp3Decoder`.

**Full builds do not change.** minimp3 keeps refusing fragment URLs when
FFmpeg is built, so FFmpeg plays chapters exactly as shipped and tested;
`FFmpegContainer` (0.5) keeps winning mp3 expansion over the new container
(0.4). M4A/Matroska chapters stay FFmpeg's in every build.

## Settled decisions

### Parser — a sibling, not an extension of the narrow one

`codecs::parseId3v2Chapters(std::span<const std::byte>, ChapterList&)` in
`codecs/common/Id3v2.{hpp,cpp}`, beside `parseId3v2`. The narrow parser's
charter (text frames and comments for HLS timed tags) stays; the sibling
reuses its private machinery — `id3v2TagLength`, the v2.2/2.3/2.4 frame
walk, syncsafe-with-fallback sizes, extended-header skip, global
unsynchronisation, and the text-encoding decode for the embedded TIT2.

- **CHAP**: element ID (null-terminated Latin-1 bytes, kept opaque like
  `TagLibChapters` does), start/end ms (big-endian 32), offsets ignored,
  embedded frames walked for TIT2 only.
- **CTOC**: element ID, flags byte — **top-level = 0x02, ordered = 0x01**
  (verified against TagLib's renderer) — entry count, child element IDs;
  embedded frames after the children skipped.
- **Order**: the top-level *ordered* CTOC's child order when one resolves,
  else stable sort by start — `readID3v2Chapters`' rule exactly, so the two
  readers agree by construction.
- CHAP/CTOC are 4-character IDs: v2.3+ only. A v2.2 tag yields no chapters,
  which is correct — the frames do not exist there.
- Malformed input degrades like the narrow parser: a truncated frame stops
  the walk, what was read is kept.

### Decoder — FFmpegDecoder's span shape, in ex sample space

`Mp3Decoder` gains `chapterStart_`/`chapterEnd_` (per-channel frames) and a
`metadata()` override, mirroring `FFmpegDecoder`'s
`startFrames_`/`endFrames_`/`subsongIndex_`:

- Fragment parse: decimal index via `from_chars`; anything else refuses.
  Streams refuse fragments too — chapters need seeking.
- **The tag bytes are already read at open.** `applyItunesGapless` reads
  the whole ID3v2 tag for `iTunSMPB`; lift that read into `openSeekable`
  and pass the span to both `applyItunesGapless` (refactored to take bytes,
  behaviour byte-identical — the existing gapless tests are the proof) and
  `parseId3v2Chapters`. No second read.
- ms→frames at `info_.hz`, rounded to nearest (`(ms * hz + 500) / 1000`,
  `av_rescale_q`'s convention) so spans agree with FFmpeg's within a frame.
  Chapter setup happens after the opening frame decode (hz known), then
  seeks to the chapter start and drops the opening frame.
- `seek(frame)` targets `(chapterStart_ + frame) * channels` in ex space;
  `readAudio` clamps at `chapterEnd_`; `totalFrames_` is the span;
  properties report the span's duration.
- **Gapless is inherited, not implemented**: ex sample 0 is the first real
  audio sample (Xing/Info delay applied by the ex layer; `iTunSMPB` by the
  existing override of the same fields). Chapter 0 at ms 0 lands there.
- `metadata()` answers the chapter's TIT2 as `title` and the file's TIT2 as
  `album` — FFmpeg's harvest rule, so `Scanner::readMetadata`'s merge gives
  chapter rows the same shape in both builds (TagLib's reader already
  retitles the chaptered file's title to album).

### Build gating — one define

`xpcog_add_codec(minimp3 ... DEFINES
XPCOG_WITH_FFMPEG=$<BOOL:${XPCOG_WITH_FFMPEG}>)` (the macro takes `DEFINES`,
PRIVATE). The fragment refusal becomes `#if XPCOG_WITH_FFMPEG` refuse
`#else` accept numeric `#endif`. No registry introspection, no runtime
branch on what is registered.

### Container — unconditional, below FFmpeg

A chapter expander in `codecs/minimp3/` (same directory, shares the
parser), registered at **0.4** — below `FFmpegContainer`'s 0.5 — claiming
the decoder's four extensions. `expand`: refuse non-seekable sources;
read the tag head through `ISource`, `id3v2TagLength`, read the tag,
`parseId3v2Chapters`; more than one chapter → `#0..#N-1` in the reader's
order, else `{url}` — `expandTune`'s threshold and shape.

Registered unconditionally: where FFmpegContainer claims the extension it
wins and nothing changes; where it does not, expansion is strictly
additive. The rows are identical to FFmpeg's by construction (same CHAP
data, same order rule), which the agreement tests prove.

### What stays out

MP1/MP2-specific quirks beyond the shared mechanism (the sample arithmetic
is layer-independent); chapter writing from the decoder side (TagLib owns
writes); HLS/stream chapters (no seeking); any change to full-build
behaviour.

## Phases

Branch `mp3-chapters` off main. **MINOR bump 4.3.0 → 4.4.0 in the first
code commit** (a user-visible feature in minimal builds). Each phase: code
+ tests together, build + ctest green in *both* configurations before
moving on.

### Phase 1 — the parser sibling

- `parseId3v2Chapters` in `codecs/common/Id3v2.{hpp,cpp}`; charter comment
  updated to name the sibling and its purpose. `parseId3v2` itself stays
  byte-identical — the sibling reuses its private helpers, not its loop.
- Chapter cases extend `tests/codecs/test_id3v2.cpp` (both builds), whose
  `TagBuilder` already builds every version and encoding — a new file
  would have duplicated it. Hand-built fixtures for CTOC order vs start
  order, unordered/nested tables, missing children, duplicate element IDs,
  every title encoding, truncated CHAP/CTOC, unsynchronised tags, v2.2
  yielding nothing; plus fixtures written two ways — the registry's
  TagLib writer (v2.3 and v2.4) and ffmpeg's ffmetadata muxer (both
  versions) — so the parser reads both implementations' output.
- Version bump here.

### Phase 2 — seek-accuracy tests (independent hardening)

- Promote the probe's method to `tests/codecs/test_mp3_seek_accuracy.cpp`
  (both builds): sequential-decode reference, correlation over ±128
  samples, targets at frame boundaries ±1 / chapter-style boundaries /
  arbitrary mid-file / near-end, CBR and VBR with and without Xing, mono
  and stereo. Assert zero positional error; keep a content tolerance for
  the reservoir transient (0 on clean LAME fixtures, the existing seek
  test's 0.1 for the wild).
- This phase stands alone: it hardens today's decoder regardless of the
  rest.

### Phase 3 — decoder spans + the minimal build

- `linux-minimal` preset: inherits the debug shape, `XPCOG_WITH_FFMPEG=OFF`,
  manifest features restated without ffmpeg (the `linux-headless`
  precedent for restating features).
- The `DEFINES` gate; `applyItunesGapless` lifted to take bytes; chapter
  setup, span clamps, `metadata()`.
- `tests/codecs/test_mp3_chapters.cpp`, registered under the minimp3 gate;
  the universal cases (whole-file playback, sibling-vs-TagLib agreement)
  run in every build, the fragment-acceptance cases compile only when
  `XPCOG_WITH_FFMPEG=0`: a chaptered fixture's `#1` opens on its span with
  its title; chapter 0 starts at the first real sample (compare against
  the sequential reference — the gapless proof); seeking within a chapter
  lands inside it; non-numeric fragments and streams refuse; removing
  chapters returns the file to whole-file playback.
- Existing gapless and seek tests stay green in both builds.

**As built (2026-11-22):** the minimal configuration had never been
exercised, and building it exposed ~16 pre-existing cases whose fixtures
only FFmpeg can decode — WAV playback is FFmpeg's in this tree, so the
DSP/equaliser, gapless-advance, scanner-artwork, HLS and vgmstream cases
that lean on `toneWav()`/`writeWav()` cannot run without it. Those cases
and their helpers are now `#if XPCOG_WITH_FFMPEG`-gated (test-only; the
full build is byte-identical), as are the five chapter-tagging session
cases and the GTK chapter smoke case that assert FFmpeg expansion —
**revisit those six in phase 4**: with the minimp3 container listing rows,
they may run in the minimal build too. Every test target gets the
`XPCOG_WITH_FFMPEG` define (one foreach, end of tests/CMakeLists.txt).
Local verification uses a system-library tree beside the preset:
`cmake --preset linux-repo-debug -B build/linux-minimal-local
-DXPCOG_WITH_FFMPEG=OFF` — the `linux-minimal` preset itself is the
vcpkg/CI shape. Both configurations green: 1103/1103 full, 1069/1069
minimal, zero warnings.

### Phase 4 — the container + agreement + CI

- The 0.4 expander; registration from the minimp3 registrar.
- Agreement cases in `tests/codecs/test_chapters_ffmpeg.cpp` (FFmpeg
  builds): for fixtures written by both implementations,
  `parseId3v2Chapters`' order/count equals `FFmpegContainer`'s expansion;
  spans agree within a frame.
- A minimal-build CI job (configure `linux-minimal`, build, ctest) beside
  the existing jobs — the configuration this feature exists for must not
  rot untested.
- `test_mp3_chapters.cpp` already serves both builds (phase 3): universal
  cases everywhere, acceptance cases `#if`-gated. Revisit the six gated
  chapter-tagging session/GTK cases here — the container may let them run
  in the minimal build too.

**As built (2026-11-22):** the expander lives in `Mp3Decoder.cpp` beside the
decoder and reuses its four-extension array; registered unconditionally at
0.4. The six chapter-tagging cases (five session, one GTK) are un-gated and
run in both builds. Two agreement cases compare `parseId3v2Chapters` against
`FFmpegContainer`'s expansion on TagLib- and ffmpeg-written fixtures: same
count, same order, each chapter's start and every interior span within a
frame. The *last* chapter's tail is deliberately not asserted to the frame —
it runs to the end of the audio, and the two decoders measure that end
differently (minimp3 trims the gapless padding, FFmpeg reports the raw
length), so the check walks the chapter boundaries cumulatively instead.
Both configurations green: 1105/1105 full, 1074/1074 minimal, zero warnings.

One pre-existing flake surfaced, unrelated to this work: the GTK chapter
case can abort with `Resource deadlock avoided` when run in isolation —
`ScanTask::~ScanTask` joins its own worker thread when the scan-completion
callback runs inline on it (`core/src/library/ScanTask.cpp`,
`uicore/src/Session.cpp`). It reproduces with the new container disabled and
passes in the full-suite run; left for its own fix, outside this path.

### Phase 5 — docs and the record

- `docs/FEATURES.md`: the chapter section notes MP3 chapters play without
  FFmpeg too (the minimal-build story), gapless chapter 0.
- `docs/PORTING.md`: a note under the codec survey — a deliberate
  divergence from Cog (which refuses fragments unconditionally), why
  (FFmpeg-less builds), and the gating.
- Fold the probe note's findings into this file's record; commit it.
- Manual E2E in a minimal build: the `~/Music/XPCog-Chapter-E2E` fixtures,
  the same walkthrough as the parity effort's.

**As built (2026-11-22):** `docs/FEATURES.md`'s chapter section notes MP3
chapters play in every build and gapless chapter 0; `docs/PORTING.md`'s
minimp3 survey records the deliberate divergence from Cog and its gating;
`docs/CHAPTERS.md`'s follow-up section is marked landed and points here.
This file and the probe note are committed as the record. The manual E2E
walkthrough in a minimal build is the one remaining step, done by hand
against the `~/Music/XPCog-Chapter-E2E` fixtures.

## Verification

- Both configurations, every phase: `linux-repo-debug` (full) and
  `linux-minimal` (no FFmpeg), build + ctest, read the skip counts.
- The agreement tests are the cross-build proof: same fixtures, same rows,
  same spans, two independent implementations.
- Windows: the gating is a compile definition and the parser is
  toolkit-free; CI's Windows job covers the full build, and the minimal
  configuration is Linux-shaped (the VM pass handles Windows breakages
  after push, per the standing decision).

## Risks and notes

- **Corrupt files**: the reservoir fill breaks early at unreadable side
  info; position stays exact (index arithmetic), content may transient.
  The accuracy tests carry a tolerance for exactly this; do not assert
  bit-exactness on non-LAME fixtures.
- **Rounding**: nearest-ms→frame matches `av_rescale_q`; agreement tests
  assert within one frame, not equality — the two implementations'
  chapter tables can differ by a sample at a boundary without anyone
  hearing it.
- **The opening frame**: chapter setup seeks after the format-proving
  read; the discarded frame must not leak into `framesReady_` or the
  stream timestamp.
- **`iTunSMPB` + chapters together**: an iTunes-ripped audiobook exercises
  both overrides of the ex fields; the span is measured in the same
  post-padding space, so it composes — test it explicitly.
- **Priority record**: minimp3's decoder is 2.0 (the CHAPTERS.md follow-up
  text says 0.8 — wrong); FFmpeg's decoder and container are 0.5. The new
  container is 0.4.
