# MIDI: staging plan

Cog's MIDI plugin is a sequencer driving a choice of **software synthesisers**,
and that is the shape that decides how it lands here. One of the synths is macOS
only and is not being ported; the other three are portable C/C++ and each is
worth its own stage, exactly as the eight emulator cores behind PSF were.

Written to be picked up on another machine. Everything below was read out of
Cog's tree rather than remembered, and the file it came from is named each time.

## Why this is not "out of scope"

[`../README.md`](../README.md) lists **AudioUnit MIDI instrument hosting** as
deliberately not ported, and that is accurate — but it describes one backend of
four, not MIDI playback. `Plugins/MIDI/MIDI.xcodeproj`'s compile phase is the
authority on what Cog actually builds:

| Source | What it is | Portable |
|---|---|---|
| `AUPlayer.mm`, `AUPlayerView.mm` | AudioUnit instrument hosting | **No** — macOS only |
| `SpessaPlayer.mm` + `spessasynth_core/` | SpessaSynth, a SoundFont synth | Yes — 105 C/C++ files, already a native port |
| `SCPlayer.mm`, `SCCore.cpp` + `Frameworks/nuked-sc55` | Roland SC-55mkII emulation | Yes — 6 C++ sources |
| `MSPlayer.cpp` + `opl3.cpp`, `opl3class.cpp`, `opl3midi.cpp`, `i_oplmusic.cpp` | Nuked OPL3, driven by Doom's OPL music code | Yes |
| `MIDIContainer.mm`, `MIDIDecoder.mm`, `MIDIMetadataReader.mm`, `MIDIPlayer.cpp`, `resampler.c` | The container, the decoder shell and the sequencer that feeds a synth | Yes — the `.mm` files are thin wrappers over C++ |

`BMPlayer.cpp` (BASSMIDI), `MT32Player.cpp` (Munt) and `SFPlayer.cpp`/`tsf.c`
(TinySoundFont) are **present in the tree and not compiled**. `Frameworks/munt`
is vestigial for the same reason. Do not port them: reading the directory
listing rather than the build was the first wrong turn taken here.

`MSPlayer` takes a moment to recognise, because nothing is named OPL. It is
selected by a preference string of `"DOOM"` plus a bank number
(`MIDIDecoder.mm:304`), sets `set_synth(0)`, and its `opl3*.cpp` files are
Nuked's OPL3 with Doom's `i_oplmusic.cpp` translating MIDI onto it.

## What the sequencer covers

`Frameworks/midi_processing` is fourteen files and reads far more than Standard
MIDI. One processor per format:

`midi_processor_standard_midi.cpp` (`mid`, `midi`), `riff_midi` (`rmi`, `mids`),
`hmi`, `hmp`, `xmi`, `mus` (Doom), `lds` (Loudness), `gmf`, `syx`.

The extensions Cog actually claims are
`hmi hmp hmq kar lds mds mid midi mids mus mxmf rmi xmf xmi`
(`MIDIDecoder.mm`). Note that `gmf` and `syx` have processors but no extension —
they are reached by content, not by name.

Note `mus` and `lds` overlap with things this tree already claims — `mus` by the
SID decoder, `lds` by nothing yet but by AdPlug in Cog. Settled by content
rather than by priority in the end: both claimants sit at the default, MIDI is
registered first, midi_processing sniffs the file, and a Commodore 64 `.mus`
fails to parse here and falls through to the SID decoder. The container path
does the same, since returning the URL unchanged is how a container declines.
That leaves `.ahx` between vgmstream and Hively still needing a decision.

## Stages

Each stage ends with something audible, and nothing registers a decoder until a
synth exists to answer it — the rule stage 0 of HighlyComplete established, and
for the same reason: a decoder that cannot decode is worse than a format the
player does not claim.

| stage | what | needs | |
|---|---|---|---|
| 0 | `midi_processing` vendored; container, sequencer, metadata, subsongs. **Registers nothing.** | — | done |
| 1 | **Nuked OPL3**, via the `opl3*` sources. First audible MIDI, and the decoder that registers. | nothing external | done |
| 2 | **SpessaSynth**, and a SoundFont setting | a user-supplied `.sf2`/`.sf3` | |
| 3 | **Nuked SC-55**, and a ROM setting | a user-supplied ROM set | audio done |
| 3b | The SC-55's **front-panel LCD**, in sync with playback | the same ROM set | capture and feed done |

OPL3 is first on purpose: it is the only one of the three that needs no asset
the user has to find, so stage 1 proves the whole sequencer path — parse, tempo
map, event delivery, rendering, seeking — with nothing else able to be blamed.

## What stage 1 turned out to be

Two things came out differently from what the table above assumed, and both are
worth stating because the next two stages inherit them.

**`MIDIPlayer` was not ported, and `MSPlayer` only half was.** Cog's
`MIDIPlayer.cpp` is no longer a sequencer at all: it is a shell over
`SS_Sequencer` from `spessasynth_core`, so porting it at stage 1 would have
dragged in the whole of stage 2 to make an OPL3 chip play a note. But
`midi_processing` already serialises a subsong to a flat event stream with
timestamps in seconds and a tempo map applied — which is what the older
`MIDIPlayer` did with it, and is all a callback-mode backend needs. So
`MidiDecoder.cpp` drives the synth from that stream directly.
`MSPlayer.cpp` survives only as `OplSynth`, the wrapper around `midisynth`.

The event loop is sample-accurate rather than chunked. Cog quantises to 256
frames because its backends want driving that way; nothing here does, and asking
an OPL3 for four samples is free.

**There are two OPL drivers, not one.** `MSPlayer` selects between
`getsynth_doom()` and `getsynth_opl3w()`, and they are genuinely different
instruments over the same chip: id's DMX driver with the six banks Doom, Doom II,
Raptor and Strife shipped, and Nuke.YKT's own General MIDI driver. Both are
exposed, through Cog's own `midiPlugin` vocabulary — `DOOM0`..`DOOM5` and
`OPL3W0` — so a settings file carried over from Cog keeps naming the same one.
An unrecognised value, which is what a macOS Cog's AudioUnit component code will
look like, falls back to the default rather than refusing the file.

`vendor/nuked-opl3` carries one local change, in both drivers: their `fm_chip *`
member is deleted by the destructor and set only by `midi_init()`, so
constructing a synth merely to read its bank names frees an indeterminate
pointer. Cog's `MSPlayer::enum_synthesizers` does exactly that and gets away with
it because a fresh page on macOS reads as zero. MSVC's debug heap fills new
memory with 0xCD, and it crashed on the first corpus file. Same shape as the
`vendor/vio2sf` GPU.h fix, and found the same way.

## The SC-55 ROMs are the user's, not ours

Cog does not ship them and neither will this. `SCPlayer.mm:158` looks in
`~/Library/Application Support/Cog/Roms/`, and only `back.data` — a small
built-in — comes from the bundle. Roland's SC-55 firmware is 3.6 MB of
commercial ROM; the C64 KERNAL in `codecs/sid` and the PlayStation BIOS in
`vendor/highlyexperimental` are vendored because Cog vendors them, and this is
the case where Cog does not.

So there is a settings path (`midiRomPath`), an opt-in variable for the tests
(`XPCOG_SC55_ROMS`), and a clean skip when neither is set.

### Identified by hash, and this document had it wrong

An earlier draft of this file said the mapping from a dumped set's part-number
filenames to the ones `mcu.cpp`'s romset table asks for "is by size and is
unambiguous". That was a guess, and Cog does something better:
`Preferences/MIDIConfig.mm`'s `+nukedRomsets` is a table of **SHA-256 hashes**,
ten of them, five per supported model. `codecs/midi/Sc55Roms.cpp` carries the
same table.

The difference is not pedantry. A size match says "a file of the right length";
a hash match says "the ROM this emulator was written against", and a wrong dump
does not fail loudly — it boots into a machine that sounds subtly wrong.

The set is also checked for completeness and for belonging to one model. Five
files of an mkII and one stray mk1 waverom is not a machine.

### It reads the archive directly

Cog's importer accepts `zip`, `rar` or `7z`, hashes the entries, and then
*copies* the ROMs into its own application-support folder for its player to read
back by a fixed path. This does the identification and skips the copy: the
setting names either the folder or the archive, and both are read in place. That
is one fewer copy of somebody's commercial firmware lying around in a directory
they did not choose.

libarchive is already in the tree for the archive source, so this cost a static
library split (`xpcog-archive-reader`) and nothing else.

### What the hardware turns out to dictate

`sc55_get_sample_rate()` reports **66,207 Hz** for the mkII and 64,000 for the
mk1 — `mcu.cpp:1085`, hard-coded from the real machine's clock. There is no rate
argument anywhere in `api.h`, so `synthSampleRate` simply does not apply to this
synthesiser, and every SC-55 track is resampled by the player downstream.

Booting is also real: `sc55_init` loads the ROMs and then the firmware has to
run its own startup, which Cog spins for seven seconds of emulated time before
sending a note. That is a fraction of a second of CPU at every track start.

### The front panel

The panel is part of the emulation, not a decoration bolted on: `lcd.cpp` is one
of the seven sources, the firmware drives it, and `api.h` is built for handing
it out. `sc55_render_with_lcd()` takes a callback that receives the panel state
whenever it changes, and `sc55_lcd_render_screen()` composites that state against
`data/back.data` -- a raw 741x268 RGBA photograph of the front panel, which
*is* shippable, unlike the firmware.

**The timestamp is a stream position, and api.h says otherwise.** The header
documents it as "milliseconds, absolute elapsed since boot". Booting is seven
seconds of emulated time, so believing the header puts every captured frame
seven seconds into a stream that may be one second long. The code disagrees:
the value comes from `st->sample_counter` (`mcu.cpp:982`), which accumulates
while rendering and is zeroed *only* inside `sc55_spin` (`mcu.cpp:1542`) -- the
boot spin, which zeroes it after every page. By the time booting ends the
counter is back at nothing, so it counts exactly the samples rendered since.

Two tests hold that down, and both were confirmed by putting the bug back: one
asserts the frames land inside the second that was rendered rather than seven
seconds past it, and one renders the same second in a single call and in a
hundred, asserting identical positions. The second is the one that matters for
sync -- if positions came from the caller's chunking, the panel would drift by
the buffer size, which changes with the output device.

**The display is amber, and the emulator already knows.** Kevin flagged that the
panel is amber where Cog draws it blue — kode54 was bitten by this — and then
that it might be a channel-order problem rather than a wrong colour. It is, and
three pieces of evidence agree:

| where | read as `0xAARRGGBB` | read as RGBA bytes |
|---|---|---|
| `back.data`, dominant pixel `(255,111,15,255)` | dark blue | **amber** — a photograph of the lit panel |
| SC-55 `lcd_col2 = 0x0050c8` (`mcu.cpp:1183`) | blue | **amber**, (200, 80, 0) |
| JV-880 fill `0xFF03be51` (`lcd.cpp:399`) | dark blue | **green**, which is the JV-880's display |

So `lcd_buffer_t` is RGBA in **byte** order — little-endian `0xAABBGGRR` — and
nothing needs recolouring. What the consumer must not do is treat a `uint32_t`
as `0xAARRGGBB`, which is the one reading that turns every one of those three
into a blue.

One trap inside the trap: the SC-55's two colour constants carry **alpha 0**,
unlike the JV-880's `0xFF...` and unlike `back.data`'s opaque pixels. A consumer
that honours alpha therefore draws the panel and leaves the characters
invisible. Qt's `QImage::Format_RGBX8888` reads the byte order above and ignores
the fourth channel, which is exactly right.

The colours are also not baked into the pixels, which is worth knowing in case a
future machine needs adjusting: `LCD_Init` copies them into the `lcd_state_t`
that reaches the callback (`lcd.cpp:215`) and `LCD_Update` reads them back out of
that blob, so a captured state can be recoloured before rendering without
touching the vendored sources.

### The queue, and why it is a global

`core/audio/PanelFeed` is the port of Cog's `MIDIVisualizationController`, and
the same shape on purpose: one process-wide queue, keyed by track.

A global was chosen after the alternative turned out not to exist. The producer
is a decoder, and a decoder belongs to the engine's feeder thread — `AudioEngine`
marshals even `seek()` across rather than touch it from the caller's, and it
exposes no decoder at all. So "let the widget ask the decoder" is not a design
that was rejected; it is one the UI thread cannot reach. Something shared and
synchronised is needed either way, and once that is true, injecting it buys only
test isolation — which is paid for instead by `clear()`, called at the top of
every test that posts.

Keyed by track because two are alive at once: `Delegate::nextTrack()` is asked
"while its audio is still playing out", so across a gapless seam two decoders
produce simultaneously and both count from zero within themselves. A drain only
ever returns the audible track's.

**One difference from Cog.** Cog derives "now" from the newest queued event's
timestamp minus `[controller getFullLatency]`, because it has no better clock to
hand. `AudioEngine::trackPositionSeconds()` is built from audio *delivered to
the device*, so it already accounts for buffering — the drain takes that and has
nothing to estimate.

Trimming is on post rather than on Cog's thirty-second timer: what is being
bounded is a panel nobody is draining, and a paused player drains nothing, so a
timer would be the one thing that could fall behind.

### Drawing it is the part still to come

Capture is done and positioned; what remains is the widget. Cog has both halves
already -- `Audio/Visualization/MIDIVisualizationController.m` and
`Visualization/SCView.m`, 1,224 lines between them -- and one design decision in
there does not transfer.

`AudioTap`, which feeds the spectrum, gets its sync for free by being filled in
the *device callback*: what is written is what is about to be heard, so the
latency compensation disappears rather than being reimplemented. LCD state
cannot use that trick, because it is produced at decode time and does not travel
in the samples. It has to be a timestamped queue drained against the playback
position -- which is what Cog does, subtracting `[controller getFullLatency]`
from the newest event's timestamp.

Three behaviours of Cog's to carry, each a bug if missed: a floor of 5 ms
between captured states, a flush on seek, and trimming states older than the
window. All three are done. One deliberately *not* carried: Cog posts the
previous state stamped with the previous timestamp, one throttle window behind,
which is defensible but means a panel that goes quiet never shows its final
state until something else changes.

`app/windows/Sc55PanelWidget` is the widget, and it is thin: the emulator
draws the panel itself, so all it does is drain the feed against
`PlaybackController::position()` thirty times a second, keep the newest state,
hand it to `sc55_lcd_render_screen()` and blit the result.

**It is not docked by default.** A front panel for one synthesiser of three,
for one format among many, is not something to put in everyone’s window — it is
a View menu item that starts hidden, and `PanelFeed::wanted()` follows the
dock’s visibility through showEvent and hideEvent, so the emulator is not
comparing its panel against the previous state on every sample for a window
nobody opened.

Only the newest state of each batch is drawn. The others are the panel’s own
history between repaints, and at up to two hundred a second they are not
something an eye resolves.

### Missing ROMs fall back rather than refusing

Cog returns `NO` from `-open:` when the ROMs are absent, so the file does not
play at all. Here the decoder builds the OPL3 instead and the file plays — with
`TrackProperties::encoding` naming what actually rendered it, which is the only
way anyone could tell the difference.

## The corpus

`-DXPCOG_MIDI_CORPUS=<path>`, opt-in like the PSF, SID and vgmstream ones and
for the same reason: these files cannot be committed.

The collection this was developed against holds 198,712 files, of which 197,475
are Standard MIDI (3.6 GB, averaging 19 KB, largest 3.9 MB). What makes it worth
more than its size is the tail, which reaches four processors a folder of `.mid`
never would:

| extension | files | processor |
|---|---|---|
| `mid`, `midi` | 197,475 | `standard_midi` |
| `mus` | 190 | `mus` — Doom |
| `hmi` | 190 | `hmi` |
| `lds` | 94 | `lds` — Loudness |
| `rmi` | 9 | `riff_midi` |
| `mids` | 3 | `riff_midi`, Microsoft's variant |

Absent from it, and so still unexercised: `hmp`, `xmi`, `kar`, `xmf`, `mxmf`,
`hmq`, `mds`. `xmi` is the one worth finding a fixture for, since it is the
format that makes subsongs real.

The sweep caps at 25 files per format. A corpus of this size holds truncated and
misnamed files and refusing those is correct, so the test requires a majority to
parse rather than all of them — what would be a failure is a format whose
processor never runs at all.

Stage 1 adds a second sweep that *renders* rather than parses, over `mid` and
`mus` — the two worlds the drivers were written for. Twelve of each, five seconds
apiece, and it is the level that is checked: a file that parses and then plays
silence is the failure this catches, and the only one the parsing sweep cannot.
Both formats currently come back twelve of twelve audible.

## Where each piece should live

`midi_processing` and `nuked-sc55` are Cog's own trees with no separate
upstream release, and `spessasynth_core` sits inside the plugin — so all three
are [`../vendor/`](../vendor) cases by the rule in
[`../ports/README.md`](../ports/README.md), not overlay ports. The OPL3 sources
live in the plugin directory in Cog and are small enough to vendor beside it.

Landed so far: [`../vendor/midi_processing`](../vendor/midi_processing) and
[`../vendor/nuked-opl3`](../vendor/nuked-opl3), the latter keeping Cog's own
`fmopl3lib` / `synthlib_doom` / `synthlib_opl3w` directory names so the sources'
`../interface.h` includes resolve unchanged and the provenance stays greppable.
`nuked-sc55` and `spessasynth_core` go beside them at stages 3 and 2.
