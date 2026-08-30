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
    VERSION 1.0.0
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

# ---------------------------------------------------------------------------
# The application's identity on a Linux desktop.
#
# One reverse-DNS name, in one place, because four separate files have to agree
# on it or the desktop silently does nothing: the .desktop file's basename, the
# AppStream metainfo's <id>, the installed icon's filename -- Flatpak requires
# the icon be named for the application ID -- and the DesktopEntry property
# MPRIS publishes, which is how a panel gets from the transport it is showing to
# the application's icon and localised name.
#
# co.losno.XPCog: losno.co reversed, which is the domain the project's site is
# on, plus the product name. Not io.github.* -- that is the fallback Flathub
# offers projects with no domain of their own. The namespace is not new either;
# the MPRIS track paths in platform/src/linux/GDbusMediaIntegration.cpp have
# been /co/losno/xpcog/track/N since MPRIS was written.
#
# The macOS bundle identifier next to it in app/CMakeLists.txt is the lowercase
# co.losno.xpcog, and deliberately stays that way: it is a different platform's
# namespace, and on macOS it is also where a user's preferences are keyed, so
# changing its spelling would orphan them.
# ---------------------------------------------------------------------------
set(XPCOG_DESKTOP_ID "co.losno.XPCog")

# bin/, lib/ and the rest, so a packager can place them.
#
# One of them does not move, and it is worth knowing which:
# core/include/xpcog/core/AssetPath.hpp resolves the shipped assets by walking
# from the executable to ../share/xpcog, and that path is compiled in. It agrees
# with CMAKE_INSTALL_DATADIR's default -- "share" -- but it cannot follow the
# variable if someone overrides it, so assets/CMakeLists.txt installs to a
# literal share/xpcog rather than pretending otherwise.
include(GNUInstallDirs)

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
# The rpath and the one bundled shared library an installed Linux build needs.
include(XPCogInstallRuntime)
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

    # Packaging, after the thing being packaged. None of what these add is ever
    # built by ALL: `installer` on Windows, and `sign`, `dmg` and `notarize` on
    # macOS. See packaging/windows/CMakeLists.txt and packaging/macos/CMakeLists.txt.
    if(WIN32)
        add_subdirectory(packaging/windows)
    elseif(APPLE)
        add_subdirectory(packaging/macos)
    else()
        # Unlike the other two this one *is* built by ALL, because it builds
        # nothing: it is install() rules and two configured text files, and they
        # are what `cmake --install` needs to produce a working player.
        add_subdirectory(packaging/linux)
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
