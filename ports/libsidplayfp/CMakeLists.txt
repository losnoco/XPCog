# libsidplayfp, built with CMake because upstream has only autotools.
#
# Dropped into the extracted source tree by ../portfile.cmake, which also brings
# the pre-assembled MUS players and the three header templates `./configure`
# would otherwise fill in. See that file for the reasoning; this one is the
# build.
#
# Scope: the reSIDfp emulation and nothing else. libsidplayfp can also build
# reSID (a git submodule), HardSID and exSID (both drivers for real SID chips on
# real hardware), and none of the three is wanted here -- Cog's SidDecoder.mm
# constructs a `ReSIDfpBuilder` and no other. Leaving reSID out is what keeps
# the port free of submodules, and with it eight generated wavetable headers.
cmake_minimum_required(VERSION 3.21)
project(libsidplayfp CXX C)

include(CheckIncludeFileCXX)
include(CheckCXXSymbolExists)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PACKAGE "libsidplayfp")
set(PACKAGE_NAME "libsidplayfp")
set(PACKAGE_VERSION "2.3.1")
set(PACKAGE_URL "https://github.com/libsidplayfp/libsidplayfp/")
set(LIB_MAJOR 6)
set(LIB_MINOR 0)
set(LIB_LEVEL 0)

# ---------------------------------------------------------------------------
# What ./configure would have probed
# ---------------------------------------------------------------------------
check_include_file_cxx(inttypes.h  HAVE_INTTYPES_H)
check_include_file_cxx(stdint.h    HAVE_STDINT_H)
check_include_file_cxx(stdio.h     HAVE_STDIO_H)
check_include_file_cxx(stdlib.h    HAVE_STDLIB_H)
check_include_file_cxx(string.h    HAVE_STRING_H)
check_include_file_cxx(strings.h   HAVE_STRINGS_H)
check_include_file_cxx(sys/stat.h  HAVE_SYS_STAT_H)
check_include_file_cxx(sys/types.h HAVE_SYS_TYPES_H)
check_include_file_cxx(unistd.h    HAVE_UNISTD_H)

# The resampler picks a SIMD path from these. Probed rather than assumed: an
# arm64 host has arm_neon.h and no emmintrin.h, and an x86 one the reverse.
check_include_file_cxx(mmintrin.h  HAVE_MMINTRIN_H)
check_include_file_cxx(emmintrin.h HAVE_EMMINTRIN_H)
check_include_file_cxx(arm_neon.h  HAVE_ARM_NEON_H)

check_cxx_symbol_exists(strcasecmp  strings.h HAVE_STRCASECMP)
check_cxx_symbol_exists(strncasecmp strings.h HAVE_STRNCASECMP)
check_cxx_symbol_exists(stricmp     string.h  HAVE_STRICMP)
check_cxx_symbol_exists(strnicmp    string.h  HAVE_STRNICMP)
check_cxx_symbol_exists(log1p       cmath     HAVE_LOG1P)

if(WIN32)
    set(HAVE_MSWINDOWS 1)
endif()

# __builtin_expect is a GNU extension; MSVC has no equivalent and reSIDfp's
# likely()/unlikely() fall back to the bare expression without it.
if(MSVC)
    set(HAVE_BUILTIN_EXPECT 0)
    set(RESID_BRANCH_HINTS 0)
else()
    set(HAVE_BUILTIN_EXPECT 1)
    set(RESID_BRANCH_HINTS 1)
endif()
set(RESID_INLINING 1)
set(RESID_INLINE inline)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/xpcog-generated/config.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/config.h")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/xpcog-generated/sidversion.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/sidplayfp/sidversion.h")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/xpcog-generated/siddefs-fp.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/residfp/siddefs-fp.h")

# The two MUS players are 6502 assembly (`sidplayer1.a65`, `sidplayer2.a65`)
# and upstream assembles them with xa65 at build time. Copied in already
# assembled instead -- see the portfile.
# Into the binary dir root rather than beside MUS.cpp: `#include "x.bin"`
# searches the including file's own directory first and the -I paths after, and
# the binary dir root is one of those.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/xpcog-generated/sidplayer1.bin"
               "${CMAKE_CURRENT_BINARY_DIR}/sidplayer1.bin" COPYONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/xpcog-generated/sidplayer2.bin"
               "${CMAKE_CURRENT_BINARY_DIR}/sidplayer2.bin" COPYONLY)

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
set(SIDPLAYFP_SOURCES
    src/EventScheduler.cpp
    src/mixer.cpp
    src/player.cpp
    src/psiddrv.cpp
    src/reloc65.cpp
    src/sidemu.cpp

    src/c64/c64.cpp
    src/c64/mmu.cpp
    src/c64/CIA/interrupt.cpp
    src/c64/CIA/mos652x.cpp
    src/c64/CIA/SerialPort.cpp
    src/c64/CIA/timer.cpp
    src/c64/CIA/tod.cpp
    src/c64/CPU/mos6510.cpp
    src/c64/VIC_II/mos656x.cpp

    src/sidplayfp/sidbuilder.cpp
    src/sidplayfp/SidConfig.cpp
    src/sidplayfp/SidInfo.cpp
    src/sidplayfp/sidplayfp.cpp
    src/sidplayfp/SidTune.cpp
    src/sidplayfp/SidTuneInfo.cpp

    src/sidtune/MUS.cpp
    src/sidtune/p00.cpp
    src/sidtune/prg.cpp
    src/sidtune/PSID.cpp
    src/sidtune/SidTuneBase.cpp
    src/sidtune/SidTuneTools.cpp

    src/utils/iniParser.cpp
    src/utils/md5Factory.cpp
    src/utils/MD5/MD5.cpp
    src/utils/SidDatabase.cpp
    src/utils/STILview/stil.cpp

    src/builders/residfp-builder/residfp-builder.cpp
    src/builders/residfp-builder/residfp-emu.cpp
    src/builders/residfp-builder/residfp/Dac.cpp
    src/builders/residfp-builder/residfp/EnvelopeGenerator.cpp
    src/builders/residfp-builder/residfp/ExternalFilter.cpp
    src/builders/residfp-builder/residfp/Filter.cpp
    src/builders/residfp-builder/residfp/Filter6581.cpp
    src/builders/residfp-builder/residfp/Filter8580.cpp
    src/builders/residfp-builder/residfp/FilterModelConfig6581.cpp
    src/builders/residfp-builder/residfp/FilterModelConfig8580.cpp
    src/builders/residfp-builder/residfp/Integrator6581.cpp
    src/builders/residfp-builder/residfp/Integrator8580.cpp
    src/builders/residfp-builder/residfp/OpAmp.cpp
    src/builders/residfp-builder/residfp/SID.cpp
    src/builders/residfp-builder/residfp/Spline.cpp
    src/builders/residfp-builder/residfp/WaveformCalculator.cpp
    src/builders/residfp-builder/residfp/WaveformGenerator.cpp
    src/builders/residfp-builder/residfp/resample/SincResampler.cpp

    # A .cc rather than .cpp, and easy to miss when listing sources by glob or
    # by eye. It is the only translation unit that defines __VERSION_CC__, which
    # is how siddefs-fp.h decides whether to declare residfp_version_string or
    # define it -- so without this the library links everywhere except at the
    # one call site in residfp-emu.cpp's getCredits().
    src/builders/residfp-builder/residfp/version.cc)

# mos6510debug.cpp is not here: it is the CPU trace logger, reached only under
# the debug build's FULL_DEBUG, and it writes to stdout.
add_library(sidplayfp STATIC ${SIDPLAYFP_SOURCES})

target_compile_definitions(sidplayfp PRIVATE HAVE_CONFIG_H)

target_include_directories(sidplayfp PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/residfp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/builders/residfp-builder"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/builders/residfp-builder/residfp")
target_include_directories(sidplayfp PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

if(MSVC)
    target_compile_definitions(sidplayfp PRIVATE
        _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS NOMINMAX
        WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
    target_compile_options(sidplayfp PRIVATE /w)
else()
    target_compile_options(sidplayfp PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS sidplayfp
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/sidplayfp/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/sidplayfp"
        FILES_MATCHING PATTERN "*.h")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/sidplayfp/sidversion.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/sidplayfp")

# residfp.h lives under builders/ in the source tree but is included as
# <sidplayfp/residfp.h> -- which is how Cog's SidDecoder.mm reaches the builder,
# and what autotools installs it as.
install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/builders/residfp-builder/residfp.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/sidplayfp")
