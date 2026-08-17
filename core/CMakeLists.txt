configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/xpcog/core/Version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/xpcog/core/Version.hpp"
    @ONLY)

add_library(xpcog-core STATIC
    src/audio/AudioConverter.cpp
    src/audio/AudioEngine.cpp
    src/audio/MiniaudioOutput.cpp
    src/audio/OfflineOutput.cpp
    src/audio/ReplayGain.cpp
    src/audio/RingBuffer.cpp
    src/audio/SampleConvert.cpp
    src/AudioChunk.cpp
    src/AudioFormat.cpp
    src/library/Library.cpp
    src/library/Playlist.cpp
    src/library/PlaylistEntry.cpp
    src/library/PlaylistFile.cpp
    src/library/PluginCache.cpp
    src/library/PropertyList.cpp
    src/library/Scanner.cpp
    src/library/Sqlite.cpp
    src/MetadataMap.cpp
    src/NaturalOrder.cpp
    src/Sha256.cpp
    src/MultiDecoder.cpp
    src/PluginRegistry.cpp
    src/Settings.cpp
    src/Url.cpp
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
    set(XPCOG_SQLITE SQLite::SQLite3)
endif()

target_link_libraries(xpcog-core
    PUBLIC  vendor-miniaudio
    PRIVATE vendor-hdcd
    PRIVATE ${XPCOG_SQLITE}
    PRIVATE soxr::soxr XPCog::warnings)

set_target_properties(xpcog-core PROPERTIES FOLDER core)

# --------------------------------------------------------------------------
# xpcog-core must never link Qt. This is not a style rule -- it is what keeps
# the engine embeddable, headless-testable, and usable from xpcog-cli.
#
# The primary enforcement is structural: xpcog-cli links no Qt, so a QtCore leak
# breaks that build immediately. This check catches it earlier, with a clearer
# message, and runs on every build rather than only when the CLI is enabled.
# --------------------------------------------------------------------------
add_custom_target(xpcog-core-no-qt ALL
    COMMAND "${CMAKE_COMMAND}"
            -D "XPCOG_CORE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            -P "${CMAKE_SOURCE_DIR}/cmake/CheckNoQt.cmake"
    COMMENT "Checking that xpcog-core stays Qt-free"
    VERBATIM)
add_dependencies(xpcog-core xpcog-core-no-qt)
set_target_properties(xpcog-core-no-qt PROPERTIES FOLDER core)
