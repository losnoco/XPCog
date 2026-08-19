# NCSF: Nintendo DS sequenced rips, on SSEQPlayer.
#
# The only core here that plays a sequence rather than emulating a machine; see
# vendor/sseqplayer/CMakeLists.txt.
xpcog_add_codec(
    NAME     ncsf
    REGISTER xpcog_register_ncsf
    SOURCES  NcsfDecoder.cpp
    DEPS     xpcog-codec-psf vendor-sseqplayer xpcog-codec-common
)
