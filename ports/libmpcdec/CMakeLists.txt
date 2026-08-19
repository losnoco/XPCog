# A CMakeLists for musepack_src's decoder library alone. See portfile.cmake for
# why upstream's own is not used.
cmake_minimum_required(VERSION 3.16)
project(libmpcdec C)

include(GNUInstallDirs)
include(TestBigEndian)

# Nine sources: upstream's own list for this target, and the whole of what the
# decoder needs.
#
# Worth stating because Cog compiles twelve, and the extra three are not a fix
# for anything missing here. `common/fastmath.c`, `common/huffman-bcl.c` and
# `common/tags.c` belong to the encoder and the tag utilities -- no source under
# libmpcdec/ references `Init_FastMath`, `_Huffman_MakeTree` or anything in
# tags.c. Cog builds one framework covering all of musepack, so it needs them;
# a decoder does not.
add_library(mpcdec
    libmpcdec/huffman.c
    libmpcdec/mpc_bits_reader.c
    libmpcdec/mpc_decoder.c
    libmpcdec/mpc_demux.c
    libmpcdec/mpc_reader.c
    libmpcdec/requant.c
    libmpcdec/streaminfo.c
    libmpcdec/synth_filter.c
    common/crc32.c)
add_library(libmpcdec::mpcdec ALIAS mpcdec)

target_include_directories(mpcdec PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
# libmpcdec/ privately, for the internal headers the sources include by name.
target_include_directories(mpcdec PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libmpcdec")

# Upstream's top-level CMakeLists adds -DFAST_MATH -DCVD_FASTLOG globally and
# they are deliberately not set here: FAST_MATH only selects the table-driven
# COSF/ATAN2F/IFLOORF in mpcmath.h, which no decoder source includes, and
# CVD_FASTLOG is read in exactly one file, libmpcpsy/cvd.c, which is the
# encoder's psychoacoustic model. Setting them on this target would change
# nothing and imply otherwise.

# pow() and log10(), in mpc_demux.c and streaminfo.c, both for the ReplayGain
# fields. On glibc those live in libm rather than libc.
if(NOT MSVC)
    include(CheckLibraryExists)
    check_library_exists(m pow "" MPCDEC_NEEDS_LIBM)
    if(MPCDEC_NEEDS_LIBM)
        target_link_libraries(mpcdec PUBLIC m)
    endif()
    target_compile_options(mpcdec PRIVATE -w)
else()
    # A 2005-vintage C library: the CRT deprecation warnings are noise here, and
    # so is every conversion warning in arithmetic written before anyone minded.
    target_compile_definitions(mpcdec PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(mpcdec PRIVATE /w)
endif()

# There is no endianness switch to set for the decoder. internal.h's
# mpc_swap32() is a plain inline function called from one place, under the SV7
# word-swap path. `MPC_BIG_ENDIAN` exists in the tree and is read in exactly one
# file, mpcenc/wave_in.c, which is why the tools target below sets it and this
# one does not. Upstream calls TEST_BIG_ENDIAN in its top-level CMakeLists and
# passes the result to neither.

set_target_properties(mpcdec PROPERTIES
    C_STANDARD 99
    POSITION_INDEPENDENT_CODE ON
    EXPORT_NAME mpcdec)

install(TARGETS mpcdec EXPORT libmpcdecTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
# Public headers keep their `mpc/` directory: the sources include
# <mpc/mpcdec.h> and so does anything using the library. Cog renames this to
# `mpcdec/` for its framework layout, which is a change this tree has no reason
# to inherit.
#
# `.svn` excluded because the tarball is an SVN export with the working-copy
# metadata left in it -- r475 predates the single top-level .svn directory, so
# every source directory has one. Installed verbatim they put eight empty
# directories in the package, which vcpkg's post-build check rejects.
install(DIRECTORY include/mpc DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        PATTERN ".svn" EXCLUDE)

install(EXPORT libmpcdecTargets
        NAMESPACE libmpcdec::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/libmpcdec")

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/libmpcdec-config.cmake"
"include(\"\${CMAKE_CURRENT_LIST_DIR}/libmpcdecTargets.cmake\")\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/libmpcdec-config.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/libmpcdec")

# ---------------------------------------------------------------------------
# mpcenc, under the `tools` feature
# ---------------------------------------------------------------------------
# The encoder, built here because the test suite needs one to produce fixtures
# and there is nowhere else to get it. Musepack has no packaged encoder on any
# platform this builds for: no winget or Chocolatey package, nothing in apt or
# Homebrew that is current, and the Windows binaries on the format's own site
# are a 2011 zip. The reference tree ships the encoder beside the decoder, so a
# port that already downloads the tree can build it for the cost of twenty more
# C files -- which also pins the encoder to the same revision as the decoder it
# is testing, rather than to whatever a mirror last had.
if(LIBMPCDEC_BUILD_TOOLS)
    add_executable(mpcenc
        mpcenc/keyboard.c
        mpcenc/mpcenc.c
        mpcenc/pipeopen.c
        mpcenc/stderr.c
        mpcenc/wave_in.c
        mpcenc/winmsg.c
        libmpcenc/analy_filter.c
        libmpcenc/bitstream.c
        libmpcenc/encode_sv7.c
        libmpcenc/huffsv7.c
        libmpcenc/quant.c
        libmpcpsy/ans.c
        libmpcpsy/cvd.c
        libmpcpsy/fft4g.c
        libmpcpsy/fft_routines.c
        libmpcpsy/profile.c
        libmpcpsy/psy.c
        libmpcpsy/psy_tab.c
        common/crc32.c
        common/fastmath.c
        common/tags.c)

    target_include_directories(mpcenc PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/libmpcenc"
        "${CMAKE_CURRENT_SOURCE_DIR}/libmpcpsy")

    # FAST_MATH and CVD_FASTLOG are upstream's own, and unlike the decoder the
    # encoder genuinely reads both: mpcmath.h's table-driven COSF/ATAN2F, whose
    # tables are common/fastmath.c, and the one #ifdef in libmpcpsy/cvd.c.
    target_compile_definitions(mpcenc PRIVATE FAST_MATH CVD_FASTLOG)

    test_big_endian(MPCENC_BIG_ENDIAN)
    if(MPCENC_BIG_ENDIAN)
        target_compile_definitions(mpcenc PRIVATE MPC_BIG_ENDIAN)
    endif()

    if(MSVC)
        target_compile_definitions(mpcenc PRIVATE _CRT_SECURE_NO_WARNINGS
                                                  _CRT_NONSTDC_NO_WARNINGS)
        target_compile_options(mpcenc PRIVATE /w)
        target_link_libraries(mpcenc PRIVATE winmm)
    else()
        target_compile_options(mpcenc PRIVATE -w)
        target_link_libraries(mpcenc PRIVATE m)
    endif()

    install(TARGETS mpcenc RUNTIME DESTINATION tools/libmpcdec)
endif()
