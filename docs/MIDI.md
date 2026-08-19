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
SID decoder, `lds` by nothing yet but by AdPlug in Cog. Priority ordering will
need stating, the same way `.ahx` needs it between vgmstream and Hively.

## Stages

Each stage ends with something audible, and nothing registers a decoder until a
synth exists to answer it — the rule stage 0 of HighlyComplete established, and
for the same reason: a decoder that cannot decode is worse than a format the
player does not claim.

| stage | what | needs |
|---|---|---|
| 0 | `midi_processing` vendored; container, decoder shell, sequencer, metadata. **Registers nothing.** | — |
| 1 | **Nuked OPL3**, via `MSPlayer` and the `opl3*` sources. First audible MIDI. | nothing external |
| 2 | **SpessaSynth**, and a SoundFont setting | a user-supplied `.sf2`/`.sf3` |
| 3 | **Nuked SC-55**, and a ROM-directory setting | a user-supplied ROM set |

OPL3 is first on purpose: it is the only one of the three that needs no asset
the user has to find, so stage 1 proves the whole sequencer path — parse, tempo
map, event delivery, rendering, seeking — with nothing else able to be blamed.

## The SC-55 ROMs are the user's, not ours

Cog does not ship them and neither will this. `SCPlayer.mm:158` looks in
`~/Library/Application Support/Cog/Roms/`, and only `back.data` — a small
built-in — comes from the bundle. Roland's SC-55 firmware is 3.6 MB of
commercial ROM; the C64 KERNAL in `codecs/sid` and the PlayStation BIOS in
`vendor/highlyexperimental` are vendored because Cog vendors them, and this is
the case where Cog does not.

So stage 3 gets a settings path, an opt-in corpus variable for its tests, and
skips cleanly when neither is set.

`Frameworks/nuked-sc55/nuked-sc55/mcu.cpp:64` holds the romset table. Index 0 is
SC-55mkII and wants five files, which is what a dumped set provides under part
numbers — the mapping is by size and is unambiguous:

| nuked-sc55 wants | a dumped mkII set calls it | bytes |
|---|---|---|
| `rom1.bin` | `r15199858_main_mcu.bin` | 32,768 |
| `rom2.bin` | `r00233567_control.bin` | 524,288 |
| `waverom1.bin` | `r15209359_pcm_1.bin` | 2,097,152 |
| `waverom2.bin` | `r15279813_pcm_2.bin` | 1,048,576 |
| `rom_sm.bin` | `r15199880_secondary_mcu.bin` | 4,096 |

Worth deciding at stage 3 whether to accept the part-number names directly
rather than making everyone rename five files. The sizes identify them exactly,
and nothing else in a ROM directory has those five sizes.

## Where each piece should live

`midi_processing` and `nuked-sc55` are Cog's own trees with no separate
upstream release, and `spessasynth_core` sits inside the plugin — so all three
are [`../vendor/`](../vendor) cases by the rule in
[`../ports/README.md`](../ports/README.md), not overlay ports. The OPL3 sources
live in the plugin directory in Cog and are small enough to vendor beside it.
