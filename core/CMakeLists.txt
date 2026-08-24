configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/xpcog/core/Version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/xpcog/core/Version.hpp"
    @ONLY)

add_library(xpcog-core STATIC
    src/audio/AudioConverter.cpp
    src/audio/AudioTap.cpp
    src/audio/PanelFeed.cpp
    src/audio/AudioEngine.cpp
    src/audio/Downmix.cpp
    src/audio/Equalizer.cpp
    src/audio/EqualizerPresets.cpp
    src/audio/Fader.cpp
    src/audio/FreeSurround.cpp
    src/audio/Fft.cpp
    src/audio/RealFft.cpp
    src/audio/MiniaudioOutput.cpp
    src/audio/OfflineOutput.cpp
    src/audio/ReplayGain.cpp
    src/audio/RingBuffer.cpp
    src/audio/SampleConvert.cpp
    src/audio/SpectrumAnalyzer.cpp
    src/AssetPath.cpp
    src/AudioChunk.cpp
    src/AudioFormat.cpp
    src/library/Library.cpp
    src/library/PlaylistCommands.cpp
    src/library/Playlist.cpp
    src/library/PlaylistEntry.cpp
    src/library/PlaylistFile.cpp
    src/library/PlaylistView.cpp
    src/library/PluginCache.cpp
    src/library/PropertyList.cpp
    src/library/Scanner.cpp
    src/library/ScanTask.cpp
    src/library/Sqlite.cpp
    src/MetadataMap.cpp
    src/NaturalOrder.cpp
    src/Sha256.cpp
    src/LoopPolicy.cpp
    src/MultiDecoder.cpp
    src/PluginRegistry.cpp
    src/SerialExecutor.cpp
    src/Settings.cpp
    src/UndoStack.cpp
    src/FilePath.cpp
    src/Url.cpp
    src/Utf8.cpp
    src/Version.cpp
)
add_library(XPCog::core ALIAS xpcog-core)

target_include_directories(xpcog-core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_BINARY_DIR}/include")

# soxr does the sample-rate conversion, as it does in Cog.
find_package(soxr CONFIG QUIET)
if(NOT soxr_FOUND)
    find_library(SOXR_LIBRARY NAMES soxr REQUIRED)
    find_path(SOXR_INCLUDE_DIR NAMES soxr.h REQUIRED)
    add_library(soxr::soxr UNKNOWN IMPORTED)
    set_target_properties(soxr::soxr PROPERTIES
        IMPORTED_LOCATION "${SOXR_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${SOXR_INCLUDE_DIR}")
endif()

# SQLite replaces Core Data. Core talks to the C API directly rather than to
# QtSql, so the scanner, the CLI and the tests reach the library without needing
# a QCoreApplication -- and Cog sets the precedent with SQLiteStore.m.
find_package(unofficial-sqlite3 CONFIG QUIET)
if(unofficial-sqlite3_FOUND)
    set(XPCOG_SQLITE unofficial::sqlite3::sqlite3)
else()
    find_package(SQLite3 REQUIRED)
    # FindSQLite3 named its target SQLite::SQLite3 until CMake 3.31 renamed it to
    # SQLite3::SQLite3 and started warning on the old spelling. Both exist in the
    # releases that carry the new one, so preferring it keeps a system SQLite --
    # which is what this branch finds -- from printing the deprecation once per
    # target that links core.
    if(TARGET SQLite3::SQLite3)
        set(XPCOG_SQLITE SQLite3::SQLite3)
    else()
        set(XPCOG_SQLITE SQLite::SQLite3)
    endif()
endif()

target_link_libraries(xpcog-core
    PUBLIC  vendor-miniaudio
    PRIVATE vendor-hdcd
    PRIVATE vendor-dsd2pcm
    # PUBLIC, not PRIVATE: FreeSurround.hpp is a public core header and names
    # channel_setup and channel_id, which are the vendored enums. Anything that
    # can construct a decoder has to be able to name its channel layout.
    PUBLIC  vendor-fsurround
    PRIVATE ${XPCOG_SQLITE}
    PRIVATE soxr::soxr XPCog::warnings)

set_target_properties(xpcog-core PROPERTIES FOLDER core)

# --------------------------------------------------------------------------
# The layering rules, checked on every build.
#
# core and codecs must link no UI toolkit: that is what keeps the engine
# embeddable, headless-testable, and usable from xpcog-cli. platform's public
# headers must name none either, which is what makes app/ replaceable -- a claim
# the wxWidgets port put to the test, and which held.
#
# The primary enforcement is structural: xpcog-cli links no toolkit, so a leak
# breaks that build immediately. This check catches it earlier, with a clearer
# message, and runs on every build rather than only when the CLI is enabled.
#
# Attached to core because core is the innermost target and always built, not
# because the scope is core's -- the check itself reads the whole tree.
# --------------------------------------------------------------------------
add_custom_target(xpcog-no-toolkit ALL
    COMMAND "${CMAKE_COMMAND}"
            -D "XPCOG_ROOT_DIR=${CMAKE_SOURCE_DIR}"
            -P "${CMAKE_SOURCE_DIR}/cmake/CheckNoToolkit.cmake"
    COMMENT "Checking the toolkit layering rules"
    VERBATIM)
add_dependencies(xpcog-core xpcog-no-toolkit)
set_target_properties(xpcog-no-toolkit PROPERTIES FOLDER core)
