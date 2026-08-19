# The MIDI container, without a synth.
#
# Not an xpcog_add_codec() for the same reason codecs/psf is not: that macro
# registers a codec with the plugin registry, and there is nothing to register
# until something can make a sound. Claiming `.mid` with no synth behind it
# would put files in the open dialog that open and then sit in silence.
#
# So this is a plain library that the first synth will link. It is worth landing
# on its own because all three synths staged in docs/MIDI.md need exactly this
# and none of them needs it differently.
add_library(xpcog-codec-midi STATIC MidiFile.cpp)
target_include_directories(xpcog-codec-midi PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(xpcog-codec-midi
    PUBLIC xpcog-core vendor-midi-processing
    PRIVATE XPCog::warnings)
set_target_properties(xpcog-codec-midi PROPERTIES FOLDER codecs)
