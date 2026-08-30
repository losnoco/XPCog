# Desktop integration for Linux: the three files that make an installed XPCog an
# application rather than a binary in bin/.
#
# Unlike packaging/windows/ and packaging/macos/, nothing here is a target you
# ask for. There is no `installer` and no `dmg` to build, because on Linux the
# artefact is the install tree itself -- `cmake --install`, and then whatever
# wraps it: a tarball, a distribution package, a Flatpak. So this directory adds
# install() rules and two configured text files, and that is all.
#
# --- Why these three files, and not just a binary --------------------------
#
# The .desktop file is what an application launcher, a file manager's "Open
# with" and the shell's task list all read; without one, an installed XPCog is
# reachable only from a terminal and opens no file by double-click.
#
# It also closes something already shipping in the code:
# platform/src/linux/GDbusMediaIntegration.cpp publishes an MPRIS DesktopEntry,
# which is how a panel gets from "some player is playing" to this application's
# icon and localised name. It answered "" for as long as there was no .desktop
# file to name, and now names this one.
#
# The icons are for that file's Icon= key -- the launcher, the task list and the
# software centre. Note that they are *not* what draws the tray icon:
# SniTrayIcon.cpp answers the StatusNotifierItem IconName property with the
# empty string and sends the artwork as pixmaps over D-Bus instead, so the tray
# has never depended on an installed theme icon and still does not.
#
# The AppStream metainfo is the third because a software centre shows nothing
# useful without it, and because Flathub requires it -- see the Flatpak note in
# README's Linux section.

# The date of the release this version number describes, for the metainfo's
# <releases> entry. The one field here that is hand-maintained: the version
# itself comes from project(), per CLAUDE.md, but AppStream wants a date beside
# it and CMake must not invent one -- configuring TODAY would make the installed
# metadata differ between two builds of identical source.
set(XPCOG_RELEASE_DATE "2026-08-30")

# ---------------------------------------------------------------------------
# What this build can open, as MIME types.
#
# Assembled from the options rather than written out once, because every codec
# is optional: a desktop file that offers XPCog for audio/x-musepack when
# XPCOG_WITH_MUSEPACK was off produces a file manager entry that opens a player
# which then cannot play the file.
#
# The types are the ones the codecs themselves register -- the kMimeTypes arrays
# the registrars hand to PluginRegistry -- so this list and codec selection
# cannot disagree about what a type means. Two deliberate exceptions are marked
# below, where a codec selects on extension alone and the desktop needs a name
# for the same thing.
# ---------------------------------------------------------------------------
set(_mime)

if(XPCOG_WITH_FLAC)
    list(APPEND _mime "audio/flac" "audio/x-flac")
endif()
if(XPCOG_WITH_VORBIS)
    list(APPEND _mime "audio/ogg" "application/ogg" "audio/x-vorbis+ogg")
endif()
if(XPCOG_WITH_OPUS)
    list(APPEND _mime "audio/opus" "audio/x-opus+ogg")
endif()
if(XPCOG_WITH_MINIMP3)
    list(APPEND _mime "audio/mpeg" "audio/mp3" "audio/x-mp3")
endif()
if(XPCOG_WITH_WAVPACK)
    list(APPEND _mime "audio/x-wavpack" "audio/wavpack")
endif()
if(XPCOG_WITH_MUSEPACK)
    list(APPEND _mime "audio/x-musepack" "audio/musepack")
endif()
if(XPCOG_WITH_OPENMPT)
    list(APPEND _mime "audio/x-mod" "audio/x-it" "audio/x-xm" "audio/x-s3m")
endif()
if(XPCOG_WITH_FFMPEG)
    list(APPEND _mime "audio/aac" "audio/mp4" "audio/x-ms-wma" "audio/ac3"
                      "audio/vnd.dolby.dd-raw" "audio/x-ape" "audio/x-tta")
endif()
if(XPCOG_WITH_APL)
    list(APPEND _mime "application/x-apl")
endif()
if(XPCOG_WITH_CUESHEET)
    list(APPEND _mime "application/x-cue")
endif()
if(XPCOG_WITH_PLAYLISTS)
    list(APPEND _mime "audio/x-mpegurl" "audio/mpegurl" "audio/x-scpls")
endif()
if(XPCOG_WITH_HLS)
    list(APPEND _mime "application/vnd.apple.mpegurl" "application/x-mpegurl")
endif()
if(XPCOG_WITH_MIDI)
    # The first exception: codecs/midi registers extensions and no MIME types --
    # selection there is by extension, and the fourteen it claims have no agreed
    # types between them. These two are what shared-mime-info calls a .mid, which
    # is what a file manager will actually hand over.
    list(APPEND _mime "audio/midi" "audio/x-midi")
endif()
if(XPCOG_WITH_DSD)
    # The second, and the same reasoning: DSF and DFF are extension-selected.
    list(APPEND _mime "audio/x-dsf" "audio/x-dff")
endif()

# HLS and the playlist reader both claim application/x-mpegurl, and a desktop
# file that lists a type twice is invalid rather than merely redundant.
list(REMOVE_DUPLICATES _mime)

# The spec wants a trailing semicolon on a list-valued key, and CMake's own list
# separator is already the one the key uses.
string(JOIN ";" XPCOG_DESKTOP_MIME_TYPES ${_mime})
string(APPEND XPCOG_DESKTOP_MIME_TYPES ";")

# ---------------------------------------------------------------------------
# The files themselves.
# ---------------------------------------------------------------------------
set(_desktop  "${CMAKE_CURRENT_BINARY_DIR}/${XPCOG_DESKTOP_ID}.desktop")
set(_metainfo "${CMAKE_CURRENT_BINARY_DIR}/${XPCOG_DESKTOP_ID}.metainfo.xml")

configure_file(co.losno.XPCog.desktop.in      "${_desktop}"  @ONLY)
configure_file(co.losno.XPCog.metainfo.xml.in "${_metainfo}" @ONLY)

install(FILES "${_desktop}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install(FILES "${_metainfo}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")

# The icons, renamed on the way in. app/icons/ names them for the size because
# that is what the Windows .ico and the macOS iconset were built from; the
# hicolor theme puts the size in the directory instead and wants every file
# named for the application. Flatpak requires that name specifically, and the
# tray icon's IconName resolves through the same theme.
#
# No scalable/ entry: the SVG artwork this project has is the layered
# app/icons/xpcog.icon package, which is three layers meant for Icon Composer to
# compose, not one flat SVG a GTK icon loader can render. Adding a flattened one
# is worth doing and is not this change.
foreach(_size IN ITEMS 16 24 32 48 64 128 256)
    install(FILES "${CMAKE_SOURCE_DIR}/app/icons/xpcog-${_size}.png"
            DESTINATION
                "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_size}x${_size}/apps"
            RENAME "${XPCOG_DESKTOP_ID}.png")
endforeach()

# Neither gtk-update-icon-cache nor update-desktop-database is run here. They
# are the responsibility of whatever installs the tree -- every distribution
# package manager has a trigger for both -- and running them from an install
# rule would write outside DESTDIR, which is precisely what breaks packaging.
# README's Linux section says what to run after a manual `cmake --install`.

# ---------------------------------------------------------------------------
# The tarball.
#
#     cmake --build build/linux-repo-release --target package
#
# produces build/linux-repo-release/XPCog-<version>-<arch>.tar.gz, which is the
# Linux counterpart of `installer` on Windows and `dmg` on macOS -- one artefact
# to attach to a release, for someone who wants to run XPCog without building
# it.
#
# --- Why CPack here, when Windows deliberately does not --------------------
#
# packaging/windows/CMakeLists.txt sets out why the installer is an NSIS script
# and not CPACK_NSIS: CPack packages what install() rules say to package, and
# there were none for the application. That was the argument against it, and on
# Linux it is now the argument for it -- the install rules above are complete
# and tested, so CPack has something to read and there is nothing left for a
# hand-written packager to add. `package` is `cmake --install` into a staging
# directory, compressed.
#
# --- What this tarball is, and is not --------------------------------------
#
# It is not an AppImage and does not pretend to be. wxWidgets, GTK and most of
# the codec libraries are the distribution's, linked dynamically, so the tarball
# runs on the distribution release that built it or a compatible newer one -- it
# is a convenience for that case, not a portable binary for any Linux.
#
# The tree inside it is relocatable in the two ways that took work to arrange:
# the player finds its shipped assets through <exe dir>/../share/xpcog and
# libvgmstream through an $ORIGIN/../lib rpath, so bin/, lib/ and share/ can sit
# anywhere as long as they sit together.
#
# One file in it is not relocatable, and it is worth knowing which. The desktop
# file's Exec is the absolute path this tree was *configured* for -- see
# co.losno.XPCog.desktop.in for why a bare name is worse -- so unpacking the
# archive somewhere other than that prefix gives a working player whose menu
# entry launches nothing. Unpack it at the prefix it was built for, which is
# what README's Linux section says to do, or edit the one line.
# ---------------------------------------------------------------------------

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    # The same warning the Windows installer gives, for a smaller reason: a Debug
    # tarball is enormous and slow rather than unredistributable. Still not what
    # anyone means to publish.
    message(STATUS
        "XPCog: the `package` target in a Debug tree produces a debug tarball. "
        "Use linux-release or linux-repo-release to ship.")
endif()

set(CPACK_PACKAGE_NAME "XPCog")
set(CPACK_PACKAGE_VENDOR "The XPCog authors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://cog.losno.co/xpcog")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/COPYING")

# TGZ alone. Not DEB or RPM: CPack can emit both, but a package worth installing
# needs a dependency list, and CPACK_DEBIAN_PACKAGE_SHLIBDEPS would derive one
# naming the exact sonames of the distribution that happened to build it. That
# is a per-distribution job done properly by a per-distribution packager, and
# `cmake --install` is what such a packager needs from us.
set(CPACK_GENERATOR "TGZ")

# And explicitly no source package. CPack's default source generator on Unix
# would add a `package_source` target that tarballs the working tree, build
# directories and all; `git archive` is the right tool for that and is nobody's
# accident.
set(CPACK_SOURCE_GENERATOR "")

# XPCog-1.0.0-x86_64.tar.gz, which is the Windows installer's name with the
# platform's own idea of the architecture. CPack's default spells the system and
# not the machine, so a 64-bit build and a build for anything else would both be
# "Linux".
set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")

# 120 MB of executable becomes 24. The symbols stay in the build tree, which is
# where a crash report is symbolicated from and where a debugger is pointed;
# what is dropped here is a copy of them in a download, which nothing reads.
set(CPACK_STRIP_FILES ON)

include(CPack)
