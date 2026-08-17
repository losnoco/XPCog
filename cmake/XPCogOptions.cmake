# Build-shape options. Codec toggles live next to their xpcog_add_codec() call.

option(XPCOG_BUILD_APP   "Build the Qt application"        ON)
option(XPCOG_BUILD_CLI   "Build the headless xpcog-cli"    ON)
option(XPCOG_BUILD_TESTS "Build the test suite"            ON)

option(XPCOG_WITH_FLAC   "FLAC decoder"                    ON)
option(XPCOG_WITH_PLAYLISTS "M3U and PLS playlist containers" ON)

# Deferred to later milestones; the options exist now so the wiring is stable.
option(XPCOG_WITH_FFMPEG    "FFmpeg decoder (M1b)"         OFF)
option(XPCOG_WITH_VORBIS    "Ogg Vorbis decoder"           ON)
option(XPCOG_WITH_OPUS      "Opus decoder"                 ON)
option(XPCOG_WITH_MPG123    "MP3 decoder (libmpg123)"      ON)
option(XPCOG_WITH_WAVPACK   "WavPack decoder"              ON)
option(XPCOG_WITH_APE       "Monkey's Audio decoder (M1b)" OFF)
option(XPCOG_WITH_MUSEPACK  "Musepack decoder (M1b)"       OFF)
option(XPCOG_WITH_TAGLIB    "TagLib metadata (M1b)"        OFF)
option(XPCOG_WITH_DSD       "DSD / DoP support (M6)"       OFF)

# Sanitizers are opt-in; the RT-safety work in M1a wants them available early.
set(XPCOG_SANITIZE "" CACHE STRING "Sanitizers, e.g. address;undefined")
