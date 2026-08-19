xpcog_add_codec(
    NAME     hls
    REGISTER xpcog_register_hls
    SOURCES  HlsDecoder.cpp HlsMemorySource.cpp HlsPlaylist.cpp HlsSegmentManager.cpp
    DEPS     xpcog-codec-common
)
