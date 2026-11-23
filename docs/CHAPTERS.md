# Chapter Tagging Plan

Multi-chapter tagging for the formats TagLib 2.3.2 can hold chapters in —
ID3v2 CHAP/CTOC (MP3, WAV, AIFF, DSF, DSDIFF, TrueAudio), MP4's two kinds
(QuickTime chapter track + Nero `chpl`), and Matroska chapter editions —
ported in behaviour from Cog's TagLib-based implementation
(`../cog/Plugins/TagLib/TagLibMetadataWriter.m`, `TagEditor/*.swift`,
`iOS/CogPluginsTests/TagWriterTests.swift`), which runs on the same TagLib
2.3.2. Full Cog parity is the scope: editor, editing from chapter rows,
playback fall-through, title→album retitling, and post-save row rebuild.

**Status (2026-11-22):** phases 1–6 are implemented and committed — 1–5 on
`main`, 6 on `chapter-tagging`, verified in a Windows VM (its MSVC fixes are
amended into the phase-6 commit). Phase 7 — this record, the Spanish
catalogue, and the FEATURES/PORTING notes — closes the effort. The plan
below is kept as written, like `docs/TAGEDITOR.md`; the manual E2E under
Verification is the one step left to a listener with an audiobook at hand.

## What exists today

- The tag editor shipped in 4.1.0: `MetadataMap` is the editor's document,
  the writer contract (`MetadataWriterDescriptor`, probe, capabilities) is
  in `core/include/xpcog/core/PluginRegistry.hpp`, `codecs/taglib/` reads
  and writes, `uicore/src/TagEditorModel` is the toolkit-free model, both
  frontends render it, `Session::editTags` writes replace/merge and reloads.
  **No chapters anywhere in the tagging path.**
- Chapter *playback* half-exists: `FFmpegContainer` (`expandTune`,
  `codecs/ffmpeg/FFmpegDecoder.cpp`) expands `nb_chapters > 1` into `#N`
  URLs for mp3/m4a/mka/wav/aiff/tta/… and `FFmpegDecoder` plays the chapter
  span, harvesting the chapter's title and the global title as album.
  Cog's container has the same `> 1` threshold — no change needed there.
- **Gap:** `codecs/minimp3/Mp3Decoder.cpp::open` ignores fragments, so
  `book.mp3#2` plays the *whole file* through minimp3. Cog's MP3Decoder
  refuses fragment URLs precisely so FFmpeg takes them
  (`Plugins/minimp3/MP3Decoder.m:41`).
- **Gap:** `Session::tagEditorSources`/`editTags` and both frontends'
  `canEditTags` reject every fragment URL, so a chapter row cannot be
  edited at all. Cog maps a chapter row to its file and edits that
  (`TagSaver.tagFileURL`), rejecting only cue-sheet tracks.
- **Gap:** `Session::applyReloadedEntries` matches rows by exact URL and
  deliberately drops changed expansions — after a chapter edit the row
  *set* goes stale. Cog compares the row set against the file's new
  expansion and offers to rebuild it (`TagSaver.rowsDiffer`,
  `TagEditorWindowController.followChapters`,
  `PlaylistController.replaceEntries:withRowsOfURL:inPlaylist:`).
- TagLib: system 2.3.2 here; vcpkg baseline provides **2.3.1** (identical
  chapter API; 2.3.2 is hardening fixes). MP4 chapters need ≥ 2.3,
  Matroska ≥ 2.2, CHAP/CTOC exist since 1.12. The system floor is
  currently 2.0 (`cmake/XPCogSystemDeps.cmake`).

## Settled decisions

### Chapter data model — a typed side channel, not a MetadataMap value

New header `core/include/xpcog/core/Chapters.hpp`:

```cpp
struct Chapter {
    std::uint64_t startMs = 0;
    std::uint64_t endMs   = 0;
    std::string   title;
    /// Opaque identity kept across saves: the ID3v2 element ID's bytes
    /// (Latin-1 text) or the Matroska ChapterUID in decimal. Empty means
    /// "mint one on write". MP4 chapters have none.
    std::string   id;
    friend bool operator==(const Chapter&, const Chapter&) = default;
};
using ChapterList = std::vector<Chapter>;
```

`MetadataMap` stays exactly strings-or-bytes. Chapters never enter it, so
they never reach `PlaylistEntry::applyMetadata`, `entry_tag`, the Scanner
cache, REST serialization or the CLI's tag output — matching Cog, where
chapters are read fresh from the file for the editor and are not playlist
metadata. Rejected alternatives: a third `MetaValue` alternative (leaks
file structure into every map consumer and the library), chapters encoded
as strings/bytes under a reserved key (stringly-typed, escaping problems),
changing `MetadataWriteFn`'s signature (MAJOR bump for no gain).

### Contract additions — all optional members, MINOR bump

In `PluginRegistry.hpp`:

- `MetadataCapabilities` gains `bool chapters = false;` **appended last**,
  so the existing five-value aggregate initializers (tests) stay valid.
- Reader side: `using MetadataReadChaptersFn = ChapterList (*)(const Url&);`
  and an optional `readChapters` member on `MetadataReaderDescriptor`.
  `PluginRegistry::readChapters(url)`: fragment → `{}`; the
  highest-priority claimant that has the function answers alone (the
  `metadataCapabilities` rule — one authority, no merge).
- Writer side: `using MetadataWriteChaptersFn = bool (*)(const Url&,
  const MetadataMap&, const ChapterList&, MetadataWriteOptions&);` and an
  optional `writeChapters` member on `MetadataWriterDescriptor`.
  `PluginRegistry::writeMetadata(url, tags, chapters, options)` overload;
  the existing three-arg form delegates to one internal implementation
  with no chapters. When chapters are requested, only writers with
  `writeChapters` are candidates; the probe gates as today.
- Semantics (Cog's): **no chapters argument → leave the file's chapters
  alone; an empty list → remove them all; a list → replace them.** One
  open, one save — tags, art and chapters land together.
  `MetadataWriteOptions` stays policy-plus-error, unchanged.

### Format matrix

| Container | Chapters | Mechanism |
| --- | --- | --- |
| MP3, WAV, AIFF, DSF, DSDIFF, TrueAudio | yes | ID3v2 CHAP + top-level ordered CTOC |
| MP4 / M4A / M4B | yes | QuickTime chapter track **and** Nero `chpl`, both written from one list |
| Matroska / MKA / MKV / WebM | yes | one default chapter edition |
| FLAC, Ogg family, APE, WavPack, MPC, ASF | no | TagLib has no chapter mechanism; probe says false, editor hides the section |

`Family` classification in `TagLibWriter.cpp` stays as it is — Matroska
and TrueAudio remain `conservative` for *tag* promises (this work probes
their chapters, not their key folding). Chapter support is its own
predicate: an ID3v2 tag handle is reachable (`id3v2TagOf`, covering the
six ID3v2 containers incl. TrueAudio's `ID3v2Tag(create)`), or the file is
`MP4::File` or `Matroska::File`. `caps.chapters` = that predicate.

### Read/write behaviour (ported from Cog, probe-verified against 2.3.2)

- **Read ID3v2:** CHAP frames in the top-level *ordered* CTOC's child
  order when one exists, else stable-sorted by start. Title from the
  embedded TIT2; `id` = element ID bytes.
- **Read Matroska:** default edition preferred (else first), hidden
  chapters skipped, ns→ms, title = first display string, `id` = UID in
  decimal.
- **Read MP4:** `qtChapters()` preferred, else `neroChapters()`;
  stable-sorted by start; ends derived — next chapter's start, last =
  `max(file length, start)`. Needs audio properties: the chapter-reading
  `FileRef` opens with `readAudioProperties=true` (the plain tag read
  stays `false`).
- **Write (all kinds):** stable-sort by start; `end[i] = start[i+1]`,
  last end = file length (clamped to ≥ start). The writer opens with
  `readAudioProperties=true` when a chapter list is requested.
- **Write ID3v2:** remove CTOC/CHAP; per chapter a `ChapterFrame` with
  embedded UTF-8 TIT2, ms times cast to `unsigned int` (32-bit ms ≈ 49
  days — clamp), byte offsets `0xFFFFFFFF` (times are what count);
  element IDs kept when non-empty, unique and Latin-1-representable,
  else minted `chp0`, `chp1`, …; one `TableOfContentsFrame("toc", …)`,
  top-level, ordered.
- **Write Matroska:** `file->chapters(!empty)`, `clear()`, one
  `ChapterEdition(list, /*default=*/true, /*hidden=*/false, uid)`;
  per chapter `Chapter(startNs, endNs, displays, uid)` with
  `Display(title, "eng")`; UID kept or minted non-zero random
  (`std::random_device` — Cog uses `arc4random_buf`). Verify the exact
  constructor signatures against the 2.3.2 headers while implementing.
- **Write MP4:** one `MP4::ChapterList` → `setQtChapters` +
  `setNeroChapters`, so Apple's players and everyone else see the same
  chapters. Empty list clears both.
- Chapters are applied after `setProperties`, before the existing
  `saveFile` (StripOthers/DoNotDuplicate semantics unchanged — CHAP/CTOC
  ride the ID3v2 block).

### Reader retitling (Cog parity)

`readWithTagLib`: when the file has chapters, its title names the whole
(an audiobook), so the `title` property is emitted as `album` — Cog's
`TagLibMetadataReader` does exactly this, overwriting any album value.
Presence check is cheap (`frameList("CHAP")` non-empty / editions
non-empty / qt-or-nero list non-empty) and runs on every metadata read,
as in Cog. Chapter rows then get their own title from FFmpeg's chapter
metadata merge in `Scanner::readMetadata`, which already harvests it.

### Editing from chapter rows

`tagFileUrl(entry)` (Session, and the same gate in both frontends'
`canEditTags`): local file required; no fragment → the URL; a fragment →
**reject `.cue` and `.flac`** (cue-sheet tracks — in XPCog those are the
only sources of fragments on those extensions: `expandFlacCue` and
`CueSheetPlugin`; FFmpeg finds no chapters in native FLAC), otherwise the
parent `url.withoutFragment()`. This is deliberately simpler than Cog's
`isCueSheetTrack` (which probes the base file for a `cuesheet` tag): the
extension rule is equivalent here because no other container fragments
those files. Document the difference in PORTING.md. Ogg-chain rows
(`.ogg#N`) edit the whole file's tags, as in Cog; no chapter section
(capabilities false), no highlight.

The editor opens on the parent file's tags; `TagEditorSource` gains
`ChapterList fileChapters` and `int chapterIndex = -1` (the row's
fragment, for highlighting the chapter it plays — shown only when
`caps.chapters`).

### Row rebuild after a chapter-changing save (Cog parity)

- `Session::editTags` gains an optional `const ChapterList* chapters =
  nullptr` parameter (additive). Dirty check: replace mode skips a file
  only when the map is unchanged **and** (no chapter payload or the
  file's current chapters equal it — compared as sorted
  `(startMs, title, id)` tuples, ends being derived).
- After a successful write that carried chapters, compare
  `registry_.expandContainer(parent)` against every playlist's rows of
  that file (URL == parent or parent#N). Different sets → collect the
  file; publish a new `chaptersChanged(std::vector<Url>)` signal once,
  after the writes. (One extra open per chapter-changed file; editTags is
  already synchronous I/O on the calling thread.)
- New setting, Cog's key verbatim:
  `XPCOG_SETTING(TagEditorChapterRows, std::string, "tagEditorChapterRows", "ask")`
  — `"ask" | "rebuild" | "keep"`.
- Frontends, on `chaptersChanged`: `rebuild` → rebuild now; `keep` →
  nothing; `ask` → a dialog (Cog's words): "The chapters of “X” changed."
  / "The chapters of N files changed.", body "Rebuild their rows in the
  playlists to match? Rows for the old chapters are replaced with rows
  for the new ones.", buttons **Rebuild Rows** / **Keep Rows**, and a
  "Do this every time" checkbox that writes the setting. GTK:
  `AdwAlertDialog` with the checkbox as extra child; WinUI:
  `ContentDialog` with a `CheckBox`. Present it after the tag dialog
  closes (dispatch the handler, don't stack dialogs mid-submit).
- `Session::rebuildChapterRows(files)`: per playlist, remove the file's
  old rows and insert its new expansion where the first one was — Cog's
  `replaceEntries:withRowsOfURL:`. Route the insert through the **scan
  queue** (a new `ScanRequest` rebuild mode carrying the file URL and
  target playlist): expansion + metadata reads then share the queue's
  PluginCache serialization, and the finished handler computes the row
  position live (avoiding stale indices when several files rebuild into
  one playlist). The shown playlist's insert goes through
  `commands_.insert` (undoable, as other structural edits); other
  playlists insert directly. Verify and test what happens when the
  *playing* row is among the replaced ones.
- A preferences picker for the setting (Cog has one): place it on the
  most fitting existing page at implementation time; the suppression
  checkbox writes the same key, so the picker is convenience, not the
  only path.

### TagLib floor — 2.3 (decided)

`cmake/XPCogSystemDeps.cmake`: `taglib >= 2.0` → `taglib >= 2.3`;
`vcpkg.json`'s taglib feature description follows ("at 2.3 or newer").
Distributions with older TagLib fall back to the vcpkg build (2.3.1 at
the current baseline — same chapter API), which is the designed
behaviour of `XPCOG_USE_SYSTEM_LIBS`. No `#if` version guards. No baseline
bump and no overlay port: 2.3.1 → 2.3.2 is hardening, not chapter API.

### Out of scope (unchanged stances)

REST chapter reads/writes (filesystem writes by network peers stay out,
`docs/REST.md`); chapter support for formats TagLib has no mechanism for;
undo for tag saves (a disk write is not undoable — the row rebuild is a
playlist edit and follows the existing structural-edit policy);
`xpcog-cli tag` (still the tag editor's tail, PORTING.md).

## Considered: a chapter reader in minimp3 (follow-up, not parity)

**Landed in 4.4.0** as the phased follow-up sketched below, on branch
`mp3-chapters`. `docs/MINIMP3-CHAPTERS.md` is the plan and record;
`docs/MINIMP3-CHAPTER-PROBE.md` is the seek-accuracy investigation that
de-risked it (zero-sample positional error, CBR and VBR alike). The notes here
are the original consideration, kept as the rationale.

Raised after Phase 3: Cog's minimp3 imports libid3tag, and XPCog's already
parses ID3v2, so could minimp3 read chapters and play the spans itself?

What the investigation found:

- XPCog's minimp3 parses ID3v2 through `codecs/common/Id3v2.hpp`, but only
  for `iTunSMPB` (iTunes gapless) — the same single purpose as Cog's
  libid3tag. That parser is deliberately narrow (text frames and comments,
  for HLS timed tags; CHAP/CTOC skipped), and minimp3 exposes no
  `metadata()` — MP3 tags come from TagLib. Cog's fragment refusal is
  deliberate design, not a gap libid3tag was meant to fill.
- The load-bearing fact: `XPCOG_WITH_FFMPEG` defaults **OFF**. In a minimal
  build there is no FFmpegContainer and no FFmpegDecoder, so chaptered MP3s
  neither expand into rows nor play as chapters — while the editor still
  writes them. That is the gap a minimp3 chapter path would close, for MP3
  only (M4A/Matroska spans stay FFmpeg's).
- Container answer: with FFmpeg built, `FFmpegContainer` already lists the
  rows — no new container needed for expansion. A minimp3 path needs its
  own expander only for FFmpeg-less builds: a small container reusing a
  CHAP/CTOC parse, registered below FFmpegContainer's priority so FFmpeg's
  expansion wins when present. The decoder's fragment acceptance is
  build-gated (refuse when `XPCOG_WITH_FFMPEG`, as Phase 3 shipped; accept
  `#N` and play the span otherwise), keeping full builds' tested behavior.
- One upside beyond minimal builds: minimp3 already applies the encoder
  delay/padding, so chapter 0 could start sample-accurately where FFmpeg's
  span may include the padding.
- **Seek accuracy is the prerequisite investigation.** Chapter spans are
  arbitrary millisecond positions, so the follow-up should start by
  measuring how sample-accurate minimp3-ex's seeking really is, CBR and
  VBR alike. The ground is promising: the decoder already opens with
  `MP3D_SEEK_TO_SAMPLE` and seeks in interleaved sample units, and
  minimp3-ex builds a per-frame index (from the Xing/Info TOC scan, or a
  full scan when there is no VBR tag) to land precisely. The existing
  "seeking an MP3 lands on the right audio" case proves positional
  accuracy on a CBR fixture, with a content tolerance (`worst < 0.1`)
  because MP3's bit reservoir makes a mid-file decode rebuild its first
  block from less history — a transient inherent to the format, which
  FFmpeg shares, not a positioning error. Extend that measurement to:
  VBR fixtures with and without a Xing TOC; arbitrary non-midpoint
  targets and chapter-relative offsets, not just `kFrames / 2`; the
  gapless-adjusted start of chapter 0; and the cost of the first seek's
  index scan on a long audiobook file (the index is lazy when a VBR tag
  gave the duration). If the positional error is zero samples across
  those, the "fiddly for VBR" span logic below reduces to arithmetic on
  sample counts.

Cost: a CHAP/CTOC parser with embedded frames (extending the narrow
parser bends its charter; a sibling over the same frame walk is cleaner),
ms→frame-boundary span logic with chapter-relative seeking (fiddly for
VBR), chapter-title metadata, and tests in both build configurations — a
third implementation of chapter semantics beside TagLib and FFmpeg, and a
divergence from Cog. Recommendation: keep it out of the parity effort; if
wanted, it is its own phased follow-up after Phase 7, scoped to "MP3
chapters without FFmpeg".

## Phases

Each phase: code + its tests in the same commit; build + ctest green
before moving on. Phases 1–4 are fully verifiable headless
(`linux-repo-debug` here; `linux-headless` shape elsewhere).

### Phase 1 — core contract (`core/`) + version bump

- `core/include/xpcog/core/Chapters.hpp` (new).
- `PluginRegistry.hpp`: `chapters` capability, the two function types,
  the two optional descriptor members, `readChapters` + the
  `writeMetadata` overload declarations.
- `PluginRegistry.cpp`: `readChapters` (capability-style selection),
  `writeMetadata` shared implementation with the chapters-aware
  candidate rule.
- `settings.def`: `TagEditorChapterRows` in the `--- tags ---` group.
- Floor bump: `XPCogSystemDeps.cmake`, `vcpkg.json` description.
- **MINOR bump 4.2.0 → 4.3.0** in this commit (`CMakeLists.txt`
  `project(VERSION)` + `vcpkg.json` `version`) — additive contract
  surface and a user-visible feature.
- Tests: `tests/core/test_plugin_registry.cpp` — selection rules with
  fake descriptors (claimant without `writeChapters` skipped when
  chapters requested; probe gating; fragment refusal; empty-list vs
  absent semantics at the contract level).
  `tests/uicore/test_settingeffect.cpp`: add `"tagEditorChapterRows"` to
  the known-keys list.

### Phase 2 — TagLib codec (`codecs/taglib/`)

- New translation unit `TagLibChapters.cpp` + codec-private
  `TagLibChapters.hpp`: `readTagLibChapters(FileRef&)`,
  `hasTagLibChapters(File*)`, `writeTagLibChapters(File*, ChapterList,
  lengthMs)` (normalize + dispatch), `tagLibSupportsChapters(File*)`,
  `id3v2TagOf(File*, create)`. Port Cog's functions to C++20/std
  (ByteVector element IDs as opaque byte strings, `std::stable_sort`,
  `std::random_device`). Add the source to `codecs/taglib/CMakeLists.txt`.
- `TagLibWriter.cpp`: shared write body taking `const ChapterList*`;
  `writeTagsWithTagLib` and a new `writeTagsWithChaptersWithTagLib` as
  thin wrappers; `readAudioProperties=true` when chapters requested;
  chapters applied after `setProperties`, before `saveFile`; probe sets
  `caps.chapters`.
- `TagLibReader.cpp`: implement `readChaptersWithTagLib` (properties on,
  for MP4 ends), register it on the reader descriptor; title→album
  retitling in `readWithTagLib` via `hasTagLibChapters`.
- Tests (`tests/codecs/test_taglib_writer.cpp`, `[tags][tagwriter]`,
  fixtures via the existing `makeFromWav`/ffmpeg helpers, silent skips —
  read the skip count):
  - capabilities: chapters true for mp3/m4a/mka/wav/aiff/tta, false for
    flac/ogg/opus/wv/ape.
  - round trip per format: out-of-order input → sorted by start; ends
    derived (next start; last == file length within tolerance); UTF-8
    titles.
  - identity: rename a middle chapter, write again → ids unchanged
    (mp3 element IDs, mka UIDs); other tags untouched by a merge write
    carrying only chapters.
  - empty list removes (mp3: no CHAP/CTOC read back; m4a: both kinds
    gone; mka: no editions); a write *without* chapters leaves existing
    ones alone.
  - retitling: `readMetadata` of a chaptered mp3 with TIT2 → album, no
    title.
  - FFmpeg agreement (where FFmpeg is built): `expandContainer` lists
    the same count/order; `readMetadata(file#1)` carries the chapter
    title; `registry.open(file#1)` properties span the chapter.

### Phase 3 — playback fall-through (`codecs/minimp3/`)

- `Mp3Decoder::open`: refuse when `source->url().fragment()` is
  non-empty (Cog's one-liner), so MultiDecoder falls through to FFmpeg.
- Tests: the Phase-2 FFmpeg-agreement cases now prove chapter playback
  for mp3; add an explicit case that a chaptered mp3's `#1` opens with
  the chapter's span and title while the whole file still plays.

### Phase 4 — model + session (`uicore/`)

- `TagEditorSource`: `fileChapters`, `chapterIndex`.
- `TagEditorModel`: chapter rows (single file, `caps.chapters`),
  `supportsChapters()`, `chapters()`, `setChapterStart/Title`,
  `addChapter()` (start = max + 1000 ms, first at 0), `removeChapter()`,
  `highlightedChapter()`, `changed()` includes chapter edits,
  `buildChapters()` → `std::optional<ChapterList>` (present only when
  supported and changed); static `timeText(ms)` /
  `millisecondsFrom(text)` ported from Cog (`h:mm:ss.mmm`, `m:ss`,
  `ss.s`; nullopt on garbage). Caps merge ANDs `chapters`.
- `Session`: `tagFileUrl` gate in `tagEditorSources`/`editTags` (dedup by
  parent URL, keep the first row's chapter index); `editTags` optional
  chapters parameter + chapter-aware dirty check + post-save expansion
  compare + `chaptersChanged` signal; `rebuildChapterRows` + the scan
  queue's rebuild mode.
- Both frontends' `canEditTags`: the same gate, probing the parent URL
  (GTK's `tagCaps_` cache keyed by parent).
- Tests: `tests/uicore/test_tageditor.cpp` (time helpers round-trip,
  add/remove/changed, `buildChapters` gating, batch never carries
  chapters, highlight, caps merge); `tests/uicore/test_session.cpp`
  (chapter-row gate: `book.mp3#2` editable, `album.cue#3` and
  `album.flac#01` refused; editTags with chapters writes and reloads;
  `chaptersChanged` fires when the expansion changes; rebuild replaces
  rows at the first old position, in every playlist).

### Phase 5 — GTK frontend (`app-gtk/`)

- `TagEditorDialog.cpp`: a Chapters group after the cover, built like
  the free-rows box (rebuildable `GtkBox` in an `AdwPreferencesGroup`,
  "Add Chapter" header suffix): per row an index label (accent colour
  when highlighted), a time entry (commit on activate/focus-out through
  the model's parser, reformat after), a title entry, a trash button;
  footer note "Each chapter ends where the next one starts, and the last
  where the file ends." Shown only when `model.supportsChapters()`.
  Submit passes `model.buildChapters()` to `editTags`.
- `chaptersChanged` handler: the ask/rebuild/keep flow above
  (`AdwAlertDialog`, suppression checkbox writes the setting).
- Preferences picker for `TagEditorChapterRows` (placement decided
  here).
- `xpcog-gtk-tests`: extend the tag-editor smoke coverage — chapters
  section on a chaptered fixture, add/remove a row, save.

### Phase 6 — WinUI frontend (`app-winui/`)

- `MainWindow.cpp`'s tag dialog: the same section, built like
  `freeContainer` (StackPanel rebuilt wholesale, head children kept),
  time/title TextBoxes, remove button, Add Chapter, footer note,
  highlight; submit passes `buildChapters()`.
- `chaptersChanged` handler: ContentDialog with Rebuild Rows / Keep
  Rows + "Do this every time" CheckBox; the preferences picker.
- `xpcog-winui-tests`: the same smoke coverage.

### Phase 7 — translations + docs

- `python tools/extract-messages.py` (WSL on this machine), fill
  `uicore/locale/es.po` for the new strings.
- `docs/FEATURES.md`: the tag editor entry gains chapters — the format
  matrix, both MP4 kinds, the row-rebuild choice.
- `docs/PORTING.md`: a note that this is new work inspired by Cog's
  chapter implementation, and the deliberate differences (extension-based
  cue gate vs Cog's cuesheet probe; Matroska/TrueAudio stay conservative
  for tag promises; MPEG saves AllTags where Cog saves ID3v2-only —
  pre-existing, chapters ride it). A *Where to pick up next* line for
  `xpcog-cli tag` chapter support.
- This plan file committed as the record.

## Verification

- `cmake --build --preset linux-repo-debug && ctest --preset linux-repo-debug`
  after every phase; read the **skip count** (ffmpeg-built fixtures).
- `./build/linux-repo-debug/bin/xpcog-tests "[tags]"` and `"[tageditor]"`
  for the focused runs; `xpcog-gtk-tests` under xvfb for Phase 5.
- Manual E2E once: an audiobook mp3/m4b — add chapters in the dialog,
  save, watch the rows rebuild, play `#N`, rename a chapter, save again,
  confirm identities and the other tags survived; check the file in
  another player (FFmpeg-based `ffprobe -show_chapters` agrees).
- Windows build + `xpcog-winui-tests`: not run locally during the phases.
  Breakages get drafted out in a Windows VM after the full set is pushed
  (decided 2026-11-20); CI covers the interim, and the WinUI changes mirror
  the GTK ones so the two cannot drift silently.

## Risks and implementation notes

- Matroska write styles (new in 2.3): the default save must keep the
  chapter edition — the round-trip test is the proof.
- MP4 chapter writes rewrite `moov`/`stco`: 2.3 fixed offset writing;
  test with a real multi-chapter m4a fixture, not a toy.
- ID3v2 CHAP times are 32-bit ms — clamp, don't wrap.
- `readAudioProperties=true` costs a parse on chapter reads/writes only;
  the plain scan path stays `false`.
- The rebuild's playing-row behaviour needs a test, not an assumption.
- `chaptersChanged` fires inside synchronous `editTags`; frontends must
  dispatch their dialogs so the tag dialog closes first.
