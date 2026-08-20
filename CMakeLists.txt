cmake_minimum_required(VERSION 3.24)

# The vcpkg toolchain must be set before project(); CMakePresets.json does that.
project(XPCog
    VERSION 0.1.0
    DESCRIPTION "Cross-platform Qt port of the Cog audio player"
    LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

include(XPCogOptions)
include(XPCogWarnings)
include(XPCogCodec)
# Before any add_subdirectory that calls find_package(Qt6): it contributes to
# CMAKE_PREFIX_PATH, which has to be set by then.
include(XPCogQt)

# ---------------------------------------------------------------------------
# Layers, innermost first. See docs: core must never link Qt.
# ---------------------------------------------------------------------------
add_subdirectory(vendor)
add_subdirectory(core)
add_subdirectory(codecs)

# Before the executables, so they can depend on the staged copy.
add_subdirectory(assets)

if(XPCOG_BUILD_CLI)
    add_subdirectory(tools/cli)
endif()

if(XPCOG_BUILD_APP)
    add_subdirectory(platform)
    add_subdirectory(app)
endif()

if(XPCOG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "")
message(STATUS "XPCog ${PROJECT_VERSION}")
message(STATUS "  build type ......... ${CMAKE_BUILD_TYPE}")
message(STATUS "  app (Qt) ........... ${XPCOG_BUILD_APP}")
message(STATUS "  cli ................ ${XPCOG_BUILD_CLI}")
message(STATUS "  tests .............. ${XPCOG_BUILD_TESTS}")
get_property(_codecs GLOBAL PROPERTY XPCOG_CODEC_TARGETS)
message(STATUS "  codecs ............. ${_codecs}")
message(STATUS "")
