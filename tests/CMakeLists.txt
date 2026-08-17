find_package(Catch2 3 REQUIRED)

add_executable(xpcog-tests
    core/test_audio.cpp
    core/test_gapless.cpp
    core/test_plugin_registry.cpp
    core/test_value_types.cpp
    core/test_version.cpp
    codecs/test_conformance.cpp
    codecs/test_containers.cpp
)

target_link_libraries(xpcog-tests PRIVATE
    xpcog-core
    xpcog-codecs
    Catch2::Catch2WithMain
    XPCog::warnings)

set_target_properties(xpcog-tests PROPERTIES FOLDER tests)

include(Catch)
catch_discover_tests(xpcog-tests)
