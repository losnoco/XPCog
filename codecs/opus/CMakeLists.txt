find_package(OpusFile CONFIG REQUIRED GLOBAL)

xpcog_add_codec(
    NAME     opus
    REGISTER xpcog_register_opus
    SOURCES  OpusDecoder.cpp
    DEPS     OpusFile::opusfile xpcog-codec-common
)
