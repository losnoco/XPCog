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
    core/test_playlist.cpp
    core/test_playlistfile.cpp
    core/test_plugin_registry.cpp
    core/test_value_types.cpp
    core/test_scanner.cpp
    core/test_seek.cpp
    core/test_spectrum.cpp
    core/test_stream_metadata.cpp
    core/test_transportgain.cpp
    core/test_version.cpp
    codecs/test_conformance.cpp
    codecs/test_containers.cpp
    codecs/test_cuesheet.cpp
    codecs/test_gme_sidecar.cpp
)

# Guarded because the HTTP source is an option: without it the headers under
# codecs/httpsource are not on the include path at all.
if(XPCOG_WITH_HTTP)
    target_sources(xpcog-tests PRIVATE codecs/test_httpsource.cpp)
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
