# No library and no data: the whole thing is a URL parser and a memset.
xpcog_add_codec(
    NAME     silence
    REGISTER xpcog_register_silence
    SOURCES  SilenceDecoder.cpp
)
