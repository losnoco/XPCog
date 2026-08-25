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
    src/audio/TimeStretch.cpp
    src/AssetPath.cpp
    src/AudioChunk.cpp
    src/AudioFormat.cpp
    src/library/CogImport.cpp
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
    src/Md5.cpp
    src/net/HttpEncoding.cpp
    src/PlayMonitor.cpp
    src/scrobble/LastFmClient.cpp
    src/scrobble/Scrobbler.cpp
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

# Rubber Band does the time-stretching, as it does in Cog. Meson upstream, so
# no CMake config package exists to find anywhere -- vcpkg and every
# distribution alike install a library and a header, and this is the same shape
# the soxr fallback above takes.
find_library(RUBBERBAND_LIBRARY NAMES rubberband REQUIRED)
find_path(RUBBERBAND_INCLUDE_DIR NAMES rubberband/rubberband-c.h REQUIRED)
add_library(rubberband::rubberband UNKNOWN IMPORTED)
set_target_properties(rubberband::rubberband PROPERTIES
    IMPORTED_LOCATION "${RUBBERBAND_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${RUBBERBAND_INCLUDE_DIR}")

# A static librubberband does not carry its dependencies the way the Windows
# DLL does, so the ones the vcpkg port builds it against are appended when the
# tree has them: SLEEF's DFT and libsamplerate on Linux and x64 Windows, FFTW
# and libsamplerate on Apple platforms. Optional on purpose -- a distribution's
# shared librubberband resolves its own dependencies, and this list finding
# nothing is exactly that case.
foreach(_xpcog_rb_dep sleefdft sleef samplerate fftw3f fftw3)
    string(TOUPPER "${_xpcog_rb_dep}" _xpcog_rb_var)
    find_library(RUBBERBAND_${_xpcog_rb_var}_LIBRARY NAMES ${_xpcog_rb_dep})
    if(RUBBERBAND_${_xpcog_rb_var}_LIBRARY)
        target_link_libraries(rubberband::rubberband
            INTERFACE "${RUBBERBAND_${_xpcog_rb_var}_LIBRARY}")
    endif()
endforeach()

# vcpkg's SLEEF builds its DFT with OpenMP, so the static archive needs the
# runtime as well. The *libraries* only, never OpenMP::OpenMP_CXX: that target
# carries -fopenmp as a compile option, and core is not OpenMP code -- it
# merely links a library that is.
if(RUBBERBAND_SLEEFDFT_LIBRARY)
    find_package(OpenMP QUIET)
    if(OpenMP_CXX_FOUND AND OpenMP_CXX_LIBRARIES)
        target_link_libraries(rubberband::rubberband INTERFACE ${OpenMP_CXX_LIBRARIES})
    endif()
endif()

# SQLite replaces Core Data. Core talks to the C API directly rather than through
# any toolkit's database layer, so the scanner, the CLI and the tests reach the
# library without an application object or an event loop -- and Cog sets the
# precedent with SQLiteStore.m.
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

# nlohmann/json parses the Last.fm API's replies. Header-only, and PRIVATE
# because no public header of ours names it -- LastFmClient.hpp speaks
# std::string and std::optional precisely so the parser stays swappable and the
# dependency stops at core's own translation units.
#
# A library rather than a hand-written parser, which is a decision worth naming
# because the tree does hand-write a plist reader and two hash functions next
# door. The difference is what the input is: those parse files this program
# wrote and buffers whose length it knows, while this one parses whatever a
# remote server sends. That is a fuzzing target, and it is not the place to save
# a dependency that costs a download and no build time.
find_package(nlohmann_json CONFIG REQUIRED)

# libcurl, for the same API calls. Behind XPCOG_WITH_HTTP because that is the
# switch that already decides whether this build can reach the network at all --
# a player built without it has no internet radio either, and scrobbling has
# nowhere to send anything. Exactly one of the two translation units below is
# compiled, so no caller needs an #ifdef; see core/src/net/NullHttpClient.cpp.
if(XPCOG_WITH_HTTP)
    xpcog_system_dep(curl _xpcog_core_curl_from_system)
    if(NOT _xpcog_core_curl_from_system)
        find_package(CURL CONFIG REQUIRED GLOBAL)
    endif()
    target_sources(xpcog-core PRIVATE src/net/CurlHttpClient.cpp)
    target_link_libraries(xpcog-core PRIVATE CURL::libcurl)
else()
    target_sources(xpcog-core PRIVATE src/net/NullHttpClient.cpp)
endif()

target_link_libraries(xpcog-core
    PUBLIC  vendor-miniaudio
    PRIVATE vendor-hdcd
    PRIVATE vendor-dsd2pcm
    PRIVATE vendor-lpc
    # PUBLIC, not PRIVATE: FreeSurround.hpp is a public core header and names
    # channel_setup and channel_id, which are the vendored enums. Anything that
    # can construct a decoder has to be able to name its channel layout.
    PUBLIC  vendor-fsurround
    PRIVATE ${XPCOG_SQLITE}
    PRIVATE soxr::soxr
    PRIVATE rubberband::rubberband
    PRIVATE vendor-signalsmith
    PRIVATE nlohmann_json::nlohmann_json
    PRIVATE XPCog::warnings)

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
