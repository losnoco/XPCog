# midi_processing: the sequencer half of Cog's MIDI plugin.
#
# Cog's Frameworks/midi_processing, verbatim. Vendored rather than made an
# overlay port for the reason ../../ports/README.md gives -- it has no upstream
# release of its own; it is kode54's, developed inside Cog, and an Xcode project
# is the only build it ships with.
#
# Thirteen sources and it is not just Standard MIDI. One processor per format:
# standard_midi (mid, midi), riff_midi (rmi, mids), hmi, hmp, xmi, mus for
# Doom's, lds for Loudness, gmf, and syx for a bare SysEx dump. That breadth is
# the reason this is worth a stage of its own -- every synth that follows reads
# the same container, and a bug in the parsing is a bug in all of them.
add_library(vendor-midi-processing STATIC
    midi_container.cpp
    midi_processor_gmf.cpp
    midi_processor_helpers.cpp
    midi_processor_hmi.cpp
    midi_processor_hmp.cpp
    midi_processor_lds.cpp
    midi_processor_mids.cpp
    midi_processor_mus.cpp
    midi_processor_riff_midi.cpp
    midi_processor_standard_midi.cpp
    midi_processor_syx.cpp
    midi_processor_xmi.cpp)

# SYSTEM for consumers, so third-party headers are not read under XPCog's own
# warning flags -- the tree being warning-free is a statement about our code.
target_include_directories(vendor-midi-processing SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(vendor-midi-processing PUBLIC cxx_std_17)
set_target_properties(vendor-midi-processing PROPERTIES FOLDER vendor)

if(MSVC)
    target_compile_definitions(vendor-midi-processing PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(vendor-midi-processing PRIVATE /w)
else()
    target_compile_options(vendor-midi-processing PRIVATE -w)
endif()
