# Monkey's Audio Link files. No decoding library: an .apl names a range within
# another file, which the registry opens.
xpcog_add_codec(
    NAME     apl
    REGISTER xpcog_register_apl
    SOURCES  AplDecoder.cpp
    DEPS     xpcog-codec-common
)
