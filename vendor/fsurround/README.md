# FreeSurround channel maps

From Cog's `Audio/ThirdParty/fsurround`, originally Christian Kothe's
FreeSurround (GPL v2 or later).

## What is here, and what is not

| File | State |
|---|---|
| `channelmaps.cpp` | **Verbatim.** ~3,100 lines of literal gain grids. |
| `channelmaps.h` | Verbatim but for its include, repointed at the header below. |
| `freesurround_channels.h` | **Trimmed** from `freesurround_decoder.h`: the two enums only. |

The decoder itself is **not** vendored. It is reimplemented in
`core/src/audio/FreeSurround.cpp`, because the only substantial thing it does
that is not arithmetic is call vDSP, and every one of those calls had to be
replaced for the port to build anywhere but macOS. Keeping a "vendored" copy that
had been rewritten line by line would claim a fidelity it does not have.

`freesurround_decoder.h` was trimmed rather than copied for the same reason: it
declares a class nothing here defines, and a header promising a type that does
not exist is worse than a shorter header. The enums stay because
`channelmaps.cpp` is keyed on them.

## Why the data is verbatim

Every character of `channelmaps.cpp` is a number, and the port is checked against
a golden capture of the original's output (`tests/golden/`). Retyping, reformatting
or regenerating the tables would put a transcription error and a porting error in
the same change, with one set of tests to tell them apart.

## Static initialisation

`channelmaps.cpp` ends with `bool success = init_maps();` in an anonymous
namespace, which fills the maps before `main`. That is the pattern
`cmake/XPCogCodec.cmake` warns about for codecs -- inside a static library the
linker only extracts an archive member that resolves an undefined symbol, so a
purely self-registering translation unit can be dropped silently.

It is safe here, and the reason is worth stating rather than trusting: the maps
themselves (`chn_alloc` and friends) are the undefined symbols the decoder
references, so the member is always extracted, and the initialiser runs because
it is in the same translation unit as the data it fills.
