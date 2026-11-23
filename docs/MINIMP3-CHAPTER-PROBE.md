# minimp3 chapter reading — investigation

The follow-up recorded in `docs/CHAPTERS.md` ("Considered: a chapter reader
in minimp3"), led by its prerequisite: measuring how sample-accurate
minimp3-ex's seeking really is. Investigated 2026-11-22; no project code
changed. The measurement harness is scratch (`/tmp/minimp3-chapter-probe/`,
ephemeral — the method below is enough to recreate it, and the implementation
phase would put proper cases in `tests/codecs`).

## The measurement

A scratch probe compiled against the vendored minimp3 with the decoder's own
configuration (`MINIMP3_FLOAT_OUTPUT`, `MINIMP3_NO_STDIO`). For each fixture:
decode sequentially from sample 0 as a reference; then for each target T,
`mp3dec_ex_seek(T)` and read a window, and cross-correlate against the
reference over ±128 samples to find where the decoder actually landed.
Positional error is that shift; the content transient is the worst sample
difference over the first two frames, before and after alignment.

Fixtures: 60 s of three summed sines (220/587/1234 Hz), mono and stereo,
encoded by libmp3lame four ways — CBR 128k and VBR `-qscale:a 2`, each with
and without the Xing/Info tag (`-write_xing 0`) — plus a 10-minute VBR file
for the index-build cost. Targets: 0, 1, frame boundaries ±1 (1151/1152/1153),
one second, two-thirds through, an arbitrary mid-file sample, near the end,
and a chapter-style 45 s boundary (sample 1984500).

## Results

| Fixture | Worst positional error | Worst transient (unaligned) | Aligned residual |
| --- | --- | --- | --- |
| CBR + Xing | **0 samples** | 0.0 | 0.0 |
| CBR, no Xing | **0 samples** | 0.0 | 0.0 |
| VBR + Xing | **0 samples** | 0.0 | 0.0 |
| VBR, no Xing | **0 samples** | 0.0 | 0.0 |
| Stereo VBR + Xing | **0 samples** | 0.0 | 0.0 |

Every target, every fixture: the decoder lands on the exact sample, and on
these clean LAME encodes the post-seek audio is bit-identical to the
sequential decode — the bit-reservoir transient the existing seek test
tolerates (`worst < 0.1`) did not appear at all.

Gapless and index cost, from the same runs:

- `start_delay` 1105 mono / 2210 stereo (LAME's delay × channels),
  `detected_samples` exactly 60.000 s — **the ex layer applies the Xing/Info
  delay and padding natively**, and measures seek positions from the first
  *real* audio sample. Chapter 0 at sample 0 is gapless-correct by
  construction; no extra work. (iTunes' `iTunSMPB` convention is already
  handled by `Mp3Decoder::applyItunesGapless`, which overrides the same three
  ex fields — a chapter span measured in ex sample space inherits both.)
- First seek on the 10-minute VBR file: **2.0 ms** (the lazy index is a
  header scan, not a decode); warm seeks ~0. Open without a Xing tag scans
  up front: 0.2–0.3 ms for 60 s, so a few ms for an audiobook. Negligible.

Why it is exact: the index stores per-frame `{offset, sample}` with sample
counts in exact 1152×channels steps; seek binary-searches it, backs up
`MINIMP3_PREDECODE_FRAMES` (2), walks back further until 511 bytes of bit
reservoir are accounted for, then sets `to_skip = position − frame.sample`.
The arithmetic is independent of the audio content, so **position stays exact
even where the reservoir fill breaks early** (corrupt or truncated frames —
the loop stops at the first unreadable side info). Content transients are
possible on such files; positional error is not.

Caveats: clean LAME encodes of synthetic signals only — real-world rips
(other encoders, free format, truncation) are the implementation phase's
test matrix, not this probe's claim. The probe used `mp3dec_ex_open_buf`;
the decoder uses `mp3dec_ex_open_cb` over `ISource` — the index and seek
code is shared (`mp3dec_load_index` is the same callback), the I/O layer
adds syscalls, not arithmetic.

## What the code survey found

- **The tag bytes are already in hand.** `Mp3Decoder::openSeekable` reads
  the whole ID3v2 tag before minimp3 sees the file (for `iTunSMPB`) and runs
  `codecs::parseId3v2` over it. A chapter parse rides the same buffer — no
  second read.
- **The narrow parser's charter stays.** `codecs/common/Id3v2.hpp` is
  deliberately text-frames-and-comments (HLS timed tags). Chapters belong
  in a sibling entry point over the same frame walk — `parseId3v2Chapters`
  beside `parseId3v2`, reusing `id3v2TagLength` and the v2.2/2.3/2.4 size
  handling: CHAP (element ID, four big-endian times/offsets, embedded TIT2)
  and CTOC (flags, child order). ~150 lines.
- **The span shape is FFmpegDecoder's.** `startFrames_`/`endFrames_`/
  `subsongIndex_`: seek adds the start, read clamps at the end, properties
  report the span, metadata harvests the chapter's own tags. The minimp3
  version converts CHAP milliseconds to frames at `info_.hz` and mirrors
  it; `totalFrames_` becomes the span, `framePos_` stays chapter-relative,
  and a `metadata()` override (the decoder has none today) answers the
  chapter's TIT2.
- **Priorities, corrected.** The plan's follow-up text says minimp3 0.8;
  it is actually **2.0** (`Mp3Decoder.cpp` registrar), FFmpeg's decoder and
  container both 0.5. The gating consequence is unchanged: when FFmpeg is
  built, minimp3 must keep refusing fragments so FFmpeg plays chapters as
  shipped and tested; when it is not, minimp3 accepts `#N`.
- **Build gating is one define.** `xpcog_add_codec` takes `DEFINES`
  (PRIVATE); pass `XPCOG_WITH_FFMPEG=$<BOOL:${XPCOG_WITH_FFMPEG}>` to the
  minimp3 target and `#if` the fragment refusal. No registry introspection.
- **The expander is only needed for FFmpeg-less builds.** With FFmpeg,
  `FFmpegContainer` (0.5) already lists the rows. Without it, a small
  container claiming mp3/mp2/mpa/m2a at 0.4 reads CHAP/CTOC through the
  sibling parser and lists `#0..#N-1` in CTOC/start order — identical rows
  to FFmpeg's, so the two configurations agree by construction. Register
  it unconditionally at 0.4 and FFmpeg's expansion wins when present; no
  build gate needed on the container itself.

## Refined shape and cost

The measurement collapses the risky part: span logic is arithmetic on
sample counts, chapter 0 is gapless-native, and seeking costs microseconds.
What remains is plumbing:

1. `parseId3v2Chapters` sibling (~150 lines) + unit tests against
   TagLib-written and ffmpeg-written fixtures.
2. `Mp3Decoder` chapter hooks (~150 lines): fragment parse, ms→frames,
   span clamps, `metadata()`; the refusal becomes build-gated.
3. A minimp3 chapter container (~80 lines) at priority 0.4.
4. Tests in both build configurations: expansion agreement with
   `FFmpegContainer` where both exist, span playback, chapter titles,
   gapless chapter 0, and the corrupt-file transient tolerance the existing
   seek test already models.

Roughly 400–600 lines with tests. The recommendation stands — its own
phased follow-up, scoped to "MP3 chapters without FFmpeg", not part of any
parity effort — but it is de-risked: the prerequisite investigation found
no accuracy obstacle, and one genuine upside over FFmpeg's chapter playback
(sample-accurate, gapless-correct chapter 0 on LAME-encoded files).
