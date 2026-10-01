# The application layer that links no toolkit.
#
# `app/` used to be all of it, and the Qt-to-wx move never had to ask how much
# of it was really about a toolkit -- the answer was "rewrite it and find out".
# Building a second frontend (docs/GTKPORT.md) asks the question properly, and this
# is the answer: the playback controller, the command tables, the playlist edits,
# the remote control's player implementation, the setting-effect table and the
# catalogue lookup. None of it names a widget.
#
# **The namespace is still `xpcog::app`, and that is deliberate.** It names the
# layer -- the application above core -- not the library and not the toolkit.
# Renaming it would have touched about 270 references across two dozen files and
# said nothing the directory does not already say. What keeps the split honest is
# not a namespace but cmake/CheckNoToolkit.cmake, which fails the build on a
# `#include <wx/...>` anywhere under here, the same way it has failed on Qt since
# M0.
#
# Include directories are flat, matching app/'s own style, so a header that moved
# here is included by the name it always had.

# The Last.fm API credentials, blank unless configured with them.
#
# Cog ships exactly this way -- `Xcode-config/Secrets.template.xcconfig` carries
# two empty values and a script turns them into a Swift file -- and for the same
# two reasons: a key committed to a public repository is a key that gets revoked,
# and the key identifies the *application*, so a fork submitting under XPCog's
# identity would be misattributing somebody's listening history.
#
# A build without them compiles all of the scrobbling code and reports itself as
# carrying no key, which is the same shape as XPCOG_WITH_SENTRY -- except that
# the listener can enter a key of their own in the preferences pane.
set(XPCOG_LASTFM_API_KEY "" CACHE STRING
    "Last.fm API key. Apply at https://www.last.fm/api/account/create")
set(XPCOG_LASTFM_API_SECRET "" CACHE STRING
    "Last.fm shared secret that goes with XPCOG_LASTFM_API_KEY")
mark_as_advanced(XPCOG_LASTFM_API_SECRET)

# Falling back to the environment lets the secret stay off the command line.
#
# `-D XPCOG_LASTFM_API_SECRET=...` puts it in the shell's history and, more
# durably, in CMakeCache.txt -- so a value fetched out of a password manager is
# copied straight back into plaintext next to the build. Reading the environment
# instead means this works, with nothing quotable left behind:
#
#     op run --env-file=lastfm.env -- cmake --preset windows-debug
#
# or, without an env file:
#
#     $env:XPCOG_LASTFM_API_KEY = op item get "Last.fm XPCog" --fields username --reveal
#
# The cache variable still wins when it is set, so nothing that worked before
# changes. Note the value is compiled into the binary either way -- that is what
# a client API key is, and Cog does the same -- so this is about not leaving a
# second copy lying around, not about hiding it.
foreach(_lastfm_var XPCOG_LASTFM_API_KEY XPCOG_LASTFM_API_SECRET)
    if(NOT ${_lastfm_var} AND DEFINED ENV{${_lastfm_var}})
        set(${_lastfm_var} "$ENV{${_lastfm_var}}")
        # Deliberately not written back to the cache: a cached copy would
        # outlive the environment that supplied it and defeat the point.
    endif()
endforeach()

if(XPCOG_LASTFM_API_KEY AND XPCOG_LASTFM_API_SECRET)
    message(STATUS "Last.fm: API key configured")
else()
    message(STATUS "Last.fm: no API key; scrobbling needs one entered in preferences")
endif()

configure_file(src/LastFmSecrets.hpp.in
               "${CMAKE_CURRENT_BINARY_DIR}/generated/LastFmSecrets.hpp" @ONLY)

add_library(xpcog-uicore STATIC
    src/AppCommands.cpp
    src/AppCommands.hpp
    src/AppPlayerControl.cpp
    src/AppPlayerControl.hpp
    src/Commands.cpp
    src/Credits.cpp
    src/Commands.hpp
    src/LastFmAccount.cpp
    src/LastFmAccount.hpp
    src/ListenBrainzAccount.cpp
    src/ListenBrainzAccount.hpp
    src/LyricsText.cpp
    src/LyricsText.hpp
    src/PlaybackController.cpp
    src/PlaybackController.hpp
    src/RemoteJobs.cpp
    src/RemoteJobs.hpp
    src/Session.cpp
    src/Session.hpp
    src/RemoteToken.cpp
    src/RemoteToken.hpp
    src/SettingChoices.cpp
    src/SettingChoices.hpp
    src/SettingEffect.cpp
    src/SettingEffect.hpp
    src/SpeedCurve.hpp
    src/TrackText.cpp
    src/TrackText.hpp
    src/TrayMenu.cpp
    src/TrayMenu.hpp
    src/Translations.cpp
    src/Translations.hpp
    src/UrlHistory.cpp
    src/UrlHistory.hpp
    src/VisualizerChannels.cpp
    src/VisualizerChannels.hpp
)

add_library(XPCog::uicore ALIAS xpcog-uicore)

target_include_directories(xpcog-uicore
    PUBLIC  src
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# platform is here because PlaybackController reports to the crash reporter.
# codecs is *not*: nothing under src/ includes a decoder, and the frontends link
# it themselves.
target_link_libraries(xpcog-uicore
    PUBLIC  xpcog-core xpcog-platform
    PRIVATE XPCog::warnings)

set_target_properties(xpcog-uicore PROPERTIES FOLDER uicore)

# The translations, which moved here with the lookup that reads them.
#
# Attached to this target rather than to the application for the reason they were
# attached to the wx player's library before -- the code that reads them is here, and the
# tests link it -- and now for a second reason: both frontends need the same
# catalogue, and a catalogue compiled into one of them is not a catalogue the
# other has. See uicore/locale/README.md for how a language is added.
xpcog_add_catalogs(
    TARGET xpcog-uicore
    NAME   catalogs
    FILES  locale/es.po
)
