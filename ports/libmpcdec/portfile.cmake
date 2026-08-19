# libmpcdec, the Musepack decoder behind the .mpc decoder.
#
# An overlay port for the reason ../README.md gives -- a real upstream with a
# release tarball -- and the README already named this library as one of the
# cases, along with the trap that catches everyone once: vcpkg's `mpc` port is
# GNU MPC, the multiprecision arithmetic library, and has nothing to do with
# Musepack.
#
# The reference tree rather than Cog's copy. Cog's `Frameworks/MPCDec` is
# r475 with three kinds of change: the public headers moved from `mpc/` to
# `mpcdec/` to suit a macOS framework, a scattering of explicit casts to quiet
# warnings, and one real fix -- see the patch below. Only the last is worth
# carrying, and the rename is actively unwanted here.
#
# ---------------------------------------------------------------------------
# Why this port carries its own CMakeLists
# ---------------------------------------------------------------------------
# Upstream ships one, and it opens `CMAKE_MINIMUM_REQUIRED(VERSION 2.4)`.
# CMake has refused anything below 3.5 since 3.27 and below 3.10 since 4.0, so
# it does not configure at all with a current CMake. It also sets
# `CMAKE_C_FLAGS` to a fixed `-O3 -Wall -fomit-frame-pointer -pipe`, which
# discards the toolchain's own flags, and builds twelve subdirectories of
# encoder and utility targets for a port that wants one library. Supplying a
# CMakeLists is less work than patching that into shape, and is what
# ../libsidplayfp does for a related reason.
#
# ---------------------------------------------------------------------------
# The one upstream fix worth taking
# ---------------------------------------------------------------------------
# Four places in mpc_demux.c mask with `-1 << 2` and `-1 << 1`. Left-shifting a
# negative value is undefined behaviour in C, and the value is a signed -1
# every time; it works on every compiler anyone has run it through, and it is
# exactly what a UBSan build reports. Cog rewrote the same four lines as
# multiplications, so the patch is Cog's fix applied to upstream rather than an
# invention here. `-fsanitize=undefined` is one `-D XPCOG_SANITIZE=undefined`
# away in this tree, which makes the difference between a clean run and four
# reports on every seek.
vcpkg_download_distfile(ARCHIVE
    URLS "https://files.musepack.net/source/musepack_src_r475.tar.gz"
    FILENAME "musepack_src_r475.tar.gz"
    SHA512 23aa16152b27ba99ff7f4672b8f91562ee9ad829bbe1ee066693eabf69844ee9f8e4b3eebf1bac78e6994119fbb53b034479a1b996576aeecbfba7d797f130b0
)

vcpkg_extract_source_archive(SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    PATCHES
        no-shift-of-negative.patch
        log2-name-collision.patch
        msvc-provides-asinh.patch
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

# Static regardless of triplet. Not a preference: the library annotates nothing
# with __declspec(dllexport) and has no export map, so a shared build on MSVC
# produces a DLL that exports no symbols and therefore no import library at all
# -- vcpkg reports it as an empty `lib/`, and anything linking it fails later
# and less clearly. Upstream's own CMakeLists defaults SHARED off on Windows for
# the same reason. It is nine C files; there is nothing to gain by shipping a
# DLL beside XPCog.exe for it.
set(VCPKG_LIBRARY_LINKAGE static)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        tools LIBMPCDEC_BUILD_TOOLS
)

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}" OPTIONS ${FEATURE_OPTIONS})
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME libmpcdec CONFIG_PATH lib/cmake/libmpcdec)
vcpkg_copy_pdbs()

# Headers ship once; vcpkg rejects a debug tree carrying its own copy. The same
# goes for mpcenc: the CMakeLists installs it under tools/, so a debug
# configuration produces a second one nothing would ever run. Not
# vcpkg_copy_tools(), which expects to *find* the binary in bin/ and move it --
# this one is installed where it belongs to begin with.
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include"
                    "${CURRENT_PACKAGES_DIR}/debug/share"
                    "${CURRENT_PACKAGES_DIR}/debug/tools")

# The tarball's own licence file is the decoder's; the tree has no top-level one.
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/libmpcdec/COPYING")
