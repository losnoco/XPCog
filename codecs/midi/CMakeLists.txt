# MIDI: the container, and the player that renders it.
#
# Two targets in one directory, the same split codecs/psf and codecs/usf make
# and for the same reason. `xpcog-codec-midi` is the parsed file and nothing
# about sound; it registers nothing, and stayed that way through stage 0 because
# claiming `.mid` with no synthesiser behind it would put files in the open
# dialog that open and then sit in silence.
#
# `xpcog-codec-midisynth` is what makes the sound, and it registers. Two
# synthesisers so far -- Nuked OPL3 and Nuked SC-55 -- behind the MidiSynth
# interface, chosen by the `midiPlugin` setting rather than by registering a
# decoder each, since all of them answer to the same extensions. SpessaSynth is
# the third; see docs/MIDI.md.
#
# xpcog-archive-reader is here because the SC-55 ROM set is read straight out of
# the zip it was distributed in.
add_library(xpcog-codec-midi STATIC MidiFile.cpp)
target_include_directories(xpcog-codec-midi PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(xpcog-codec-midi
    PUBLIC xpcog-core vendor-midi-processing
    PRIVATE XPCog::warnings)
set_target_properties(xpcog-codec-midi PROPERTIES FOLDER codecs)

xpcog_add_codec(
    NAME     midisynth
    REGISTER xpcog_register_midi
    SOURCES  OplSynth.cpp Sc55Roms.cpp Sc55Synth.cpp MidiDecoder.cpp
    DEPS     xpcog-codec-midi vendor-nuked-opl3 vendor-nuked-sc55
             xpcog-archive-reader xpcog-codec-common
)
