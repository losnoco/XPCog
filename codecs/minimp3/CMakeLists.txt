xpcog_add_codec(
    NAME     minimp3
    REGISTER xpcog_register_minimp3
    SOURCES  Mp3Decoder.cpp
    DEPS     vendor-minimp3 xpcog-codec-common
)
