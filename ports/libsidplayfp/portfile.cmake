# libsidplayfp, as the C64 SID core behind the .sid decoder.
#
# An overlay port for the reason ../README.md gives -- a real upstream at a
# pinned commit -- and the README already named this library as one of the
# cases. kode54's fork rather than libsidplayfp/libsidplayfp, at the commit
# Cog's .gitmodules pins, since Cog's plugin is written against that tree.
#
# ---------------------------------------------------------------------------
# Why this port carries its own CMakeLists
# ---------------------------------------------------------------------------
# Upstream builds with autotools and nothing else: no CMakeLists anywhere in the
# tree. Running ./configure inside a vcpkg port would mean autoconf, automake,
# libtool and a POSIX shell on every platform including MSVC, to produce a
# static library from fifty source files. So the port supplies a CMakeLists and
# does the configure step's work with CMake's own probes -- which is also the
# only way the feature checks come out right per platform rather than per
# whoever last ran the script.
#
# ---------------------------------------------------------------------------
# What is copied in, and why it cannot be generated here
# ---------------------------------------------------------------------------
# `xpcog-generated/` holds four files.
#
# Three are ./configure templates -- config.h.in (written here, since autoconf's
# emits a great deal the code never reads), sidversion.h.in and siddefs-fp.h.in
# (upstream's own, verbatim). CMake fills them in.
#
# The fourth pair is different in kind. `sidplayer1.bin` and `sidplayer2.bin`
# are the two MUS players -- 6502 machine code that libsidplayfp installs into
# C64 memory to play Compute!'s Sidplayer tunes -- and upstream assembles them
# from `sidplayer1.a65` and `sidplayer2.a65` at build time with xa65, a
# cross-assembler for the 6502 that is not in vcpkg and is not on a CI runner.
# They are copied in already assembled, which is what Cog does too: its
# `update-generated.sh` runs the whole autotools build once on a developer's
# machine and commits the outputs.
#
# They are text rather than binary -- comma-separated hex bytes, `#include`d
# into an array initialiser -- so they are reviewable in a diff, and dropping
# MUS support would have been the alternative. Cog claims `.mus` and this
# follows it.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO kode54/libsidplayfp
    REF 519d1201efcc6c97f7cc3506947875d21a9bd195
    SHA512 6dc4c2dfd4f8cca7f9c7f408a84a32804edff7d50e43089d5dc462151ba5052cec022653605efb0f2069890351b444fe11d49bfb664eeb0c1f7c6c17b538942a
    HEAD_REF master
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/generated/"
     DESTINATION "${SOURCE_PATH}/xpcog-generated")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt"
     DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
vcpkg_copy_pdbs()

# Headers ship once; vcpkg rejects a debug tree carrying its own copy.
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include"
                    "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
