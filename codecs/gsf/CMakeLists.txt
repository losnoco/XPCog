# GSF: Game Boy Advance rips, on mGBA.
#
# mGBA arrives as a vcpkg overlay port (../../ports/mgba) rather than as a
# submodule the way Cog carries it -- see that portfile for why. It ships no
# CMake config package, so the target is assembled here the same way libgme's is
# in ../gme/CMakeLists.txt.
# mGBA's own link libraries, which an imported target assembled by hand does not
# inherit. This is the trap already recorded in ../gme/CMakeLists.txt for libgme
# and zlib, and it bites the same way: the omission is invisible wherever some
# other target happens to pull the library in first, and a hard error where
# nothing does. Windows is where it showed up --
#
#   mgba.lib(config.c.obj) : error LNK2019: unresolved external symbol
#   __imp_PathRemoveFileSpecW referenced in function mCoreConfigPortableIniPath
#
# -- PathRemoveFileSpecW and PathIsRelativeW being shlwapi. Taken from mGBA's own
# CMakeLists: `list(APPEND OS_LIB ws2_32 shlwapi)` on Win32, Foundation on Apple,
# libm everywhere else, and zlib because the port builds with USE_ZLIB=ON.
find_package(ZLIB REQUIRED)
set(XPCOG_MGBA_LINK ZLIB::ZLIB)
if(WIN32)
    list(APPEND XPCOG_MGBA_LINK ws2_32 shlwapi)
elseif(APPLE)
    list(APPEND XPCOG_MGBA_LINK "-framework Foundation")
else()
    list(APPEND XPCOG_MGBA_LINK m)
endif()

find_path(MGBA_INCLUDE_DIR NAMES mgba/core/core.h REQUIRED)
find_library(MGBA_LIBRARY_RELEASE NAMES mgba REQUIRED)
find_library(MGBA_LIBRARY_DEBUG NAMES mgba
             PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib"
             NO_DEFAULT_PATH)

# The feature macros, and they are not optional decoration.
#
# `struct mCore` -- the vtable every call into mGBA goes through -- declares its
# members inside `#ifdef ENABLE_VFS`, `#if defined(ENABLE_VFS) &&
# defined(ENABLE_DIRECTORIES)`, `#ifndef MINIMAL_CORE` and friends. mGBA passes
# those as -D on its own targets and its public headers do not set them, so a
# consumer that compiles core.h with a different set sees a struct with
# different offsets. That does not fail to compile and does not fail to link: it
# reads every function pointer from the wrong slot and jumps to whatever was
# there. Here it was address zero, inside the first core->init().
#
# `mgba/flags.h` is the generated header that looks like it solves this, and it
# does not -- at this pin it reports ENABLE_DIRECTORIES, ENABLE_VFS_FD and
# USE_MINIZIP as undefined while the library is built with all three. Trusting
# it produces exactly the crash above.
#
# So the list is stated here, and it must match what ../../ports/mgba builds.
# To re-derive it after changing that portfile, configure once and read the
# definitions off the port's own build:
#
#   grep -o '\-D[A-Z_0-9]*' \
#     "$VCPKG_ROOT/buildtrees/mgba/<triplet>-dbg/build.ninja" | sort -u
#
# Only the macros that guard *struct members* matter; the HAVE_* ones gate
# function availability and vary per platform, so they are deliberately absent.
set(XPCOG_MGBA_DEFINES
    M_CORE_GBA        # the GBA core is built; the GB core is not
    ENABLE_VFS        # mGBA's file abstraction, which VFileFromConstMemory is
    ENABLE_VFS_FD
    ENABLE_DIRECTORIES
    USE_MINIZIP       # on despite -DUSE_MINIZIP=OFF: mGBA bundles its own
    USE_ZLIB
    BUILD_STATIC)

if(NOT TARGET mGBA::mgba)
    add_library(mGBA::mgba UNKNOWN IMPORTED GLOBAL)
    set_target_properties(mGBA::mgba PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${MGBA_INCLUDE_DIR}"
        INTERFACE_COMPILE_DEFINITIONS "${XPCOG_MGBA_DEFINES}"
        INTERFACE_LINK_LIBRARIES "${XPCOG_MGBA_LINK}"
        IMPORTED_LOCATION "${MGBA_LIBRARY_RELEASE}")
    if(MGBA_LIBRARY_DEBUG)
        set_target_properties(mGBA::mgba PROPERTIES
            IMPORTED_LOCATION_DEBUG "${MGBA_LIBRARY_DEBUG}")
    endif()
endif()

xpcog_add_codec(
    NAME     gsf
    REGISTER xpcog_register_gsf
    SOURCES  GsfDecoder.cpp
    DEPS     xpcog-codec-psf mGBA::mgba xpcog-codec-common
)
