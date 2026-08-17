find_package(Catch2 3 REQUIRED)

add_executable(xpcog-tests
    core/test_audio.cpp
    core/test_gapless.cpp
    core/test_m1c.cpp
    core/test_library.cpp
    core/test_playlist.cpp
    core/test_playlistfile.cpp
    core/test_plugin_registry.cpp
    core/test_value_types.cpp
    core/test_scanner.cpp
    core/test_seek.cpp
    core/test_version.cpp
    codecs/test_conformance.cpp
    codecs/test_containers.cpp
    codecs/test_cuesheet.cpp
)

target_link_libraries(xpcog-tests PRIVATE
    xpcog-core
    xpcog-codecs
    Catch2::Catch2WithMain
    XPCog::warnings)

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
        app/test_playlistcommands.cpp
        app/test_playlistmodel.cpp
        app/test_scantask.cpp
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

    catch_discover_tests(xpcog-app-tests)
endif()
