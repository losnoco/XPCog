# Replaces upstream's autotools build; see portfile.cmake for why.
cmake_minimum_required(VERSION 3.16)
project(adplug CXX C)

include(GNUInstallDirs)

find_package(binio CONFIG QUIET)
if(NOT TARGET binio)
    # libbinio's port installs a plain archive and headers rather than a CMake
    # config package, so the target is assembled here. Same shape as the GME
    # case in codecs/gme/CMakeLists.txt, and for the same reason.
    find_path(BINIO_INCLUDE_DIR NAMES binio.h REQUIRED)
    find_library(BINIO_LIBRARY_RELEASE NAMES binio REQUIRED)
    find_library(BINIO_LIBRARY_DEBUG NAMES binio
                 PATHS "${CMAKE_PREFIX_PATH}/debug/lib" NO_DEFAULT_PATH)

    add_library(binio STATIC IMPORTED)
    set_target_properties(binio PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${BINIO_INCLUDE_DIR}"
        IMPORTED_LOCATION "${BINIO_LIBRARY_RELEASE}")
    if(BINIO_LIBRARY_DEBUG)
        set_target_properties(binio PROPERTIES
            IMPORTED_LOCATION_DEBUG "${BINIO_LIBRARY_DEBUG}")
    endif()
endif()

# The one thing ./configure generates that src/ actually reads: adplug.cpp
# includes version.h, which is a single #define carrying AC_INIT's version
# string. Written here rather than copied in, so it cannot drift from the
# version this port declares.
set(VERSION "2.3.4-beta")
configure_file(src/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/version.h")

add_library(adplug STATIC
    src/adlibemu.c
    src/debug.c
    src/fmopl.c
    src/nukedopl.c
    src/a2m.cpp
    src/adl.cpp
    src/adlib.cpp
    src/adplug.cpp
    src/adtrack.cpp
    src/amd.cpp
    src/analopl.cpp
    src/bam.cpp
    src/bmf.cpp
    src/cff.cpp
    src/cmf.cpp
    src/cmfmcsop.cpp
    src/d00.cpp
    src/database.cpp
    src/dfm.cpp
    src/diskopl.cpp
    src/dmo.cpp
    src/dro2.cpp
    src/dro.cpp
    src/dtm.cpp
    src/emuopl.cpp
    src/flash.cpp
    src/fmc.cpp
    src/fprovide.cpp
    src/got.cpp
    src/herad.cpp
    src/hsc.cpp
    src/hsp.cpp
    src/hybrid.cpp
    src/hyp.cpp
    src/imf.cpp
    src/jbm.cpp
    src/ksm.cpp
    src/lds.cpp
    src/mad.cpp
    src/mdi.cpp
    src/mid.cpp
    src/mkj.cpp
    src/msc.cpp
    src/mtk.cpp
    src/mus.cpp
    src/nemuopl.cpp
    src/player.cpp
    src/players.cpp
    src/protrack.cpp
    src/psi.cpp
    src/rad2.cpp
    src/rat.cpp
    src/raw.cpp
    src/realopl.cpp
    src/rix.cpp
    src/rol.cpp
    src/s3m.cpp
    src/sa2.cpp
    src/sng.cpp
    src/sop.cpp
    src/surroundopl.cpp
    src/temuopl.cpp
    src/u6m.cpp
    src/vgm.cpp
    src/woodyopl.cpp
    src/xad.cpp
    src/xsm.cpp
)

target_include_directories(adplug PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(adplug PUBLIC binio)

# AdPlug calls stricmp() throughout for caseless comparison, and the Makefile.am
# carries a comment calling the substitution a hack. It is still the right one:
# stricmp is the DOS and Windows spelling, POSIX has strcasecmp, and MSVC has
# deprecated the unprefixed name in favour of _stricmp -- so each platform gets
# the name it actually provides rather than the library growing a shim.
if(MSVC)
    target_compile_definitions(adplug PRIVATE stricmp=_stricmp)
    # A 2001 codebase against a current MSVC: it uses fopen, strcpy and sprintf
    # freely, and every one of them raises C4996. They are not this port's to
    # fix and reading like ours is the harm.
    target_compile_definitions(adplug PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(adplug PRIVATE /w)
else()
    target_compile_definitions(adplug PRIVATE stricmp=strcasecmp)
    target_compile_options(adplug PRIVATE -w)
endif()

install(TARGETS adplug ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

# Every header in src/, rather than the seventy Makefile.am names one by one.
# The four it leaves out -- debug.h, mididata.h and their neighbours -- are
# internal, and shipping them costs nothing next to a list that has to be kept
# in step with upstream by hand.
file(GLOB ADPLUG_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
install(FILES ${ADPLUG_HEADERS} "${CMAKE_CURRENT_BINARY_DIR}/version.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/adplug")
