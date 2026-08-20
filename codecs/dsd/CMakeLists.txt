# No external dependency: both containers are read here, and the decimation the
# audio then goes through belongs to core (vendor/dsd2pcm, via AudioConverter).
# See DsdDecoder.cpp for why this is not part of codecs/ffmpeg.
xpcog_add_codec(
    NAME     dsd
    REGISTER xpcog_register_dsd
    SOURCES  DsdDecoder.cpp
    DEPS     xpcog-codec-common
)
