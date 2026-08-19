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
    REF 28a362aa65a1035e2b5f2730001843f8f81e8564
    SHA512 c0071191263ee1f52f23582642931a4165e9e1bb76ab212a0037a9eddcf422ad061594bdcaf38a2869049723f7d733b35047107f2f8435295ca07c23487fa91a
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
