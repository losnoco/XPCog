# A container with no decoder of its own, like playlists: chaining is a property
# of the Ogg container rather than of FLAC, Vorbis or Opus, so it is registered
# once here instead of three times.
xpcog_add_codec(
    NAME     oggchain
    REGISTER xpcog_register_oggchain
    SOURCES  OggChainContainer.cpp
    DEPS     xpcog-codec-common
)
