xpcog_add_codec(
    NAME     minimp3
    REGISTER xpcog_register_minimp3
    SOURCES  Mp3Decoder.cpp
    DEPS     vendor-minimp3 xpcog-codec-common
    # The fragment gate: with FFmpeg built, minimp3 refuses chapter URLs and
    # FFmpeg plays them; without it, minimp3 plays the spans itself.
    DEFINES  XPCOG_WITH_FFMPEG=$<BOOL:${XPCOG_WITH_FFMPEG}>
)
