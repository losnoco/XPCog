# SNSF: Super Nintendo rips, on snes9x.
xpcog_add_codec(
    NAME     snsf
    REGISTER xpcog_register_snsf
    SOURCES  SnsfDecoder.cpp
    DEPS     xpcog-codec-psf vendor-snes9x xpcog-codec-common
)
