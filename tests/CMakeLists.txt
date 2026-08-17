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
        app/test_playlistmodel.cpp
    )

    target_link_libraries(xpcog-app-tests PRIVATE
        xpcog-appcore
        Catch2::Catch2
        XPCog::warnings)

    set_target_properties(xpcog-app-tests PROPERTIES
        AUTOMOC ON
        FOLDER tests)

    catch_discover_tests(xpcog-app-tests)
endif()
