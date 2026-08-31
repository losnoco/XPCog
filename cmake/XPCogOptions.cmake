# Build-shape options. Codec toggles live next to their xpcog_add_codec() call.

option(XPCOG_BUILD_APP   "Build the wxWidgets application" ON)
option(XPCOG_BUILD_CLI   "Build the headless xpcog-cli"    ON)
option(XPCOG_BUILD_TESTS "Build the test suite"            ON)

option(XPCOG_WITH_FLAC   "FLAC decoder"                    ON)
option(XPCOG_WITH_PLAYLISTS "M3U and PLS playlist containers" ON)
option(XPCOG_WITH_CUESHEET  "Cue sheet container and decoder"  ON)

# Deferred to later milestones; the options exist now so the wiring is stable.
option(XPCOG_WITH_FFMPEG    "FFmpeg decoder (M1b)"         OFF)
option(XPCOG_WITH_VORBIS    "Ogg Vorbis decoder"           ON)
option(XPCOG_WITH_OPUS      "Opus decoder"                 ON)
option(XPCOG_WITH_OGGCHAIN  "Chained Ogg files as subsongs" ON)
option(XPCOG_WITH_MINIMP3   "MP3 decoder (vendored minimp3)" ON)
option(XPCOG_WITH_WAVPACK   "WavPack decoder"              ON)
# APE has no option of its own and will not get one: Cog has no native
# Monkey's Audio decoder, and claims .ape from its FFmpeg plugin, which
# XPCOG_WITH_FFMPEG already does here. What Cog does have and this lacked is
# the .apl link container, which is XPCOG_WITH_APL below.
option(XPCOG_WITH_APL       "Monkey's Audio Link (.apl) container" ON)
option(XPCOG_WITH_MUSEPACK  "Musepack decoder (libmpcdec)"  OFF)
option(XPCOG_WITH_TAGLIB    "TagLib tag reading"           ON)
# MIDI is staged; see docs/MIDI.md. This switches on the container and the
# sequencer, and each synth follows behind it. OFF until one of them can
# answer, on the rule the PSF container set: a decoder that cannot decode is
# worse than a format the player does not claim.
option(XPCOG_WITH_MIDI      "MIDI sequencing (midi_processing)" OFF)
option(XPCOG_WITH_DSD       "DSD files: DSF and DSDIFF"     ON)
option(XPCOG_WITH_HTTP      "HTTP and internet radio source (libcurl)" ON)
option(XPCOG_WITH_HLS       "HTTP Live Streaming (.m3u8)"  ON)
option(XPCOG_WITH_OPENMPT   "Tracker modules (libopenmpt)" ON)
option(XPCOG_WITH_GME       "Game music rips (Game_Music_Emu)" ON)
option(XPCOG_WITH_HIVELY    "AHX and Hively tracker modules (vendored hvl_replay)" ON)
option(XPCOG_WITH_ADPLUG   "AdLib and OPL2 formats (AdPlug)" OFF)
option(XPCOG_WITH_ORGANYA  "Cave Story music (.org)" ON)
option(XPCOG_WITH_SYNTRAX  "Syntrax modules (.jxs, vendored libjaytrax)" ON)
option(XPCOG_WITH_SILENCE  "silence:// tracks, for gaps and failed opens" ON)
option(XPCOG_WITH_LIBVGM   "VGM, S98, DRO and GYM chip logs (libvgm)" OFF)
option(XPCOG_WITH_ARCHIVE   "Archive source (libarchive)" ON)
option(XPCOG_WITH_VGMSTREAM "Video-game streamed audio (vgmstream)" OFF)
option(XPCOG_WITH_PSF       "PSF container (HighlyComplete; cores land separately)" OFF)
option(XPCOG_WITH_SID       "Commodore 64 tunes (libsidplayfp)" OFF)

# The REST remote control. OFF for a bare `cmake` and ON in the presets, which is
# XPCOG_WITH_SENTRY's rule -- but off here for a second reason the others do not
# have. app/src/SingleInstance.hpp is an argument against this program owning a
# listening socket at all: "the firewall asks the user to approve a *music
# player* wanting network access, which is alarming, unanswerable and entirely
# self-inflicted." That argument stands, and it is answered twice over as well.
# This decides whether a server is *built*; `remoteEnable` decides whether it
# ever listens, and it is false by default. A packager who wants neither has this
# switch, and a build that never compiles a server is the strongest form of the
# answer.
option(XPCOG_WITH_REST "REST remote-control server (cpp-httplib)" OFF)

# Crash reporting, which is opt-in twice over: this decides whether the reporter
# is *built*, and `sentryConsented` decides whether it ever runs. OFF here for
# the same reason XPCOG_WITH_FFMPEG is -- sentry-native builds crashpad and, on
# Linux, libunwind, which is a lot to hand someone who typed plain `cmake`. The
# presets turn it on. See platform/include/xpcog/platform/CrashReporter.hpp.
option(XPCOG_WITH_SENTRY "Opt-in crash reporting (sentry-native)" OFF)

# Sanitizers are opt-in; the RT-safety work in M1a wants them available early.
set(XPCOG_SANITIZE "" CACHE STRING "Sanitizers, e.g. address;undefined")
