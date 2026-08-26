cmake_minimum_required(VERSION 3.24)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# Before project(), and it has to be: vcpkg installs the manifest from inside its
# toolchain file, which project() is what reads. Anything that decides which
# packages vcpkg installs -- which is exactly what this does on Linux, when
# XPCOG_USE_SYSTEM_LIBS is on -- is too late one line further down.
include(XPCogSystemDeps)
xpcog_probe_system_deps()

# The vcpkg toolchain must be set before project(); CMakePresets.json does that.
project(XPCog
    VERSION 0.2.8
    DESCRIPTION "Cross-platform wxWidgets port of the Cog audio player"
    LANGUAGES C CXX)

# After project(), and only after: vcpkg's toolchain sets these same variables as
# project() reads it. See the function for what a distribution package's single,
# configuration-less build does to an imported target without them.
xpcog_system_deps_config_map()

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
include(XPCogResources)
include(XPCogCatalogs)
# The UI toolkit. An ordinary vcpkg dependency: no environment variable to set,
# no separate installer, and no deploy step. See docs/WXPORT.md for what that
# replaced.
include(XPCogWx)

# ---------------------------------------------------------------------------
# Layers, innermost first. core and codecs link no toolkit; platform links no
# toolkit either. cmake/CheckNoToolkit.cmake is what keeps that true.
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

    # Packaging, after the thing being packaged. Adds one target, `installer`,
    # which is never built by ALL -- see packaging/windows/CMakeLists.txt.
    if(WIN32)
        add_subdirectory(packaging/windows)
    endif()
endif()

if(XPCOG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "")
message(STATUS "XPCog ${PROJECT_VERSION}")
message(STATUS "  build type ......... ${CMAKE_BUILD_TYPE}")
message(STATUS "  app ................ ${XPCOG_BUILD_APP}")
message(STATUS "  toolkit ............ wxWidgets (${XPCOG_WX_DISCOVERY})")
message(STATUS "  cli ................ ${XPCOG_BUILD_CLI}")
message(STATUS "  tests .............. ${XPCOG_BUILD_TESTS}")
get_property(_codecs GLOBAL PROPERTY XPCOG_CODEC_TARGETS)
message(STATUS "  codecs ............. ${_codecs}")
xpcog_system_deps_report()
message(STATUS "")
