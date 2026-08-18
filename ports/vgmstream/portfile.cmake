# vgmstream: several hundred video-game streaming formats behind one reader.
#
# Not in vcpkg upstream. Two things about its build shape drive this file.
#
# Only the *shared* target carries install rules and a CMake export -- the static
# `libvgmstream` is built for the CLI and never installed -- so BUILD_SHARED_LIBS
# is on regardless of the triplet's preference, and static triplets are refused
# rather than silently producing a package with no library in it.
#
# Every optional codec is off, for two separate reasons.
#
# ATRAC9, CELT, Speex, G.719 and G.722.1 have no find_package path and are
# *downloaded* at configure time. A port that fetches from the network mid-build
# is not reproducible and fails outright in an offline or asset-cached
# environment, which is most CI.
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
    REF r2117
    SHA512 10cb8158f6d1d72217eae43990b7954ce74f93417ce1de3d6357519c3d4b2a060a0e17bc73d42404f54028e24accc8e1a5e7d288ac2b72b932c6d65eba29b08e
    HEAD_REF master
    PATCHES
        # Upstream builds the static library and the shared one side by side, and
        # on MSVC the shared target's import library lands on the same
        # src/libvgmstream.lib the static target writes. Two ninja rules for one
        # file is an error, not a race, so a shared build simply cannot configure
        # on MSVC without this.
        fix-msvc-import-lib-clash.patch

        # And the export macro is MSVC-only without saying so:
        #
        #   #if defined(LIBVGMSTREAM_EXPORT)
        #       #define LIBVGMSTREAM_API __declspec(dllexport)
        #
        # with no _WIN32 guard, while the shared target defines
        # LIBVGMSTREAM_EXPORT unconditionally. On GCC and Clang every API
        # function therefore begins `__declspec(dllexport)`, which they parse as
        # a K&R-style definition -- so the headers that follow are read as
        # *parameter declarations* and the file ends with "expected '{' at end
        # of input". Hundreds of errors inside /usr/include/stdio.h, none of
        # them the cause. A shared build simply cannot compile off Windows
        # without this.
        fix-declspec-on-non-windows.patch
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
        -DUSE_ATRAC9=OFF
        -DUSE_CELT=OFF
        -DUSE_SPEEX=OFF
        -DUSE_G7221=OFF
        -DUSE_G719=OFF
        -DUSE_JANSSON=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME vgmstream CONFIG_PATH lib/cmake/vgmstream)
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
