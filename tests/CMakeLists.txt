find_package(Catch2 3 REQUIRED)

add_executable(xpcog-tests
    core/test_assetpath.cpp
    core/test_audio.cpp
    core/test_dsd.cpp
    core/test_dsp.cpp
    core/test_eqpresets.cpp
    core/test_fft.cpp
    core/test_realfft.cpp
    core/test_freesurround.cpp
    core/test_gapless.cpp
    core/test_m1c.cpp
    core/test_oscilloscope.cpp
    core/test_output_device.cpp
    core/test_panelfeed.cpp
    core/test_library.cpp
    core/test_loop_policy.cpp
    core/test_lastfm.cpp
    core/test_lastfm_live.cpp
    core/test_listenbrainz.cpp
    core/test_lrc.cpp
    core/test_lrclib.cpp
    core/test_md5.cpp
    core/test_playmonitor.cpp
    core/test_scrobbler.cpp
    core/test_playlist.cpp
    core/test_paths.cpp
    core/test_playlistcommands.cpp
    core/test_playlistfile.cpp
    core/test_playlistview.cpp
    core/test_plugin_registry.cpp
    core/test_value_types.cpp
    core/test_scanner.cpp
    core/test_scantask.cpp
    core/test_serialexecutor.cpp
    core/test_seek.cpp
    core/test_undostack.cpp
    core/test_spectrum.cpp
    core/test_stream_metadata.cpp
    core/test_timestretch.cpp
    core/test_transportgain.cpp
    core/test_version.cpp
    core/test_waveform.cpp
    codecs/test_codec_settings.cpp
    codecs/test_conformance.cpp
    codecs/test_containers.cpp
    codecs/test_cuesheet.cpp
    codecs/test_gme_sidecar.cpp
    codecs/test_id3v2.cpp
    core/test_equalizerpreset.cpp
    core/test_remote_server.cpp
    core/test_remote_token.cpp
)

# The remote-control suite is compiled either way -- the null twin is worth a
# case of its own -- so the option arrives as a define rather than as a source
# list. The cases that need a server are behind it; the one that asks whether
# this build has one is not.
if(XPCOG_WITH_REST)
    target_compile_definitions(xpcog-tests PRIVATE XPCOG_HAS_REST=1)
    # The gate is a private header beside the server, and the suite reaches it
    # directly: what it does is worth testing on its own, and going through
    # handle() would test the router instead.
    target_sources(xpcog-tests PRIVATE core/test_remote_assets.cpp
                                     core/test_remote_gate.cpp
                                     core/test_remote_openapi.cpp
                                     core/test_remote_router.cpp
                                     core/test_remote_socket.cpp)
    # The socket test speaks to the server with httplib's own client, which is
    # the same library the server uses and therefore already available.
    target_link_libraries(xpcog-tests PRIVATE httplib::httplib)
    # The MANIFEST is read at run time rather than baked in, so the test fails
    # when the file and the embedded bytes disagree rather than when someone
    # remembers to update a constant.
    target_compile_definitions(xpcog-tests PRIVATE
        XPCOG_SWAGGER_DIR="${CMAKE_SOURCE_DIR}/assets/swagger-ui")
    target_include_directories(xpcog-tests PRIVATE "${CMAKE_SOURCE_DIR}/core/src/remote")
endif()

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
    target_sources(xpcog-tests PRIVATE codecs/test_flac.cpp)
endif()

if(XPCOG_WITH_HIVELY)
    target_sources(xpcog-tests PRIVATE codecs/test_hively.cpp)

    # Its own corpus variable, for the same reason SID has one: an AHX
    # collection is a different thing from a PSF rip archive, and pointing one
    # at the other only produces skips that read like failures.
    #   -DXPCOG_HIVELY_CORPUS=/path/to/modules
    # Unset, the corpus cases skip and the registration and rejection ones still
    # run.
    set(XPCOG_HIVELY_CORPUS "" CACHE PATH
        "AHX and HVL modules, for decode fixtures that cannot be committed; empty to skip")
    if(XPCOG_HIVELY_CORPUS)
        target_compile_definitions(xpcog-tests
            PRIVATE "XPCOG_HIVELY_CORPUS=\"${XPCOG_HIVELY_CORPUS}\"")
        message(STATUS "Hively corpus tests: ${XPCOG_HIVELY_CORPUS}")
    endif()
endif()

if(XPCOG_WITH_ADPLUG)
    target_sources(xpcog-tests PRIVATE codecs/test_adplug.cpp)

    # Its own corpus variable. An AdLib collection is a different thing again
    # from a tracker one, and the formats these tests separate -- .raw against
    # vgmstream, .s3m against OpenMPT -- only exist to be separated where both
    # kinds of file are present.
    #   -DXPCOG_ADPLUG_CORPUS=/path/to/adlib
    set(XPCOG_ADPLUG_CORPUS "" CACHE PATH
        "AdLib and OPL2 modules, for decode fixtures that cannot be committed; empty to skip")
    if(XPCOG_ADPLUG_CORPUS)
        target_compile_definitions(xpcog-tests
            PRIVATE "XPCOG_ADPLUG_CORPUS=\"${XPCOG_ADPLUG_CORPUS}\"")
        message(STATUS "AdPlug corpus tests: ${XPCOG_ADPLUG_CORPUS}")
    endif()
endif()

if(XPCOG_WITH_ORGANYA)
    target_sources(xpcog-tests PRIVATE codecs/test_organya.cpp)

    # Most of the Organya cases build their own songs and need nothing, which
    # is unusual enough to be worth saying: the format is documented and
    # uncompressed, so a fixture can be written rather than found. This variable
    # only widens the same claims over real music.
    #   -DXPCOG_ORGANYA_CORPUS=/path/to/org
    set(XPCOG_ORGANYA_CORPUS "" CACHE PATH
        "Organya (.org) songs, for breadth over real music; empty to skip those cases")
    if(XPCOG_ORGANYA_CORPUS)
        target_compile_definitions(xpcog-tests
            PRIVATE "XPCOG_ORGANYA_CORPUS=\"${XPCOG_ORGANYA_CORPUS}\"")
        message(STATUS "Organya corpus tests: ${XPCOG_ORGANYA_CORPUS}")
    endif()
endif()

if(XPCOG_WITH_SILENCE)
    # No fixture and no corpus: the format has no bytes.
    target_sources(xpcog-tests PRIVATE codecs/test_silence.cpp)
endif()

if(XPCOG_WITH_FFMPEG)
    target_sources(xpcog-tests PRIVATE codecs/test_shorten.cpp)

    # Shorten is FFmpeg's to decode; what is ours is the length, and the length
    # parser is tested against files this builds itself. The variable only adds
    # the end-to-end case, and it names one file rather than a corpus because
    # one is all it takes.
    #   -DXPCOG_SHORTEN_FILE=/path/to/something.shn
    set(XPCOG_SHORTEN_FILE "" CACHE FILEPATH
        "A .shn file, for the end-to-end Shorten case; empty to skip it")
    if(XPCOG_SHORTEN_FILE)
        target_compile_definitions(xpcog-tests
            PRIVATE "XPCOG_SHORTEN_FILE=\"${XPCOG_SHORTEN_FILE}\"")
        message(STATUS "Shorten file tests: ${XPCOG_SHORTEN_FILE}")
    endif()
endif()

if(XPCOG_WITH_SYNTRAX)
    target_sources(xpcog-tests PRIVATE codecs/test_syntrax.cpp)

    # Unlike Organya, a .jxs cannot reasonably be written by a test -- the
    # header is a packed struct of counts, followed by whole subsong, pattern
    # and instrument structures -- so everything but registration and rejection
    # needs a collection.
    #   -DXPCOG_SYNTRAX_CORPUS=/path/to/jxs
    set(XPCOG_SYNTRAX_CORPUS "" CACHE PATH
        "Syntrax (.jxs) modules, for decode fixtures that cannot be committed; empty to skip")
    if(XPCOG_SYNTRAX_CORPUS)
        target_compile_definitions(xpcog-tests
            PRIVATE "XPCOG_SYNTRAX_CORPUS=\"${XPCOG_SYNTRAX_CORPUS}\"")
        message(STATUS "Syntrax corpus tests: ${XPCOG_SYNTRAX_CORPUS}")
    endif()
endif()

if(XPCOG_WITH_VORBIS AND XPCOG_WITH_OPUS AND XPCOG_WITH_OGGCHAIN)
    target_sources(xpcog-tests PRIVATE codecs/test_ogg_chained.cpp)
endif()

if(XPCOG_WITH_FFMPEG)
    xpcog_system_dep(ffmpeg _ffmpeg_from_system)
    if(NOT _ffmpeg_from_system)
        find_package(FFMPEG REQUIRED)
    endif()
    target_sources(xpcog-tests PRIVATE codecs/test_ffmpeg_stream_tags.cpp
                                       codecs/test_ffmpeg_format_change.cpp)
    target_include_directories(xpcog-tests SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
endif()

# Guarded for the same reason as the HTTP source: without HLS the headers under
# codecs/hls are not on the include path.
if(XPCOG_WITH_HLS)
    target_sources(xpcog-tests PRIVATE codecs/test_hls.cpp)
endif()

# A folder of game rips, read by two codecs' tests.
#
# Every fixture these formats need is a rip of copyrighted game audio, so none
# of it can live in this repository -- which left the vgmstream decoder with no
# automated decode coverage at all, and its worst bug (every rip decoding to
# zero frames, silently) was exactly what a decode test catches first.
#
# So point the suite at a corpus that already exists on the machine running
# it:  -DXPCOG_VGM_CORPUS=D:/vgm
#
# Declared out here rather than inside either codec's block because both
# vgmstream and libvgm read it: a folder of rips holds streamed audio and chip
# logs side by side, and asking for the same tree twice under two names would
# only produce skips that read like failures.
#
# Unset, the cases skip. CI and every other clone are unaffected and nothing
# copyrighted is committed, downloaded or referenced.
set(XPCOG_VGM_CORPUS "" CACHE PATH
    "Folder of game rips to decode in the vgmstream and libvgm tests; empty to skip")
if(XPCOG_VGM_CORPUS)
    target_compile_definitions(xpcog-tests
        PRIVATE "XPCOG_VGM_CORPUS=\"${XPCOG_VGM_CORPUS}\"")
    message(STATUS "Game-rip corpus tests: ${XPCOG_VGM_CORPUS}")
endif()

if(XPCOG_WITH_VGMSTREAM)
    target_sources(xpcog-tests PRIVATE
        codecs/test_vgmstream.cpp
        codecs/test_vgmstream_corpus.cpp)
endif()

if(XPCOG_WITH_LIBVGM)
    target_sources(xpcog-tests PRIVATE codecs/test_libvgm.cpp)
endif()

if(XPCOG_WITH_DSD)
    target_sources(xpcog-tests PRIVATE codecs/test_dsd_containers.cpp)

    # A folder of DSD files rather than one, because the case that carries the
    # most needs a `.dsf` and a `.wv` of the *same* recording sitting beside each
    # other -- WavPack's DSD path is the only reference this tree has for what
    # bits a DSD decoder should produce.
    #   -DXPCOG_DSD_CORPUS=/path/to/dsd
    set(XPCOG_DSD_CORPUS "" CACHE PATH
        "Folder of DSF and DSDIFF files; empty to skip")
    if(XPCOG_DSD_CORPUS)
        target_compile_definitions(xpcog-tests
            PRIVATE "XPCOG_DSD_CORPUS=\"${XPCOG_DSD_CORPUS}\"")
        message(STATUS "DSD corpus tests: ${XPCOG_DSD_CORPUS}")
    endif()
endif()

# One DSD file, for the case that proves a decoder actually reports the format.
# A `.wv` carrying DSD is what this was written against; an SACD rip is several
# gigabytes and is nobody's to commit.
set(XPCOG_DSD_FILE "" CACHE FILEPATH
    "A DSD file for the conversion tests; empty to skip")
if(XPCOG_DSD_FILE)
    target_compile_definitions(xpcog-tests PRIVATE "XPCOG_DSD_FILE=\"${XPCOG_DSD_FILE}\"")
    message(STATUS "DSD file tests: ${XPCOG_DSD_FILE}")
endif()

if(XPCOG_WITH_MIDI)
    target_sources(xpcog-tests PRIVATE codecs/test_midi_container.cpp
                                       codecs/test_midi_playback.cpp
                                       codecs/test_midi_sc55.cpp
                                       codecs/test_midi_soundfont.cpp)
    target_link_libraries(xpcog-tests PRIVATE xpcog-codec-midi)

    # The Roland ROM set, which cannot be committed and which nobody can be
    # pointed at. Either the folder holding the five files or the archive they
    # arrived in -- the loader takes both, and which one this names decides
    # which of its two paths the tests exercise.
    set(XPCOG_SC55_ROMS "" CACHE PATH
        "SC-55 ROM folder or archive for the MIDI tests; empty to skip")
    if(XPCOG_SC55_ROMS)
        target_compile_definitions(xpcog-tests
            PRIVATE "XPCOG_SC55_ROMS=\"${XPCOG_SC55_ROMS}\"")
        message(STATUS "SC-55 ROM tests: ${XPCOG_SC55_ROMS}")
    endif()

    # A SoundFont, which is the one asset of the three that a listener plausibly
    # already has and still cannot be committed -- the bank this was written
    # against is 1.3 GB. Anything the engine reads will do: .sf2, .sf3, .dls or
    # an .sflist naming several.
    set(XPCOG_SOUNDFONT "" CACHE FILEPATH
        "SoundFont bank for the MIDI tests; empty to skip")
    if(XPCOG_SOUNDFONT)
        target_compile_definitions(xpcog-tests
            PRIVATE "XPCOG_SOUNDFONT=\"${XPCOG_SOUNDFONT}\"")
        message(STATUS "SoundFont tests: ${XPCOG_SOUNDFONT}")
    endif()

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
    #
    # Unless the decoder came from the distribution rather than from the port, in
    # which case there is no vcpkg tools directory to name and PATH is the only
    # place left to look -- Debian packages the encoder as `musepack-tools`, and
    # where a distribution does not, the fixtures skip as they did before. Same
    # fixture when it is there; what is lost is the guarantee that the encoder and
    # the decoder came from one revision.
    set(_mpcenc "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/libmpcdec/mpcenc${CMAKE_EXECUTABLE_SUFFIX}")
    if(NOT EXISTS "${_mpcenc}")
        find_program(XPCOG_MPCENC_PROGRAM NAMES mpcenc)
        set(_mpcenc "${XPCOG_MPCENC_PROGRAM}")
    endif()
    if(_mpcenc AND EXISTS "${_mpcenc}")
        target_compile_definitions(xpcog-tests PRIVATE "XPCOG_MPCENC=\"${_mpcenc}\"")
    else()
        message(STATUS "mpcenc not found in vcpkg's tools directory or on PATH; "
                       "Musepack fixtures will skip")
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

# The shipped bank is found relative to the test binary, so it has to be there
# before the tests run -- and CTest runs them straight after a build.
add_dependencies(xpcog-tests xpcog-assets)

include(Catch)
catch_discover_tests(xpcog-tests)

# ---------------------------------------------------------------------------
# The toolkit-free half of the application layer, tested without one.
#
# A separate binary, and the point is what it links: xpcog-uicore and nothing
# else. If something under uicore/ grows a dependency on a widget this fails to
# link, which makes the extraction a fact about the build rather than a claim in
# a comment -- the same trade cmake/CheckNoToolkit.cmake makes, arrived at from
# the other side.
#
# Gated on the player, like uicore/ itself, and built under either frontend.
# ---------------------------------------------------------------------------
if(XPCOG_BUILD_APP)
    add_executable(xpcog-uicore-tests
        uicore/test_appcommands.cpp
        uicore/test_commands.cpp
        uicore/test_crashreporter.cpp
        uicore/test_credits.cpp
        uicore/test_lyricstext.cpp
        uicore/test_remotejobs.cpp
        uicore/test_session.cpp
        uicore/test_settingeffect.cpp
        uicore/test_translations.cpp
    )
    # codecs as well as uicore, and that keeps the rule rather than bending it:
    # the point is that no *toolkit* is linked, and codecs link none. Without
    # them the session test could scan nothing and play nothing.
    target_link_libraries(xpcog-uicore-tests
        PRIVATE xpcog-uicore xpcog-codecs Catch2::Catch2WithMain XPCog::warnings)
    # So the crash reporter's availability case can assert rather than accept
    # either answer. The test cannot ask the build system at run time, and a
    # case that passes both ways is one that would not notice the reporter
    # quietly dropping out of the build.
    if(XPCOG_WITH_SENTRY)
        target_compile_definitions(xpcog-uicore-tests PRIVATE XPCOG_TESTS_EXPECT_SENTRY=1)
    endif()
    add_dependencies(xpcog-uicore-tests xpcog-assets)
    set_target_properties(xpcog-uicore-tests PROPERTIES FOLDER tests)
    catch_discover_tests(xpcog-uicore-tests)
endif()

# ---------------------------------------------------------------------------
# The WinUI player, opened.
#
# Its windows built and laid out with the real runtime: every pane the View
# menu shows, Preferences page by page, the mini player and the About box. See
# winui/test_winui.cpp for how it runs beside the XAML loop. Registered by hand
# as one test, like the GTK suite below and for the same reason: the cases share
# one application, which discovery would start once per case.
#
# Exit code 4 is a skip: no Windows App Runtime on the machine, so no window can
# be made. CI installs the runtime first, so there it runs.
# ---------------------------------------------------------------------------
if(XPCOG_BUILD_WINUI_APP)
    add_executable(xpcog-winui-tests winui/test_winui.cpp)
    # The panel photograph only exists in a MIDI build, and so does the case
    # that checks it is still compiled in.
    if(XPCOG_WITH_MIDI)
        target_sources(xpcog-winui-tests PRIVATE winui/test_sc55panel.cpp)
    endif()
    target_link_libraries(xpcog-winui-tests PRIVATE
        xpcog-winuicore
        Catch2::Catch2
        XPCog::warnings)
    set(XPCOG_WINUI_MANIFEST "${CMAKE_SOURCE_DIR}/app-winui/XPCog.manifest")
    configure_file(winui/manifest.rc.in "${CMAKE_CURRENT_BINARY_DIR}/winui-manifest.rc" @ONLY)
    target_sources(xpcog-winui-tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/winui-manifest.rc")
    set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/winui-manifest.rc"
        APPEND PROPERTY OBJECT_DEPENDS "${XPCOG_WINUI_MANIFEST}")
    target_link_options(xpcog-winui-tests PRIVATE "/MANIFEST:NO")
    set_target_properties(xpcog-winui-tests PROPERTIES FOLDER tests)
    # The player stages what the windows load beside it -- the bootstrapper,
    # Win2D, the icon -- in the bin/ this lands in too.
    add_dependencies(xpcog-winui-tests xpcog-winui xpcog-assets)

    add_test(NAME "the WinUI windows" COMMAND "$<TARGET_FILE:xpcog-winui-tests>")
    set_tests_properties("the WinUI windows" PROPERTIES SKIP_RETURN_CODE 4)
endif()

# ---------------------------------------------------------------------------
# The one suite that wants a screen: the GTK frontend's.
#
# Everything above is deliberately free of an application object, and this is
# what could not be: a layout that recurses, a control squeezed below its own
# minimum, a painted pane that draws nothing -- none of them exists until a
# toolkit is handing real widgets real sizes. So the preferences dialog is
# walked page by page, the equaliser's scales measured, the painted panes run
# for a few frames, and GTK's own warnings are part of the verdict.
#
# Registered by hand rather than with catch_discover_tests(): discovery
# generates its own add_test() calls and has nowhere to put a launcher, so a
# discovered test would run bare -- passing on a developer's desktop and
# skipping in every headless CI job. One entry, run under Xvfb where there is
# one. Linux only because that is where the GTK frontend is, and where a
# display can be conjured.
# ---------------------------------------------------------------------------
if(XPCOG_BUILD_GTK_APP)
    add_executable(xpcog-gtk-tests gtk/test_gtkwidgets.cpp)

    target_link_libraries(xpcog-gtk-tests PRIVATE
        xpcog-gtkcore
        Catch2::Catch2WithMain
        XPCog::warnings)

    set_target_properties(xpcog-gtk-tests PROPERTIES FOLDER tests)

    find_program(XPCOG_XVFB_RUN NAMES xvfb-run)
    if(XPCOG_XVFB_RUN)
        # -a picks a free server number, so parallel ctest jobs do not collide
        # over :99.
        add_test(NAME "the GTK widgets that need a screen"
                 COMMAND "${XPCOG_XVFB_RUN}" -a "$<TARGET_FILE:xpcog-gtk-tests>")
    elseif(DEFINED ENV{CI})
        # A developer without Xvfb has a desktop; a runner without it has
        # nothing, and the case would skip in a job that stayed green -- the
        # shape of coverage quietly going away. So this is the one
        # configuration where a missing tool is an error.
        message(FATAL_ERROR
            "xvfb-run not found, and CI is set: the GTK tests would skip rather "
            "than run. Install xvfb.")
    else()
        add_test(NAME "the GTK widgets that need a screen"
                 COMMAND "$<TARGET_FILE:xpcog-gtk-tests>")
    endif()
    set_tests_properties("the GTK widgets that need a screen"
                         PROPERTIES SKIP_RETURN_CODE 4)
    if(XPCOG_XVFB_RUN)
        # Under Xvfb the display is X, and GTK has to be told so: with
        # GDK_BACKEND unset it tries Wayland first, and on a Wayland desktop
        # the test windows open on the real screen. The suite pins this itself
        # when it is unset; said here too. The lock is for running beside
        # anything else that starts an Xvfb.
        set_tests_properties("the GTK widgets that need a screen"
                             PROPERTIES ENVIRONMENT_MODIFICATION "GDK_BACKEND=set:x11"
                                        RESOURCE_LOCK xvfb)
    endif()
endif()
