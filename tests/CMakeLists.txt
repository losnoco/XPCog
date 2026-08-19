find_package(Catch2 3 REQUIRED)

add_executable(xpcog-tests
    core/test_audio.cpp
    core/test_dsp.cpp
    core/test_fft.cpp
    core/test_realfft.cpp
    core/test_freesurround.cpp
    core/test_gapless.cpp
    core/test_m1c.cpp
    core/test_library.cpp
    core/test_loop_policy.cpp
    core/test_playlist.cpp
    core/test_paths.cpp
    core/test_playlistfile.cpp
    core/test_plugin_registry.cpp
    core/test_value_types.cpp
    core/test_scanner.cpp
    core/test_seek.cpp
    core/test_spectrum.cpp
    core/test_stream_metadata.cpp
    core/test_transportgain.cpp
    core/test_version.cpp
    codecs/test_codec_settings.cpp
    codecs/test_conformance.cpp
    codecs/test_containers.cpp
    codecs/test_cuesheet.cpp
    codecs/test_gme_sidecar.cpp
    codecs/test_id3v2.cpp
)

# Guarded because the HTTP source is an option: without it the headers under
# codecs/httpsource are not on the include path at all.
if(XPCOG_WITH_HTTP)
    target_sources(xpcog-tests PRIVATE codecs/test_httpsource.cpp)
endif()

# The mid-stream tag tests build their own MPEG-TS fixture with libavformat:
# only the mpegts demuxer ever produces a timed-ID3 stream, so there is nothing
# to feed the muxer from through the ffmpeg binary.
if(XPCOG_WITH_MINIMP3)
    target_sources(xpcog-tests PRIVATE codecs/test_mp3.cpp)

    # Gapless is decided by fields no synthetic fixture can vouch for: a test
    # that writes the tag it then parses proves only that it is self-consistent.
    # FFmpeg's FATE suite has real iTunes and LAME encodes, and it is not ours
    # to vendor -- so point at a copy that already exists on the machine:
    #   -DXPCOG_FATE_SUITE=/path/to/fate-suite
    # Unset, the cases skip and every other clone is unaffected.
    set(XPCOG_FATE_SUITE "" CACHE PATH
        "FFmpeg FATE suite, for decode fixtures that cannot be committed; empty to skip")
    if(XPCOG_FATE_SUITE)
        target_compile_definitions(xpcog-tests
            PRIVATE "XPCOG_FATE_SUITE=\"${XPCOG_FATE_SUITE}\"")
        message(STATUS "FATE suite tests: ${XPCOG_FATE_SUITE}")
    endif()
endif()

if(XPCOG_WITH_FLAC)
    target_sources(xpcog-tests PRIVATE codecs/test_flac_chained.cpp)
endif()

if(XPCOG_WITH_VORBIS AND XPCOG_WITH_OPUS AND XPCOG_WITH_OGGCHAIN)
    target_sources(xpcog-tests PRIVATE codecs/test_ogg_chained.cpp)
endif()

if(XPCOG_WITH_FFMPEG)
    find_package(FFMPEG REQUIRED)
    target_sources(xpcog-tests PRIVATE codecs/test_ffmpeg_stream_tags.cpp)
    target_include_directories(xpcog-tests SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
endif()

# Guarded for the same reason as the HTTP source: without HLS the headers under
# codecs/hls are not on the include path.
if(XPCOG_WITH_HLS)
    target_sources(xpcog-tests PRIVATE codecs/test_hls.cpp)
endif()

if(XPCOG_WITH_VGMSTREAM)
    target_sources(xpcog-tests PRIVATE
        codecs/test_vgmstream.cpp
        codecs/test_vgmstream_corpus.cpp)

    # Every fixture these formats need is a rip of copyrighted game audio, so
    # none of it can live in this repository -- which left the decoder with no
    # automated decode coverage at all, and its worst bug (every rip decoding to
    # zero frames, silently) was exactly what a decode test catches first.
    #
    # So point the suite at a corpus that already exists on the machine running
    # it:  -DXPCOG_VGM_CORPUS=D:/vgm
    #
    # Unset, the cases skip. CI and every other clone are unaffected and nothing
    # copyrighted is committed, downloaded or referenced.
    set(XPCOG_VGM_CORPUS "" CACHE PATH
        "Folder of game rips to decode in the vgmstream tests; empty to skip")
    if(XPCOG_VGM_CORPUS)
        target_compile_definitions(xpcog-tests
            PRIVATE "XPCOG_VGM_CORPUS=\"${XPCOG_VGM_CORPUS}\"")
        message(STATUS "vgmstream corpus tests: ${XPCOG_VGM_CORPUS}")
    endif()
endif()

if(XPCOG_WITH_MIDI)
    target_sources(xpcog-tests PRIVATE codecs/test_midi_container.cpp)
    target_link_libraries(xpcog-tests PRIVATE xpcog-codec-midi)

    # Its own corpus variable, like the SID one and for the same reason: a MIDI
    # collection is not a folder of rips of anything else. The formats past
    # Standard MIDI -- HMI, Doom's MUS, Loudness LDS -- cannot be synthesised
    # convincingly by a test and cannot be committed, so breadth is opt-in.
    set(XPCOG_MIDI_CORPUS "" CACHE PATH
        "Folder of MIDI files for the container tests; empty to skip")
    if(XPCOG_MIDI_CORPUS)
        target_compile_definitions(xpcog-tests
            PRIVATE "XPCOG_MIDI_CORPUS=\"${XPCOG_MIDI_CORPUS}\"")
        message(STATUS "MIDI corpus tests: ${XPCOG_MIDI_CORPUS}")
    endif()
endif()

if(XPCOG_WITH_APL)
    # The fixtures are FLAC, not Monkey's Audio: an .apl names a range in a file
    # the registry opens, so what codec answers is not the thing under test, and
    # FLAC's encoder is one every platform already has.
    target_sources(xpcog-tests PRIVATE codecs/test_apl.cpp)
endif()

if(XPCOG_WITH_MUSEPACK)
    target_sources(xpcog-tests PRIVATE codecs/test_musepack.cpp)

    # The encoder comes from the same port as the decoder and is named by its
    # full path rather than looked up on PATH, which is what every other codec
    # test does. There is nothing to look up: Musepack has no packaged encoder
    # for any platform here, so ports/libmpcdec builds mpcenc under its `tools`
    # feature and it lands in the manifest's own vcpkg_installed tree. Naming it
    # also means the fixture is encoded by exactly the revision the decoder was
    # built from.
    set(_mpcenc "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/libmpcdec/mpcenc${CMAKE_EXECUTABLE_SUFFIX}")
    if(EXISTS "${_mpcenc}")
        target_compile_definitions(xpcog-tests PRIVATE "XPCOG_MPCENC=\"${_mpcenc}\"")
    else()
        message(STATUS "mpcenc not found at ${_mpcenc}; Musepack fixtures will skip")
    endif()
endif()

if(XPCOG_WITH_SID)
    target_sources(xpcog-tests PRIVATE codecs/test_sid.cpp)

    # Its own corpus variable rather than XPCOG_PSF_CORPUS: a SID collection is
    # not a folder of rips of anything else, and HVSC is the one everybody has.
    set(XPCOG_SID_CORPUS "" CACHE PATH
        "Folder of .sid files for the SID tests; empty to skip")
    if(XPCOG_SID_CORPUS)
        target_compile_definitions(xpcog-tests
            PRIVATE "XPCOG_SID_CORPUS=\"${XPCOG_SID_CORPUS}\"")
        message(STATUS "SID corpus tests: ${XPCOG_SID_CORPUS}")
    endif()
endif()

if(XPCOG_WITH_PSF)
    target_sources(xpcog-tests PRIVATE codecs/test_psf.cpp codecs/test_usf.cpp
                                       codecs/test_gsf.cpp
                                       codecs/test_twosf.cpp
                                       codecs/test_snsf.cpp
                                       codecs/test_sdsf.cpp
                                       codecs/test_ncsf.cpp
                                       codecs/test_psx.cpp
                                       codecs/test_qsf.cpp)
    target_link_libraries(xpcog-tests PRIVATE xpcog-codec-psf)

    # Same opt-in shape as the vgmstream corpus, and for the same reason: a PSF
    # is a rip of a game's sound program and cannot be committed. Point it at a
    # collection that already exists -- -DXPCOG_PSF_CORPUS=D:/vgm -- and the
    # lib-chain cases run; unset, they skip.
    set(XPCOG_PSF_CORPUS "" CACHE PATH
        "Folder of PSF rips for the container tests; empty to skip")
    if(XPCOG_PSF_CORPUS)
        target_compile_definitions(xpcog-tests
            PRIVATE "XPCOG_PSF_CORPUS=\"${XPCOG_PSF_CORPUS}\"")
        message(STATUS "PSF corpus tests: ${XPCOG_PSF_CORPUS}")
    endif()
endif()

# Likewise the archive codec: these include its headers and link its objects.
if(XPCOG_WITH_ARCHIVE)
    target_sources(xpcog-tests PRIVATE
        codecs/test_archive.cpp
        codecs/test_unpackurl.cpp)
endif()

target_link_libraries(xpcog-tests PRIVATE
    xpcog-core
    xpcog-codecs
    Catch2::Catch2WithMain
    XPCog::warnings)

# Where the golden captures live. Passed as a path rather than copied beside the
# binary: the fixtures are inputs to a comparison, not build outputs, and a copy
# is one more thing that can silently be stale.
target_compile_definitions(xpcog-tests
    PRIVATE "XPCOG_GOLDEN_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/golden\"")

set_target_properties(xpcog-tests PROPERTIES FOLDER tests)

include(Catch)
catch_discover_tests(xpcog-tests)

# ---------------------------------------------------------------------------
# The Qt half. Separate because the headless preset builds no app at all, and
# because these need a QCoreApplication that the core tests must not have.
# ---------------------------------------------------------------------------
if(XPCOG_BUILD_APP)
    add_executable(xpcog-app-tests
        app/main.cpp
        app/QtStringMaker.hpp
        app/test_appicon.cpp
        app/test_infopanel.cpp
        app/test_lucideicon.cpp
        app/test_openurldialog.cpp
        app/test_playlistcommands.cpp
        app/test_playlistmodel.cpp
        app/test_scantask.cpp
        app/test_singleinstance.cpp
        app/test_translations.cpp
    )

    target_link_libraries(xpcog-app-tests PRIVATE
        xpcog-appcore
        Catch2::Catch2
        XPCog::warnings)

    set_target_properties(xpcog-app-tests PROPERTIES
        AUTOMOC ON
        FOLDER tests)

    # The compiled Spanish catalogue, so the i18n tests read a real .qm rather
    # than re-parsing the .ts they are meant to be checking the output of.
    foreach(_qm IN LISTS XPCOG_QM_FILES)
        if(_qm MATCHES "_es\\.qm$")
            target_compile_definitions(xpcog-app-tests PRIVATE "XPCOG_QM_ES=\"${_qm}\"")
        endif()
    endforeach()
    add_dependencies(xpcog-app-tests release_translations)

    # Windows resolves DLLs from PATH, having no RPATH equivalent. Test
    # discovery *runs* this executable to enumerate its cases, and it cannot
    # start without Qt's DLLs reachable -- the build failed at that step with
    # 0xc0000135 (STATUS_DLL_NOT_FOUND) rather than at link time, which makes it
    # look like a Catch2 fault instead of a missing library. vcpkg's applocal
    # step copies the vcpkg DLLs next to the binary, but Qt deliberately does
    # not come from vcpkg, so nothing copies Qt's.
    #
    # DL_PATHS is Catch2's hook for precisely this: it prepends to PATH both
    # when listing the tests and when ctest runs them. macOS and Linux need
    # nothing here, where the RPATH baked into the binary already resolves Qt.
    if(WIN32)
        # Qt6_DIR is <prefix>/lib/cmake/Qt6; the same relative walk the
        # windeployqt lookup in app/CMakeLists.txt uses.
        get_filename_component(_xpcog_qt_bin "${Qt6_DIR}/../../../bin" ABSOLUTE)
        catch_discover_tests(xpcog-app-tests DL_PATHS "${_xpcog_qt_bin}")
    else()
        catch_discover_tests(xpcog-app-tests)
    endif()
endif()
