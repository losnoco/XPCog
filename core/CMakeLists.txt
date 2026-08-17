configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/xpcog/core/Version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/xpcog/core/Version.hpp"
    @ONLY)

add_library(xpcog-core STATIC
    src/PluginRegistry.cpp
    src/Version.cpp
)
add_library(XPCog::core ALIAS xpcog-core)

target_include_directories(xpcog-core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_BINARY_DIR}/include")

target_link_libraries(xpcog-core PRIVATE XPCog::warnings)

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
