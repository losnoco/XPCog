# MIDI: the container, and the player that renders it.
#
# Two targets in one directory, the same split codecs/psf and codecs/usf make
# and for the same reason. `xpcog-codec-midi` is the parsed file and nothing
# about sound; it registers nothing, and stayed that way through stage 0 because
# claiming `.mid` with no synthesiser behind it would put files in the open
# dialog that open and then sit in silence.
#
# `xpcog-codec-midisynth` is what makes the sound, and it registers. Nuked OPL3
# is the first synthesiser; SpessaSynth and Nuked SC-55 are staged behind the
# same event stream (docs/MIDI.md) and land as further sources here, chosen by
# the `midiPlugin` setting -- not as further decoders, since all three answer to
# the same extensions.
add_library(xpcog-codec-midi STATIC MidiFile.cpp)
target_include_directories(xpcog-codec-midi PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(xpcog-codec-midi
    PUBLIC xpcog-core vendor-midi-processing
    PRIVATE XPCog::warnings)
set_target_properties(xpcog-codec-midi PROPERTIES FOLDER codecs)

xpcog_add_codec(
    NAME     midisynth
    REGISTER xpcog_register_midi
    SOURCES  OplSynth.cpp MidiDecoder.cpp
    DEPS     xpcog-codec-midi vendor-nuked-opl3 xpcog-codec-common
)
