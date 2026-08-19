# 2SF: Nintendo DS rips, on melonDS (vendored as vio2sf; see that CMakeLists
# for why the name does not match the emulator).
#
# zlib directly: the `reserved` section holds deflated SAVE chunks with their
# own CRC, inflated in the decoder rather than by psflib.
find_package(ZLIB REQUIRED)

xpcog_add_codec(
    NAME     twosf
    REGISTER xpcog_register_twosf
    SOURCES  TwoSfDecoder.cpp
    DEPS     xpcog-codec-psf vendor-vio2sf ZLIB::ZLIB xpcog-codec-common
)
