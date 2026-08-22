# MIDI: the container, and the player that renders it.
#
# Two targets in one directory, the same split codecs/psf and codecs/usf make
# and for the same reason. `xpcog-codec-midi` is the parsed file and nothing
# about sound; it registers nothing, and stayed that way through stage 0 because
# claiming `.mid` with no synthesiser behind it would put files in the open
# dialog that open and then sit in silence.
#
# `xpcog-codec-midisynth` is what makes the sound, and it registers. Three
# synthesisers -- Nuked OPL3, Nuked SC-55 and SpessaSynth -- behind the
# MidiSynth interface, chosen by the `midiPlugin` setting rather than by
# registering a decoder each, since all of them answer to the same extensions.
#
# xpcog-archive-reader is here because the SC-55 ROM set is read straight out of
# the zip it was distributed in.
add_library(xpcog-codec-midi STATIC MidiFile.cpp)
target_include_directories(xpcog-codec-midi PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(xpcog-codec-midi
    PUBLIC xpcog-core
    PRIVATE XPCog::warnings)
set_target_properties(xpcog-codec-midi PROPERTIES FOLDER codecs)

# SpessaSynth is an overlay port (../../ports/spessasynth-core) that installs a
# header tree and an archive and no CMake config package, so the imported target
# is assembled here -- the same way as libsidplayfp's and libgme's.
#
# The three link dependencies are its own: FLAC and Vorbis decode the compressed
# samples in an SF3 bank, and zlib the compressed nodes in an XMF file. A static
# archive carries no record of them, so they are named here or nothing links.
find_path(SPESSASYNTH_INCLUDE_DIR NAMES spessasynth/spessasynth.h REQUIRED)
find_library(SPESSASYNTH_LIBRARY_RELEASE NAMES spessasynth REQUIRED)
find_library(SPESSASYNTH_LIBRARY_DEBUG NAMES spessasynth
             PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib"
             NO_DEFAULT_PATH)

find_package(FLAC CONFIG REQUIRED)
find_package(Vorbis CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

if(NOT TARGET spessasynth::spessasynth)
    add_library(spessasynth::spessasynth UNKNOWN IMPORTED GLOBAL)
    set_target_properties(spessasynth::spessasynth PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${SPESSASYNTH_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "FLAC::FLAC;Vorbis::vorbisfile;ZLIB::ZLIB"
        IMPORTED_LOCATION "${SPESSASYNTH_LIBRARY_RELEASE}")
    if(SPESSASYNTH_LIBRARY_DEBUG)
        set_target_properties(spessasynth::spessasynth PROPERTIES
            IMPORTED_LOCATION_DEBUG "${SPESSASYNTH_LIBRARY_DEBUG}")
    endif()
endif()

xpcog_add_codec(
    NAME     midisynth
    REGISTER xpcog_register_midi
    SOURCES  OplSynth.cpp Sc55Roms.cpp Sc55Synth.cpp SoundFontSynth.cpp
             MidiDecoder.cpp
    DEPS     xpcog-codec-midi vendor-nuked-opl3 vendor-nuked-sc55
             spessasynth::spessasynth xpcog-archive-reader xpcog-codec-common
)
