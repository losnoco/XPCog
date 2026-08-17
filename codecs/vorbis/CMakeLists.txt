find_package(Vorbis CONFIG REQUIRED GLOBAL)

xpcog_add_codec(
    NAME     vorbis
    REGISTER xpcog_register_vorbis
    SOURCES  VorbisDecoder.cpp
    DEPS     Vorbis::vorbisfile xpcog-codec-common
)
