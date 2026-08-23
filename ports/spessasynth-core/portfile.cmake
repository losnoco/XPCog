# SpessaSynth Core, as the SoundFont synthesiser behind MIDI.
#
# An overlay port rather than a vendored copy, by the rule in ../README.md: a
# real upstream at a pinned commit. Cog carries it as a submodule for the same
# reason it carries mGBA as one -- it has no package manager -- and the commit
# below is the one Cog's superproject records.
#
# kode54's C port rather than SpessaSynth itself, which is TypeScript. The two
# are kept in step: this tree tracks upstream's 4.3 line and its README claims
# parity down to float32 rounding.
#
# Note the source layout: the repository root holds the Xcode project and a
# website, and the library lives one directory down. Hence the SOURCE_PATH
# suffix on the configure step.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO kode54/spessasynth_core_c
    REF 99fc352d8bb38851cb9b13c36eb8eb7a98903c2b
    SHA512 0736bed1dbe84e3227a37e3daa6c6823576d03f1360f10ed2ef8811a35d6a0e006b17cda1275c47e2315422b4f99dc54d185f52adfbac89db7b25d955c423c4a
    HEAD_REF main
    PATCHES
        msvc-static-export-header.patch
        vorbis-via-cmake-config.patch
)

# Static, like everything else here, and no examples -- the example tool is a
# renderer with its own argument parsing that nothing links.
#
# SF3 support is what the FLAC and Vorbis dependencies are for, and both are
# left on. An SF3 bank is an SF2 whose samples are compressed, which is the
# difference between a 30 MB bank and a 300 MB one; refusing them would rule
# out most of what a user is likely to have downloaded. zlib is for XMF files
# with compressed FileNodes, and XPCog links it already.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/spessasynth_core"
    OPTIONS
        -DSS_BUILD_SHARED=OFF
        -DSS_BUILD_EXAMPLES=OFF
        -DSS_ENABLE_SF3_VORBIS=ON
        -DSS_ENABLE_SF3_FLAC=ON
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

# Headers ship once; vcpkg rejects a debug tree carrying its own copy.
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
