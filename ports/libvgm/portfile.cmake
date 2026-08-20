# libvgm: ValleyBell's sound chip emulators, and the four players that drive
# them -- VGM, S98, DRO and GYM.
#
# Not in vcpkg upstream, and pinned to the same commit Cog ships prebuilt in its
# ThirdParty/libvgm (867223e, recorded in that directory's README). Upstream has
# no tags at all, so a commit is the only thing there is to pin; taking Cog's
# means the emulator revision is the one Cog's listeners have been hearing.
#
# Three of the six build options are off because they build *programs* rather
# than libraries -- the player application, vgm2wav, and the test binaries -- and
# a fourth, BUILD_LIBAUDIO, because it is an output backend. XPCog has its own
# (IAudioOutput), and libaudio is where libvgm would otherwise pull in ALSA,
# PulseAudio, DirectSound, WASAPI and CoreAudio depending on the platform. Cog
# turns off exactly the same set.
#
# USE_SANITIZERS defaults to ON upstream, which is a reasonable default for
# somebody hacking on libvgm and not for a dependency: it changes the ABI of
# what it instruments, and a sanitized library linked into an unsanitized
# application is a link error at best.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ValleyBell/libvgm
    REF 867223e7c33d63de115d1ab955f784c44f19040a
    SHA512 4bc18521d7c02b961d849916f67d87e2fe2124aa063ee95c068387c7ba2f9e025aeffd7907e676871250c9c82d1074aa748612444c2f4fced85ddcaa930f836d
    HEAD_REF master
)

# LIBRARY_TYPE is libvgm's own name for what every other project spells
# BUILD_SHARED_LIBS, so the triplet's preference has to be translated rather
# than passed through.
if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    set(LIBVGM_LIBRARY_TYPE "SHARED")
else()
    set(LIBVGM_LIBRARY_TYPE "STATIC")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DLIBRARY_TYPE=${LIBVGM_LIBRARY_TYPE}
        -DBUILD_LIBAUDIO=OFF
        -DBUILD_LIBEMU=ON
        -DBUILD_LIBPLAYER=ON
        -DBUILD_TESTS=OFF
        -DBUILD_PLAYER=OFF
        -DBUILD_VGM2WAV=OFF
        -DUSE_SANITIZERS=OFF
)

vcpkg_cmake_install()

# One config package, not four. The sub-projects install their targets and
# headers but only the top-level CMakeLists calls cmake_cfg_install, so
# `libvgmConfig.cmake` is the whole export -- it carries vgm-emu, vgm-utils and
# vgm-player as targets rather than as packages of their own. The pkg-config
# files below are per-library; those are separate and are three.
vcpkg_cmake_config_fixup(PACKAGE_NAME libvgm CONFIG_PATH lib/cmake/libvgm)

vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

# Written rather than copied: libvgm ships no top-level licence file, so there is
# nothing for vcpkg_install_copyright's FILE_LIST to point at.
file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/copyright"
"libvgm carries no top-level licence file.\n"
"\n"
"The library as a whole is GPL-2.0. Individual emulator cores under emu/cores\n"
"carry the licences their original authors gave them -- most are GPL-2.0 or\n"
"BSD, several are ports from MAME or openMSX, and each states its own in a\n"
"`license:` line at the top of the file. Check those before shipping a build\n"
"under anything other than the GPL.\n")
