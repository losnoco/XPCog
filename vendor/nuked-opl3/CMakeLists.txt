# Nuked OPL3, and the two MIDI drivers Cog puts in front of it.
#
# Copied from Cog Plugins/MIDI/MIDI, keeping its directory names so the
# provenance stays greppable and so the sources' own `../interface.h` includes
# resolve unchanged. Vendored rather than made an overlay port by the rule in
# ../../ports/README.md: Nuke.YKT publishes Nuked OPL3 as a C file to drop in,
# not as a release, and the two drivers around it exist only inside Cog.
#
# Three layers, and it is worth naming them because only one of the three is
# what "OPL3" usually means:
#
#   fmopl3lib/       Nuked OPL3 itself -- a cycle-accurate YMF262 emulation.
#                    It has no idea what MIDI is; it takes register writes.
#                    opl3class.cpp wraps it with the write queue and the
#                    resampler that turn 49716 Hz into the output rate.
#   synthlib_doom/   id's OPL driver from Chocolate Doom (i_oplmusic.c),
#                    with the six DMX instrument banks it played. This is the
#                    one Cog selects by default, and the reason a `.mus` sounds
#                    the way it did in 1993 rather than merely correct.
#   synthlib_opl3w/  Nuke.YKT's own General MIDI driver over the same chip,
#                    with a full GM timbre set. Better coverage of a general
#                    MIDI file; less faithful to a Doom one.
#
# Both drivers implement the same `midisynth` interface (interface.h), which is
# what lets one wrapper offer both.
#
# Local changes to the vendored sources
# -------------------------------------
#   synthlib_doom/i_oplmusic.h   `fm_chip *opl` initialised to nullptr
#   synthlib_opl3w/opl3midi.h    `fm_chip *opl_chip` likewise
#
# Both destructors delete their chip pointer, and neither class sets it before
# midi_init() does -- so constructing a synth merely to ask it for its bank
# names, which is what MSPlayer::enum_synthesizers and OplSynth::bankCount both
# do, frees an indeterminate pointer. It is invisible on macOS, where a fresh
# page reads as zero and `delete nullptr` does nothing, and immediate under
# MSVC's debug heap, which fills new memory with 0xCD. Same shape as the
# vendor/vio2sf GPU.h fix, and found the same way: it crashed on Windows.
#
# resampler.c is a third copy of kode54's sinc resampler and is deliberately not
# shared with vendor/lazyusf2's: they are different revisions, and each defines
# RESAMPLER_DECORATE to prefix its own symbols -- `midi_` here, `usf_` there --
# so both can link into one binary. Merging them would mean picking a revision
# for code that was tuned against each.
add_library(vendor-nuked-opl3 STATIC
    resampler.c
    fmopl3lib/opl3.cpp
    fmopl3lib/opl3class.cpp
    synthlib_doom/i_oplmusic.cpp
    synthlib_opl3w/opl3midi.cpp)

# SYSTEM for consumers, so third-party headers are not read under XPCog's own
# warning flags -- the tree being warning-free is a statement about our code.
target_include_directories(vendor-nuked-opl3 SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(vendor-nuked-opl3 PUBLIC cxx_std_17)
set_target_properties(vendor-nuked-opl3 PROPERTIES FOLDER vendor)

if(MSVC)
    target_compile_definitions(vendor-nuked-opl3 PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(vendor-nuked-opl3 PRIVATE /w)
else()
    target_compile_options(vendor-nuked-opl3 PRIVATE -w)
endif()
