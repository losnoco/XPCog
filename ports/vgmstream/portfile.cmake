# vgmstream: several hundred video-game streaming formats behind one reader.
#
# Not in vcpkg upstream. Two things about its build shape drive this file.
#
# Only the *shared* target carries install rules and a CMake export -- the static
# `libvgmstream` is built for the CLI and never installed -- so BUILD_SHARED_LIBS
# is on regardless of the triplet's preference, and static triplets are refused
# rather than silently producing a package with no library in it.
#
# ATRAC9, G.719 and G.722.1 are on: since r2168 they are decoders inside the
# tree, with nothing to fetch. Every other optional codec is off, for two
# separate reasons.
#
# CELT and Speex have no find_package path and are *downloaded* at configure
# time. A port that fetches from the network mid-build is not reproducible and
# fails outright in an offline or asset-cached environment, which is most CI.
#
# MPEG and Vorbis are off despite vcpkg carrying both, and that one is worth
# explaining. On MSVC, vgmstream does not link the libraries it finds: it
# generates import libraries from .def files it ships (ext_libs/libvorbis.def
# and friends) and expects its own matching DLL names at run time --
# libmpg123-0.dll, libvorbis.dll -- which are not what vcpkg builds. Forcing
# that path to configure only moves the failure from link time to a missing DLL
# on first play. Wiring vgmstream to vcpkg's copies properly is its own patch,
# and it is worth doing later for Wwise and FSB Vorbis.
#
# What is built in and unaffected: every ADPCM and PCM family, HCA and ADX. That
# is what console rips overwhelmingly are.

vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO vgmstream/vgmstream
    REF 7ac2263bbf2bc8c2207cbce653b7803690ba6ee4
    SHA512 785e29d6dfa8bc507d6e0845f3418beb233be0a431e1a01cfde731365f15a693e2b04d645a523fb0f98a3d03d4eea0e5b9370bfd1d6573563fd205bb0c4eb422
    HEAD_REF master
    PATCHES
        # Upstream builds the static library and the shared one side by side, and
        # on MSVC the shared target's import library lands on the same
        # src/libvgmstream.lib the static target writes. Two ninja rules for one
        # file is an error, not a race, so a shared build simply cannot configure
        # on MSVC without this.
        fix-msvc-import-lib-clash.patch

)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_SHARED_LIBS=ON
        -DBUILD_CLI=OFF
        -DBUILD_V123=OFF
        -DBUILD_WINAMP=OFF
        -DBUILD_XMPLAY=OFF
        -DBUILD_FB2K=OFF
        -DBUILD_AUDACIOUS=OFF
        -DUSE_FFMPEG=OFF
        -DUSE_MPEG=OFF
        -DUSE_VORBIS=OFF
        -DUSE_ATRAC9=ON
        -DUSE_CELT=OFF
        -DUSE_SPEEX=OFF
        -DUSE_G7221=ON
        -DUSE_G719=ON
        -DUSE_JANSSON=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME vgmstream CONFIG_PATH lib/cmake/vgmstream)
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
