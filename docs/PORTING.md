# Porting Cog to Qt: plan, progress and decisions

This is the working document for the port. The README describes what XPCog *is*;
this describes what has been done, what is left, and why each structural choice was
made — including the ones that look arbitrary until you hit the thing they avoid.

Upstream Cog is assumed to be checked out alongside for reference. File references
like `Audio/Chain/ChunkList.m` are paths inside Cog's tree.

---

## What the survey established

| Layer | Live LOC | Verdict |
|---|---|---|
| `Audio/` first-party | 16.1k Obj-C | Rewrite in C++ — all vDSP / Mach / ASBD |
| `Audio/ThirdParty/` | 6.7k C/C++ | Ports nearly as-is |
| UI / app layer | ~17.4k (a further ~5.5k on disk is **dead**, in no `.pbxproj`) | Full rewrite |
| XIBs | 5.7k lines XML, 190 bindings, 165 IBOutlets, 225 IBActions | Hand-rewrite |
| `Plugins/` vendored | ~2.3M | Ships as-is; needs CMake builds |

Three findings shape the whole plan:

1. **`Audio/Plugin.h` is the key asset.** A narrow six-protocol contract that maps
   ~1:1 onto C++ abstract base classes. Preserving it is what makes decoder
   migration incremental rather than a rewrite per format.
2. **`ChunkList` is secretly the format-conversion engine**, not just a FIFO — 1,113
   lines doing DSD→PCM, DoP packing, integer→float, endian swap and HDCD. "Port the
   buffer class" is much bigger than it sounds.
3. **Apple lock-in is concentrated, not diffuse.** Output is 2 files; only
   `CoreAudioDecoder.m` (479 lines), MIDI's `AUPlayer.mm` (~900) and dead QuickTime
   code are hard plugin locks.

`SandboxBroker` (559 lines), `urlBookmark` and the `SandboxToken` entity exist solely
for the Mac App Store sandbox. They are **excised deliberately**, not translated —
but behind a no-op `IFileAccess` seam so the call sites survive.

---

## Structural decisions

### The Qt-free rule enforces itself

`xpcog-core` never links Qt. This is not a style preference: the scanner, library,
playlist model and engine all have to work without a `QCoreApplication` so the CLI
and the tests can exercise them headlessly in CI on all three platforms.

The rule is enforced structurally rather than by review — `xpcog-cli` links no Qt at
all, so a `QtCore` leak breaks that target immediately. `cmake/CheckNoQt.cmake`
reports it earlier with a clearer message.

**Consequence worth knowing:** core cannot use `emit`, `slots`, `signals` or
`foreach` as identifiers. They are Qt macros, and although core does not include Qt,
the *application* includes both. `xpcog::Signal::publish()` is named that way for
exactly this reason.

### Codecs register at compile time

The obvious `static Registrar<FlacDecoder> reg_;` per codec **silently does not
work** with static libraries: the linker only pulls an archive member that resolves
an undefined symbol, so the object file is never extracted and the codec vanishes at
runtime. `--whole-archive` / `-force_load` / `/WHOLEARCHIVE:` are per-linker, bloat
the binary, and fight `-dead_strip` and LTO.

Instead `xpcog_add_codec(NAME flac REGISTER xpcog_register_flac …)` appends to a
CMake global property, and `codecs/CMakeLists.txt` generates a `RegisterAll.cpp` that
calls every registrar in order. Deterministic, breakpointable, no linker flags,
`-DXPCOG_WITH_X=OFF` for free, and single-codec registries in tests. It scales
identically at 8 codecs and at 35.

### Persistence is SQLite in core, not QtSql

Cog has precedent: `Utils/SQLiteStore.m` is 2,247 lines of hand-rolled sqlite3. Core
takes the database path by injection; `xpcog-platform` supplies the per-OS location.

**No `metadataBlob`.** Cog stores an `NSKeyedArchiver` transformable and materialises
`spam`/`indexedSpam` derived properties purely to build a search haystack. A child
table `(entry_id, key, ordinal, value)` round-trips `MetadataMap` losslessly and
makes search an indexable SQL query instead of "deserialise 50k blobs per keystroke".

Dropped columns: `urlBookmark` and the `SandboxToken` entity (sandbox-only);
`deLeted`/`removed` (soft-delete flags deferring Core Data deletion — SQLite just
`DELETE`s); `spotlightLength`/`spotlightTrack`; `queued` (derived from
`queue_position >= 0`); `dbIndex`/`entryId` merged.

**Performance note:** preparing SQL statements per row is what a straightforward
implementation does and it costs more than the inserts. 50k entries went from 3.9 s
to well under a second purely by hoisting them out of the loop (`EntryWriter`).

### Settings are generated from one file

`core/settings/settings.def` is the single source of truth, harvested from Cog's 8
`registerDefaults:` sites plus a grep of the 69 files that read defaults. **The key
strings are identical to Cog's** so an existing plist imports verbatim.

Core reads settings by injection, not a global. Slightly more plumbing; it is exactly
what makes core unit-testable with per-test settings.

### Real-time safety is stricter than Cog

Cog's callback (`Audio/Output/OutputCoreAudio.m:877`) takes an `NSLock` and enters an
`@autoreleasepool` on the real-time thread. XPCog's does only `ring.read()`, an
atomic gain multiply and a tail `memset`. No lock, no allocation, no `std::function`,
no logging. A feeder thread does the decoding.

`BUFFER_SIZE = 1 MiB` and `CHUNK_SIZE = 16 KiB` keep Cog's values — they are tuned,
and changing them changes latency behaviour.

---

## Progress

### Done

- **M0** — CMake + vcpkg + Ninja, compile-time codec registration, Qt-free core.
- **M1a** — Value types (`Url`, `AudioFormat`, `AudioChunk`, `MetadataMap`,
  `TrackProperties`), `FileSource`, `FlacDecoder`, miniaudio output behind
  `IAudioOutput` with a lock-free SPSC ring and feeder thread from day one. FLAC
  decode is byte-exact against `flac -d`.
- **M1b** — `AudioEngine` with gapless handoff, verified sample-exact against
  separately-decoded references. Decoders: FLAC, Vorbis, Opus, MP3 (mpg123), WavPack,
  FFmpeg (AAC/ALAC/WMA/AC3/…). Containers: M3U, PLS, cue sheets.
- **M1c** — ReplayGain, `Settings`, `AudioConverter` with soxr (a track at a
  different sample rate joins gaplessly), HDCD gated to Red Book format and verified
  bit-transparent on material carrying no codes.
- **M2** — `xpcog::Playlist` (canonical order, shuffle/repeat/queue, stable ids),
  SQLite `Library`, `Scanner`, `PluginCache`, TagLib tag reading, and playlist files:
  M3U, PLS, XSPF and Cog's own XML — which is an Apple property list, so core carries
  a small plist reader and writer.
- **M3** — The Qt application. `ActionRegistry` + declarative menu table,
  `MainWindow`, playlist model/proxy with drag-and-drop, `PlaybackController`
  bridging the engine's feeder thread to Qt signals, transport with a
  click-to-position seek bar, `PreferencesDialog`, `FileTree`, `QSettingsStore`,
  About dialogs, playlist export, undo for every playlist edit, folder scans on a
  worker thread, macOS media keys and Now Playing, i18n scaffolding, and a `deploy`
  target producing a signed self-contained bundle.

### Next: M4 — the DSP chain

`DSPNode` base, equaliser (hand-written cascaded biquad DF2T replacing
`vDSP_biquadm`), fader, downmix, rubberband, signalsmith, FreeSurround.

*Verify:* magnitude response against a reference impulse; A/B against Cog. Capture
golden reference output from Cog **before** replacing each kernel — numeric drift
from removing vDSP is silent otherwise.

LPC extrapolation is already vendored but deliberately not built: the converter keeps
soxr's delay line continuous, so chunk edges already are. It earns its place at the
first block after a seek, which is M4's work.

### Then

- **M5** — Visualization (pffft + QML islands), mini player, Windows SMTC (C++/WinRT),
  Linux MPRIS (QtDBus), `QLocalServer` single-instance, tray icon.
- **M6** — Breadth: archive/HTTP sources (libcurl, to keep sources Qt-free), DSD/DoP,
  the remaining ~27 decoders and ~32 vendored libraries (one `xpcog_add_codec()` plus
  one vcpkg overlay port each), `cogimport`, HRTF, Last.fm, global hotkeys.

Milestone 1's narrow format scope was a fastest-path-to-execution choice, not the
destination. The architecture is sized for full Cog parity throughout: if adding a
decoder ever requires a refactor, the design has failed.

---

## Deliberate differences from Cog

All of these are also documented at the call site.

**Behaviour**

- **Sort order is display-only and never changes playback order.** In Cog, shuffle
  and next/previous operate on `arrangedObjects` — the *sorted* order — a
  long-standing source of confusion. `xpcog::Playlist` owns canonical order and the
  proxy model is display-only.
- Playlist entries carry **stable ids** rather than indices, which removes Cog's
  `deLeted` / `nextEntryAfterDeleted` / negative-index machinery entirely.
- Album repeat over untagged files loops the untagged group instead of repeating the
  single track, which is what `RepeatMode::One` is for.
- Undo exists. Cog has none: `-delete:` removes the managed objects and the only way
  back is to re-add the files.

**Bugs fixed rather than reproduced**

- Cog's cue parser keeps one `artist` variable for the whole sheet and never resets
  it per track, so a single track-level `PERFORMER` mis-credits every following
  track.
- A non-`AUDIO` `TRACK` is skipped, but Cog still lets its `INDEX` create an entry,
  so a mixed-mode disc gains a bogus track that decodes to noise.
- `PluginCache` is keyed on mtime and size as well as URL, so retagging a file in
  another program invalidates it. Cog's URL-only key does not.
- iTunes Sound Check hex is parsed properly.
- Cog stores ReplayGain as scalar floats defaulting to 0, so it cannot tell "no album
  gain" from "0 dB". XPCog keeps absent absent.

**Implementation**

- FLAC emits native-endian.
- Matching sample rates bypass the resampler, so a same-rate file stays bit-exact.
- Metadata readers merge in priority order rather than stopping at the first.
- `AudioChunk` is move-only and read APIs take an out-parameter so storage recycles.
  Cog's `-readAudio` allocates a fresh object every 16 KiB.

**Not ported**

Mac App Store sandbox, AudioUnit MIDI instrument hosting, AppleScript, Spotlight,
the MCP server, Sparkle, Sentry. Cog also does **not** write tags —
`PluginController -putMetadataInURL:` is stubbed `return 0`, the facade has no
callers and `TagEditorController` is fully commented out. Tag writing is therefore a
new feature, not port work, and is deferred.

---

## Verification strategy

The CLI is the test harness, not an afterthought: `xpcog-cli` links no Qt, so it
exercises core headlessly in CI on all three platforms.

- **Per-codec conformance** — one asymmetric reference signal (440 Hz left, 660 Hz
  right, different amplitudes) catches swapped, duplicated and silent channels, which
  a duration or size check would miss. Adding a codec is one row in that table.
- **Gapless seam** — sample-exactness against separately-decoded references, waveform
  continuity, notification ordering, and a three-track case where a per-seam
  off-by-one accumulates rather than cancels. Plus a format-changing seam
  (44.1 → 48 kHz), which is where it will actually break.
- **Resampler null test** — 44100 → 44100 must be bit-exact passthrough.
- **Architecture** — CI greps `core/` for `#include <Q`; `xpcog-cli` failing to link
  *is* the Qt-free test.

### Two things that bite

**Tests that build fixtures skip silently when their encoder is missing.** Sixteen
tests shell out to `flac`, `oggenc`, `opusenc`, `lame`, `wavpack` or `ffmpeg`. A skip
is not a failure, so CI reported "100% tests passed out of 178" for months while
gapless, seeking, cue spans and tag reading went unrun on every platform. CI now
installs the encoders. **Check the skip count, not just the pass rate.**

**`OfflineOutput` drains at maximum speed.** Any bug whose symptom is measured in
wall-clock seconds — a stall, a delayed position update, a buffer that takes time to
drain — collapses to nothing under it and cannot be regression-tested that way. This
produced two convincing-looking tests for the seek-position fix that both passed with
the bug deliberately restored.

Which leads to the general rule, and the most useful habit in this project:

> **Before claiming a fix is tested, put the bug back and confirm the test fails.**

It has caught worthless tests repeatedly — including one in the i18n work that
asserted a property Qt provides rather than the one the code was responsible for.

---

## Known gaps

- Windows CI installs no encoders, so those sixteen tests still skip there.
  Chocolatey has no dependable packages for them, and the decoders they exercise are
  not platform-specific — what the Windows job catches is compiling, linking and path
  handling.
- `populateMenuBar()`'s translation lookup is not covered by a test: it needs a
  `QMenuBar`, so a `QApplication` and a platform plugin, and the test binary has
  neither.
- The macOS Now Playing integration is verified by hand, not by test.
