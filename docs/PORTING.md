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
reports it earlier with a clearer message. (The CLI is temporary; see the
verification section for what has to take over that job when it goes.)

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
`@autoreleasepool` on the real-time thread. XPCog's does `ring.read()`, a per-frame
multiply by the volume and the transport-fade gain, and a tail `memset`. No lock, no
allocation, no `std::function`, no logging. A feeder thread does the decoding.

The fade gain is the one thing that has been added to that list since, and only
because pause and stop cannot be faded anywhere else: the audio they fade is already
queued for the device (see M4 below). It is a bounded per-frame ramp toward an atomic
target, so the properties above still hold.

Both the volume and that ramp live in `TransportGain`, and that class exists because
they used to be written out twice — once in the device callback and once in
`OfflineOutput` — where the two copies disagreed. The device's skipped its multiply
whenever the ramp had *settled*, which is correct at unity and silent-turned-loud at
zero: a faded pause left level and target both at zero, so the next buffer went out
untouched, at full volume, and only started fading once `rampGain` made the two
differ again. The offline copy had the condition right, which is why no test could
reach the bug — the only implementation a test could see was the correct one.

Two lessons, both cheap to state and expensive to relearn. A real-time routine
duplicated for a test double is not a test double, it is a second implementation; and
an early-out on "nothing has changed" is not the same predicate as "nothing needs
doing". `TransportGain` is header-only for the inlining and has its own tests,
including one that pins the exact case that broke.

Cog's `BUFFER_SIZE = 1 MiB` and `CHUNK_SIZE = 16 KiB` are tuned values and are kept
— but M4 split where the buffering sits. `BUFFER_SIZE` is now the *pre*-DSP ring
inside the engine, which is where the depth belongs; the ring between the DSP chain
and the device is deliberately shallow (`1 << 14`, ~186 ms), because that number is
the latency of every DSP change. See the `AudioEngine` class comment.

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

### M4 — the DSP chain (complete bar FreeSurround)

**Done:** `DSPNode` and the equaliser, wired into the engine behind a two-stage
buffer.

Buffering deserves its own note, because getting it wrong was not obvious. The
chain first went in *behind* the ring, which is the arrangement that reads as
correct -- decode, convert, filter, hand to the device. But the ring is Cog's
BUFFER_SIZE, about three seconds, and its whole job is to absorb a feeder-thread
hiccup. Filtering ahead of three seconds of already-filtered audio means every
DSP change is inaudible for three seconds: moving an equaliser slider appeared to
do nothing for five, and it was reported as a bug rather than noticed here.

So the depth moved ahead of the chain and only a shallow ring follows it:

```
decoder -> converter -> preRing (deep, ~3 s) -> chain -> ring (~186 ms) -> device
```

Which is what Cog's per-node thread and `ChunkList` were for all along. The
threading dismissed above as following from Cog's engine shape is *also* what
keeps its DSP responsive, and replacing it with synchronous stages behind one deep
buffer quietly traded that away. The fix keeps synchronous stages -- one pump
thread for the whole chain rather than one per stage -- and measures 0.4 s from
slider to speaker, guarded by a test so re-deepening that ring cannot silently
undo it.

A side benefit: the chain now runs in exactly one place, so the four-call-sites
hazard below is gone -- no future path into the ring can forget to apply it.

`DSPNode` is deliberately not Cog's. Cog's is a threaded node — a thread, a
`ChunkList`, two semaphores and a recursive lock each, chained as
producer/consumer handoffs — which follows from an engine where every stage
between decoder and output is such a node. XPCog has one feeder thread writing
into a lock-free ring, so that shape would add a thread and a lock per stage to a
pipeline that already has the concurrency it needs. A node here is a synchronous
in-place transform and the chain is a sequence, which also makes each stage
testable by calling `process()` on a buffer.

The equaliser keeps Cog's filter exactly (31 AUGraphicEQ centres, Q 1.4, RBJ
peaking normalised by `a0`, Nyquist bands as identity sections so indices are
stable across sample rates) and Cog's settings keys, so a Cog plist carries a
user's curve across. It differs in two documented ways: double state where
`vDSP_biquadm` keeps single, and flat is *skipped* rather than run, which makes
0 dB bit-transparent by construction.

Cost, measured rather than assumed: all 31 bands on stereo 44.1 kHz is **0.31% of
one core**, 322× realtime, 35 ns/sample. The first stage with a real per-sample
cost on the feeder thread, and comfortably free.

**Also done:** the fader, which is what finally implements `enableFading` -- the
setting existed in `settings.def` and was read by nothing. A linear 200 ms ramp,
Cog's `fadeTimeMS` and Cog's shape (`vDSP_vrampmuladd` is linear too).

It hangs off `reset()` rather than having a transport API of its own: reset()
already means "the signal does not flow across this point", which is exactly when
a fade in is wanted, so no engine plumbing can introduce a discontinuity without
the fade following it.

**Where each fade lives is the thing to understand**, because they are not all in
the same place and cannot be.

A *seek* fade belongs in the chain: the seek discards the queue, so a crossfade
produced upstream is heard at once. *Pause* and *stop* are the opposite — the audio
they fade is already queued for the device, so fading it upstream would only take
effect after that queue drained, by which time the transport has stopped. Those
two therefore ramp at the output, next to the volume multiply that was already
there (`IAudioOutput::rampGain`). Getting this wrong is not subtle in use: pause
and stop simply cut, which is how they shipped until it was reported.

Pause also has to stay responsive. The fade must be *played* to be heard, so
`pause()` marks its intent and returns — the status flips immediately, the feeder
stops the device once the ramp has run. `stop()` does wait, briefly and with a
deadline, since it already blocks to join two threads.

Fading out across a seek is the interesting half, because the audio to fade out
has already been discarded by the time the chain sees anything new.
Cog retains it in a `FadedBuffer`; the same idea works here because the fader is
last in the chain, so everything it emits goes straight to the shallow ring and a
rolling copy of its last 200 ms *is* the audio a flush is about to drop.

Which *part* of that copy to use turned out to matter more than the copy itself.
The history spans `[lastEmitted - 200 ms, lastEmitted]`, but only its newest frames
are still queued; starting the tail at the oldest frame replays the difference, and
a replayed ~14 ms is a stutter, not a fade. It was dismissed here as inaudible and
then reported as "doesn't sound clean". The feeder now passes the exact discarded
frame count (`noteDiscardedFrames`, read before the flush, which is the last moment
it exists), the tail is taken from the newest end, and the ramp spans the tail so
both sides finish together.

The curve also departs from Cog's linear ramp. A seek lands on unrelated audio, so
the two sides are uncorrelated and their *powers* add: linear complements of 0.5
leave half the power, a 3 dB dip through the middle of every seek. Sine and cosine
legs hold the sum of squares at one. The cost, recorded rather than hidden, is that
identical audio either side — a very short seek — now swells up to 3 dB instead of
holding flat.

The cost is one property the fader used to have: to keep its history fed it must
see every block, so `active()` stays true whenever fading is enabled instead of
going false once a ramp lands. `process()` still leaves the samples untouched when
there is nothing to apply, so the output is bit-identical — what it no longer does
is let the chain skip the stage.

**And the downmix matrix**, which is *not* a `DSPNode` — a departure worth
recording. Cog's nodes pass `AudioChunk`s carrying their own format, so one can
change the channel count; a `DSPNode` here transforms a buffer in place at a fixed
format, and downmix changes the frame size by definition. So it lives where
channel geometry already changes, in `AudioConverter`, replacing the `fitChannels`
placeholder whose own comment had expected a `DSPDownmixNode`. The ratios are
Cog's constant for constant, including the ones that are magic numbers there too.

One thing that came out of testing it: **Cog's matrix has no headroom guarantee.**
Six correlated channels at full scale sum to 1.968 per side, not something ≤ 1.
The test was written asserting the opposite and failed, which is the more useful
outcome — it is recorded rather than corrected, because quietly rescaling would
make XPCog disagree with Cog on every surround file. Anything downstream assuming
samples sit in [-1, 1] should know a 5.1 source can hand it nearly double.

**Upmixing** is routing rather than a matrix: each input channel goes to the
output slot carrying the same flag, silence where there is no source. Cog's
`upmix()` is a ladder of per-layout cases, but every non-mono branch reduces to
that routing plus one real rule — splitting a back centre into the back pair when
the target has no BC slot — so the port keeps the rules and drops the ladder. It
also replaces the positional copy `fitChannels` used to do, which sent a quad
file's back pair to a 5.1 device's centre and subwoofer.

Two Cog bugs fixed rather than reproduced along the way:

- Cog's 6.1 upmix reads the side pair at interleave indexes 4–5 and back centre
  at 6, but 6.1 in flag order is FL FR FC LFE **BC SL SR** — back centre is bit 8,
  the sides bits 9–10, in Cog's own `AudioChunk.h`. So upmixing 6.1 rotates three
  channels: back centre plays from a side speaker. Routing by flag cannot express
  the mistake.
- The root of that is `channelIndexFromConfig`, which in Cog returns the index a
  flag *would* occupy even when the config lacks the channel — asking a 7.1 layout
  for its back centre answers 6, not "absent". XPCog's now returns `~0` as its
  own comment always claimed, with a test pinning it, because the upmix branches
  on exactly that.

**The time-stretchers are dropped, by decision rather than deferral.** Cog uses
rubberband and signalsmith for its tempo and pitch controls. Neither can be a
`DSPNode` as the contract stands: `process()` is in place with a fixed frame
count, and a time-stretcher returns a different number of frames than it was
given. So adding one was never "write a node" — it was "widen the interface every
other stage depends on", to gain a control a music player is not primarily for.
Skipping avoids that change rather than postponing it. `Rate`, `MinimumRate` and
`MaximumRate` are reported as a fixed 1 over MPRIS, which is how that protocol
says "not adjustable", and the `dsp` vcpkg feature that pulled in rubberband is
gone.

**FreeSurround is deferred, and blocked twice over.** It is not a time-stretcher
— it is a matrix surround decoder, stereo to 5.1 — so the paragraph above does
not cover it. It changes the *channel* count, which the node contract cannot
express either, and it is one of the kernels that needs a golden capture from Cog
before being replaced, which needs a Mac.

**Still to do:** the reduction from one multichannel layout to a smaller one (7.1
into quad), which still uses the positional copy. Cog runs every reduction
through its stereo matrix, which would throw away a real quad device's back
speakers.

*Verify:* the equaliser showed that one test is not enough, because each
plausible test is blind to something. Three were needed, and the second and third
gaps only became visible once the previous test passed:

- Cascade magnitude measured off a sine against its transfer function evaluated
  in the complex plane. Catches a transposed-form slip — but takes its
  expectation from `coefficients()`, so it cannot see a wrong *formula*.
- Each band's peak against the definition of a peaking EQ. Checks the formula
  against the maths rather than a re-derivation of itself — but peak gain is
  independent of Q.
- Q recovered from the half-gain bandwidth, the only parameter the other two are
  blind to.

Plus one at engine level, because there are four places that fill the converted
buffer — the prebuffer, the normal path and two drains — and a chain wired into
three of them passes every kernel test while dropping audio past the filter at a
seam. Deleting the prebuffer call moves the measured gain by 6%, which that test
catches.

For FreeSurround, capture golden reference output from Cog **before** replacing
the kernel — numeric drift from removing vDSP is silent otherwise, and unlike the
equaliser it is not pinned down by a closed-form response. Note this needs a
macOS machine: Cog does not run elsewhere, so on Windows or Linux that
verification is simply unavailable.

LPC extrapolation is already vendored but deliberately not built: the converter keeps
soxr's delay line continuous, so chunk edges already are. It earns its place at the
first block after a seek, which is M4's work.

### In progress: M5 — the platform's idea of a player

**Done:**

- **Windows SMTC** (C++/WinRT), and with it all three media integrations:
  MediaPlayer.framework on macOS, SMTC on Windows, MPRIS on Linux. The interface
  is the union of what they can do, and MPRIS is much the widest of the three —
  it is a general remote-control protocol, so it asks to raise the window, quit
  the application, set the volume and open a URL, none of which the other two can
  express. Those arrive as ordinary signals and the other two simply never emit
  them.
- **MPRIS** is the one integration written from a specification rather than
  translated from Objective-C, because Cog has nothing to port here. That removes
  the usual check — "does it do what Cog does" — so the reasoning is at the call
  site instead, particularly the four parts of the spec that are silent when got
  wrong: properties never announce themselves, times are microseconds, trackid is
  an object path, and `Position` must *not* be announced.
- **The presence outside the window**, which is deliberately two different objects.
  Windows and Linux get a `QSystemTrayIcon`; macOS gets the Dock menu, through
  `QMenu::setAsDockMenu()`. That is not a shortcut — Cog contains no `NSStatusItem`
  anywhere in its tree and hangs its transport off the Dock instead. The two do not
  carry the same items, because AppKit appends Quit and the window list to a Dock
  menu itself.
- **Single instance**, `QLocalServer`, off macOS only: LaunchServices already turns
  a second open into an event rather than a second process. The handover is framed
  by a length prefix, which a test forced — see the note below.
- **The application icon**, in two shapes, because the platforms disagree about
  what an icon is. Details in `app/icons/make-icons.py`.

- **The spectrum analyser**, which is Cog's configuration of deadbeef's
  `ddb_analyzer` plus the FFT stage from its `fft_pffft.c`: a 4096-sample Hamming
  window, 2048 bins scaled by 2/2048, semitone bands on a tempered scale from
  C0, 10 Hz–22 kHz, an 80 dB floor and a ten-frame peak hold. Every one of those
  numbers is taken from where Cog sets it rather than from the library's defaults,
  which differ.

  Two departures from the original plan, both deliberate. **No pffft**: the analyser
  needs one transform per displayed frame, so a dependency would buy a speed nobody
  can measure — the FFT is written out and checked against a naive DFT on random
  input, which is stronger evidence than trusting a library would have been. **No
  QML**: Cog itself ships a Core Graphics spectrum view beside its SceneKit one, so
  a 2D CPU path is Cog's own arrangement, and QPainter is very nearly a
  one-for-one substitution for Core Graphics. That also settles the deployment
  question — see the graphics runtime note under Known gaps.

  The audio is tapped in the *device callback*, after the gain, which is where
  XPCog differs from Cog structurally rather than incidentally. Cog posts PCM from
  up in the chain and then has to read behind its own write cursor by the device
  latency to undo the lead that creates; tapping at the last point before the driver
  means there is no lead to undo. See `AudioTap`.

  Customisation follows Cog's own preference keys where they exist:
  `spectrumBarColor`, `spectrumDotColor` and `spectrumFreqMode`. Two of Cog's five
  are deliberately absent -- `spectrumSceneKit` and `spectrumProjectionMode` both
  configure the SceneKit renderer, and a checkbox for a renderer that does not
  exist is worse than no checkbox. Two are new (`spectrumFloorDb`,
  `spectrumShowPeaks`) and labelled as such in `settings.def`.

  The colour keys match Cog's but their *values* do not: Cog archives NSColor
  through a value transformer, so an imported plist holds a keyed archive where a
  `#rrggbb` string is expected. That fails to parse and falls back to the same
  colour Cog defaults to, which makes the import correct for anyone who never
  changed it and a reset for anyone who did.

  One lesson learned twice, which is why it is here rather than only at the call
  site: **the band table says where the bars are, and the window's resolution is
  not its business.** Bands whose bins collide -- which happens below a few hundred
  hertz, where a semitone is under a fifth of a bin -- are kept and repeat the bin.
  Dropping them was the first attempt in *both* modes, with a different
  justification each time, and a test caught it each time: in NoteBands it breaks
  the note grid that motivates a tempered scale, and in Frequencies it turns an even
  log axis into steps that jump from a ratio of 1.13 to 2.08.

- **The taskbar button**, `ITaskbarList3`: a play/pause overlay badge and a
  progress bar. Port of Cog's `DockIconController`, with one correction to the
  obvious guess about what the bar means. It is **not** the position within the
  track. Cog observes an `NSProgress` published by `PlaylistLoader`, so the bar
  fills while files are being added and is gone the rest of the time; here it
  follows `ScanTask`. A bar tracking the seek position would be a different feature
  that happens to look the same, and it would sit at some arbitrary fraction for the
  whole of every track.

  Two deliberate differences from the Dock. Cog badges *every* state including
  stopped, because its badge replaces the whole 1024px tile and something has to be
  there; a Windows overlay is a 16px corner stamp on an icon that is already
  visible, and the convention is to show one only when there is something to say —
  so stopped clears it. And the glyphs are drawn with QPainter rather than shipped
  as assets, so they follow the display's scale factor instead of going soft at
  150%.

- **The mini player**, Cog's `miniWindow`. A *mode*, as in Cog: the main window
  hides and a compact one appears, because the point is to get the playlist off the
  screen rather than to add a second window beside it. `miniMode` and
  `floatingMiniWindow` are Cog's keys, including restoring the mode at launch.

  Not a visual copy, for a structural reason: Cog's mini window has **no content
  view at all**. It is an NSWindow whose entire body is a unified toolbar in the
  title bar, which is why `AppController` keeps setting its content height to zero.
  Qt has no equivalent — the title bar belongs to the window manager on every
  platform this targets — so the same controls sit in one compact row below a normal
  title bar. Cog's larger `miniPlusWindow` variant is deliberately not ported.

**Still to do:** the Dock half of the taskbar work — `NSDockTile`, which is the
surface Cog actually draws on. The interface and both call sites exist; only the
macOS implementation is missing, and it is the kind of custom tile drawing that
wants a Mac to look at rather than CI to compile.

### Then

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
- Cog's 6.1 upmix reads the side pair at interleave indexes 4–5 and back centre at 6,
  but 6.1 in flag order is FL FR FC LFE **BC SL SR** — back centre is bit 8 and the
  sides bits 9–10 in Cog's own `AudioChunk.h`. Upmixing 6.1 therefore rotates three
  channels and back centre plays from a side speaker.
- `channelIndexFromConfig` in Cog answers with the index a flag *would* occupy even
  when the config lacks that channel, so a 7.1 layout asked for its back centre says
  6 rather than "absent". That is the root of the bug above; XPCog's returns `~0` as
  its comment always claimed.

**Implementation**

- FLAC emits native-endian.
- Matching sample rates bypass the resampler, so a same-rate file stays bit-exact.
- Metadata readers merge in priority order rather than stopping at the first.
- `AudioChunk` is move-only and read APIs take an out-parameter so storage recycles.
  Cog's `-readAudio` allocates a fresh object every 16 KiB.
- **DSP stages are synchronous, not threaded.** Cog's `DSPNode` owns a thread, a
  `ChunkList`, two semaphores and a recursive lock per stage; XPCog's is an in-place
  transform and the chain is a sequence driven by one pump thread. The buffering that
  Cog's per-node threads provided is kept — see the two-stage ring — because dropping
  it made every DSP change inaudible for three seconds.
- **The equaliser keeps double biquad state** where `vDSP_biquadm` keeps single. A
  20 Hz section's poles sit close enough to the unit circle for it to matter, and 31
  sections cost nothing.
- **A flat equaliser is skipped, not run.** Cog pushes every sample through 31
  sections at 0 dB; skipping makes flat bit-transparent by construction.
- **Downmix is not a DSP node.** Cog's nodes pass chunks carrying their own format, so
  one can change the channel count; a `DSPNode` here works in place at a fixed format,
  so downmix lives in `AudioConverter` where channel geometry already changes.
- **Upmix is routing, not a per-layout ladder.** Every non-mono branch of Cog's
  `upmix()` reduces to "send each channel to the slot with the same flag", plus one
  real rule for splitting a back centre.
- **The seek crossfade holds power, not amplitude.** Cog ramps linearly; a seek lands
  on unrelated audio, so the two sides add in power and linear complements lose 3 dB
  through the middle. Sine and cosine legs hold the sum of squares at one. The cost is
  that identical audio either side — a very short seek — swells up to 3 dB instead.

**Not ported**

Mac App Store sandbox, AudioUnit MIDI instrument hosting, AppleScript, Spotlight,
the MCP server, Sparkle, Sentry. Cog also does **not** write tags —
`PluginController -putMetadataInURL:` is stubbed `return 0`, the facade has no
callers and `TagEditorController` is fully commented out. Tag writing is therefore a
new feature, not port work, and is deferred.

---

## Verification strategy

`xpcog-cli` links no Qt, so it exercises core headlessly in CI on all three
platforms — and it is **scaffolding, due to be deleted once the port lands**, so it
is not worth extending. Note what that will cost when it goes: this document and the
README both present its failure to link as *the* enforcement of the Qt-free rule.
What actually survives is the `*-headless` presets, which build core and the tests
with `XPCOG_BUILD_APP=OFF` and no Qt at all, plus `cmake/CheckNoQt.cmake`. Confirm
those still fail on a `QtCore` leak before removing the target.

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

### Five things that bite

**Playlist notifies after the fact; Qt wants to be told before.** `Playlist`
mutates, reindexes, and *then* calls its observers. Qt's model protocol is the
opposite: inside `beginRemoveRows()` the rows must still be there and `rowCount()`
must still report the old total. Reading the playlist live from `rowCount()` looks
obviously right and satisfies the assertion for every removal except one — the row
being removed has to be *less than* the new count, so removing the final row fails.

`Playlist::remove` works descending, so "select all and delete" erases the last row
first and aborts on the very first step. It shipped, and no test caught it, because
every existing test removed from the middle. `PlaylistModel` now keeps the count Qt
expects to see and advances it between begin and end, and its accessors bounds-check
against the *playlist* rather than against that count, since the two disagree for
exactly the width of that window.

The general lesson is worth more than the fix: an adapter between two change
protocols has to hold whichever state the stricter one requires. Reaching through to
the source of truth is what feels natural and is precisely what breaks.


**Tests that build fixtures skip silently when their encoder is missing.** Sixteen
tests shell out to `flac`, `oggenc`, `opusenc`, `lame`, `wavpack` or `ffmpeg`. A skip
is not a failure, so CI reported "100% tests passed out of 178" for months while
gapless, seeking, cue spans and tag reading went unrun on every platform. CI now
installs the encoders. **Check the skip count, not just the pass rate.**

**The fixture commands are POSIX shell, and `std::system` on Windows is not.**
Every one of those sixteen tests ended its command with `2>/dev/null`, and the
scanner probed for its encoder with `command -v`. Under `cmd.exe` the redirection
names a file called `\dev\null`, whose directory does not exist, so the
redirection fails and the encoder never runs; `command -v` is a shell builtin
`cmd.exe` does not have. Both surface identically to a missing tool, so
installing the encoders on Windows changed *nothing* — the tests skipped anyway.
Two silent failures composing into a third is why `tests/TestShell.hpp` now owns
the difference (`kSilenceStderr`, `haveTool`) rather than each call site.

**Tests share a temp directory across separate processes.** `catch_discover_tests`
runs every Catch2 case in its own process and ctest orders them *by name*, so a test
that reads a fixture another test writes is an ordering dependency — and one that
never shows locally, because the file survives in `$TMPDIR` from the previous run.
`album.cue` was written by "cue tracks decode the correct span" and read by three
tests, one of which sorts before it. It passed everywhere it had ever run and failed
the first time CI stopped skipping it.

Build every fixture through a lazily-initialised helper (`albumFlac()`, `albumCue()`)
so each process creates what it needs. To check for this, delete the fixture
directories and run the suite:

```sh
rm -rf "${TMPDIR:-/tmp}"/xpcog-*    # note: $TMPDIR on macOS, not /tmp
ctest --preset macos-debug
```

**`OfflineOutput` drains at maximum speed.** Any bug whose symptom is measured in
wall-clock seconds — a stall, a delayed position update, a buffer that takes time to
drain — collapses to nothing under it and cannot be regression-tested that way. This
produced two convincing-looking tests for the seek-position fix that both passed with
the bug deliberately restored.

It also makes any test that acts *during* playback a race, and the test does not
look like one. An eight-second file is consumed in the time it takes to decode it,
so "play, wait for the position to pass 0.5 s, then seek" read a position of 8.0
and seeked a track that had already ended. It passed on macOS for as long as
decoding happened to be slower than the poll loop, and failed immediately on a
faster Windows machine — restoring the unpaced output fails it 3 runs in 5, so it
was always flaky, just not where anyone was looking.

`makeOfflineOutput(ring, speedMultiple)` therefore takes an optional rate limit:
0 (the default) keeps the unlimited drain for every test that only checks *what*
was produced, and a positive value consumes at that multiple of real time for the
few that must observe playback in flight. The entitlement is capped at one read,
so a slow decoder open cannot bank credit and then burst through the ring — which
would be the unpaced behaviour again, intermittently.

Which leads to the general rule, and the most useful habit in this project:

> **Before claiming a fix is tested, put the bug back and confirm the test fails.**

It has caught worthless tests repeatedly — including one in the i18n work that
asserted a property Qt provides rather than the one the code was responsible for.

---

## Known gaps

- Windows CI now installs all six encoders from pinned upstream releases, so the
  suite runs the same sixteen tests on every platform. It depends on those URLs
  staying up, and a failed download fails the job deliberately — a mirror that
  quietly degraded back to sixteen skips is the exact failure mode being closed.
  Two of the six come from RareWares, which is not a versioned host in the way a
  GitHub release is; if it becomes unreliable, vendor the binaries instead of
  making the step tolerant.
- The Windows SMTC card is captioned **"Unknown app"** above otherwise correct
  track metadata. This is app identity, not metadata: an unpackaged executable
  has none, and Windows derives the name either from an AppUserModelID backed by
  a Start Menu shortcut carrying `System.AppUserModel.ID`, or from a package
  (MSIX, or an external-location "sparse" package). A version resource does not
  do it — `app/XPCog.rc.in` supplies `FileDescription`, which fixes Task Manager
  and the file's properties but leaves the caption unchanged, and
  `AssocQueryString(ASSOCSTR_FRIENDLYAPPNAME)` still answers `XPCog.exe`.
  Microsoft's guidance is that the shortcut is the installer's job, so this waits
  on there being an installer rather than the app writing to the Start menu
  itself.

  **Measured twice, and neither works.** A version resource's `FileDescription`
  does not change it, and neither does the `FriendlyAppName` that `--register` now
  writes — even though that is the key `AssocQueryString(ASSOCSTR_FRIENDLYAPPNAME)`
  answers with. So file metadata of any kind is ruled out, by experiment rather
  than by inference.

  What settles *why* is one API: `GlobalSystemMediaTransportControlsSession`
  exposes **`SourceAppUserModelId`**. The SMTC identifies a session by AUMID, and
  Microsoft's package-identity overview states that only packaged apps — full MSIX
  or packaged-with-external-location — have identity at run time, with the AUMID
  *derived from the package family name and the declared application ID*. An
  unpackaged process has no AUMID that resolves to a display name, so the media UI
  has nothing to caption the card with.

  The fix is therefore **package identity**, specifically a signed
  external-location ("sparse") package registered by the installer with
  `Add-AppxPackage -ExternalLocation`. That needs an MSIX identity manifest whose
  `Publisher` matches a code-signing certificate's subject, a signed `.msix`, and
  identity metadata in the application manifest — genuine installer work requiring
  a certificate, not something to bolt onto a build target.

  Note what is deliberately *not* done in the meantime:
  `SetCurrentProcessExplicitAppUserModelID`. It is otherwise good practice — the
  Win32 AppUserModelID documentation strongly recommends an explicit ID — but that
  same page requires the same ID to be assigned to every window, shortcut and file
  association the application owns, and XPCog installs no shortcut. Setting one
  half of that contract can leave taskbar pinning worse off than the heuristic ID
  Windows assigns on its own, and it would not touch this caption regardless. It
  belongs with the shortcut, in the installer, where the requirement can actually
  be met.
- **File associations exist but only on request.** `XPCog --register` adds XPCog to
  each supported extension's `OpenWithProgids` under `HKCU`; `--unregister` removes
  it. Never done at startup: an application that claims file types the first time
  it runs has taken them from whatever held them before without asking.

  It cannot make itself the *default*, and that is a fact about Windows rather than
  a gap — since Windows 8 the effective default lives in a hash-protected
  `UserChoice` key that only the Settings UI can write. So the registration makes
  XPCog available to choose, and choosing it is the user's step. The extension list
  comes from `PluginRegistry::allExtensions()` rather than being written out
  anywhere, so it cannot fall behind the codecs.
- `windeployqt` is invoked with `--no-translations`, so a deployed Windows build
  carries no Qt catalogues and Qt's own dialog strings stay English even in
  Spanish. XPCog's strings are unaffected — they are compiled into the executable
  as a `:/i18n` resource — and `macdeployqt` copies Qt's by default, so the two
  platforms currently disagree.
- `populateMenuBar()`'s translation lookup is not covered by a test: it needs a
  `QMenuBar`, so a `QApplication` and a platform plugin, and the test binary has
  neither.
- The macOS Now Playing integration is verified by hand, not by test. So are the
  pause and stop fades against a real device: the offline output shows the ramp in
  the capture, but that it sounds like a fade rather than a duck is a listening
  judgement.
- Nothing here, but worth keeping: **the tree is warning-free**, on a clean build,
  on all four CI jobs. That is a recent and hard-won property rather than a
  standing one, and the way it was lost is the point.

  Each compiler emits a different set, and a Windows-only development host sees
  only MSVC's. CI's jobs between them reported nine warnings this host had never
  shown: two `-Wshadow` from GCC (one a `data` that shadowed a `QWidget` member),
  C++20's deprecation of a bitwise operation between two different enumeration
  types, and `-Wdouble-promotion` in five places, three of which only Clang
  reported. Meanwhile a *clean* local rebuild turned up four `C4244`s from
  FFmpeg's own `libavutil/common.h`, which an incremental build had not
  recompiled — those are now suppressed at the source, by passing a codec's
  `INCLUDES` as `SYSTEM` in `xpcog_add_codec()`, since a decoder library's
  warnings are not ours to fix and reading like ours is the harm.

  `C4324` on `RingBuffer`'s deliberate cache-line `alignas` is suppressed
  narrowly, around exactly the three declarations that raise it, rather than
  project-wide — the padding is the entire point of the `alignas`, but switching
  the warning off globally would also hide the next unintended padding somewhere
  it matters.

  So: the list to check is CI's annotations across every job **plus** a clean
  local build, not the incremental log in front of you. Zero is only useful as a
  threshold while it is actually zero.
- **Preferences has no keyboard shortcut on Windows or Linux.**
  `QKeySequence::Preferences` is bound only on macOS, so `ActionRegistry` gives the
  action an empty sequence everywhere else. The menu item works; the keyboard does
  not.
- The `deploy` target's payload is **74.4 MB**, down from 94.8 MB. That gap was
  being held open on purpose until the visualisation's rendering was decided: if it
  had gone through Qt Quick, `opengl32sw.dll` — Mesa's software GL fallback, 19.7 MB
  of it — is exactly what keeps the application working on a machine with no usable
  GL driver. The spectrum draws with QPainter, so nothing here needs a GL context,
  and `--no-opengl-sw` now drops it. **Reinstate that flag's removal the moment
  anything starts using Qt Quick or a `QOpenGLWidget`.**

  What remains is **19.6 MB of shader compilers** — `dxcompiler.dll`, `dxil.dll`
  and `d3dcompiler_47.dll` — which come with Qt6Gui's RHI and which `windeployqt`
  has no switch to decline. A raster-only application almost certainly never loads
  them, so they could be deleted after deployment; that is not done, because
  "almost certainly" is doing a lot of work in that sentence and the failure mode is
  a missing DLL at startup on someone else's machine.
- Reducing one multichannel layout to a smaller one — 7.1 into quad — still uses a
  positional copy rather than a matrix. Cog runs every reduction through its stereo
  matrix, which would throw away a real quad device's back speakers, so the plain
  copy stays until that case has a matrix of its own.
- Gapless playback against a **real** Windows device — both a cue sheet's spans
  and consecutive loose FLACs — is verified by hand. `OfflineOutput` establishes
  that the seam is sample-exact, which is the part that can be automated; that it
  is also inaudible through WASAPI is not.
- `app/icons/xpcog.icns` is **structurally verified but never rendered**. The
  standard ImageMagick Windows build ships no ICNS coder (`IM_MOD_RL_ICNS_.dll`
  is absent) and `iconutil` is macOS-only, so `make-icons.py` emits the container
  itself and parses it back — magic, total length, chunk lengths, and a PNG
  signature per chunk. That rules out a packing slip, not a wrong chunk type for
  a given size: macOS declines to draw an icon it dislikes rather than reporting
  it, so the first real check is a Finder window on a Mac. The Windows `.ico` and
  the Qt resource are verified on this host, the `.ico` by extracting it back out
  of the linked executable.
- The **Dock menu** on macOS is likewise unrendered here. It compiles in CI, and
  `QMenu::setAsDockMenu()` is Qt's own call, but that the menu carries the right
  items *below* the ones AppKit appends is a thing to look at rather than assert.
- **MPRIS is compiled but not exercised.** CI's Linux job builds it; nothing on
  any of the three hosts has run `playerctl` against it or clicked a panel's media
  widget. Compilation is a weak check for a D-Bus contract, where the failures are
  a wrong variant type or a missing `PropertiesChanged` — both of which produce a
  player that appears on the bus and then quietly does not update.
- MPRIS reports **no `DesktopEntry`**, because XPCog installs no `.desktop` file.
  A panel therefore shows the transport with no application icon or localised
  name. Naming a file that does not exist would not improve that, so this waits on
  Linux packaging existing at all — the same gap as there being no `deploy` target
  for Linux.
- MPRIS omits the optional **`LoopStatus` and `Shuffle`** properties, so a panel
  cannot see or change the repeat and shuffle modes even though the playlist has
  both. They map onto `RepeatMode` and `ShuffleMode`, which have three and four
  values against MPRIS's two and one — a mapping worth designing rather than
  guessing at.
