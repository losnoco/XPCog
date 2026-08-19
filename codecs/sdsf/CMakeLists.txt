# SSF and DSF: Saturn and Dreamcast rips, on HighlyTheoretical.
#
# One codec for two formats because it is one emulator library for two machines
# that share a sound chip; see vendor/highlytheoretical/CMakeLists.txt.
xpcog_add_codec(
    NAME     sdsf
    REGISTER xpcog_register_sdsf
    SOURCES  SdsfDecoder.cpp
    DEPS     xpcog-codec-psf vendor-highlytheoretical xpcog-codec-common
)
