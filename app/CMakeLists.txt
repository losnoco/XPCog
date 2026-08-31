# The application.
#
# Two targets, and the split is load-bearing: everything but main() lives in a
# library so the models, the formatters and the embedded resources can be tested
# without opening a window. Several of the sharpest bugs this project has had were
# in exactly that code.

set(XPCOG_APP_SOURCES
    src/AboutDialog.cpp
    src/AboutDialog.hpp
    src/AppCommands.cpp
    src/AppCommands.hpp
    src/AppIcon.cpp
    src/AppIcon.hpp
    src/Commands.cpp
    src/Commands.hpp
    src/EqualizerPanel.cpp
    src/EqualizerPanel.hpp
    src/FileTree.cpp
    src/FileTree.hpp
    src/InfoPanel.cpp
    src/InfoPanel.hpp
    src/LastFmAccount.cpp
    src/LastFmAccount.hpp
    src/Localization.cpp
    src/Localization.hpp
    src/LucideIcon.cpp
    src/LucideIcon.hpp
    src/LyricsPanel.cpp
    src/LyricsPanel.hpp
    src/MainFrame.cpp
    src/MainFrame.hpp
    src/MiniFrame.cpp
    src/MiniFrame.hpp
    src/OpenUrlDialog.cpp
    src/OpenUrlDialog.hpp
    src/PlaybackController.cpp
    src/PlaybackController.hpp
    src/PlaylistDataModel.cpp
    src/PlaylistDataModel.hpp
    src/PreferencesDialog.cpp
    src/PreferencesDialog.hpp
    src/SeekBar.cpp
    src/SeekBar.hpp
    src/SettingEffect.cpp
    src/SettingEffect.hpp
    src/SingleInstance.cpp
    src/SingleInstance.hpp
    src/SpectrumPanel.cpp
    src/SpectrumPanel.hpp
    src/SpeedCurve.hpp
    src/SpeedPanel.cpp
    src/SpeedPanel.hpp
    src/StatusPresence.cpp
    src/StatusPresence.hpp
    src/Text.hpp
    src/XPCogApp.cpp
    src/XPCogApp.hpp
)

# The Last.fm API credentials, blank unless configured with them.
#
# Cog ships exactly this way -- `Xcode-config/Secrets.template.xcconfig` carries
# two empty values and a script turns them into a Swift file -- and for the same
# two reasons: a key committed to a public repository is a key that gets revoked,
# and the key identifies the *application*, so a fork submitting under XPCog's
# identity would be misattributing somebody's listening history.
#
# A build without them compiles all of the scrobbling code and reports the
# feature as unavailable, which is the same shape as XPCOG_WITH_SENTRY.
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
    message(STATUS "Last.fm: no API key; scrobbling will report itself unavailable")
endif()

configure_file(src/LastFmSecrets.hpp.in
               "${CMAKE_CURRENT_BINARY_DIR}/generated/LastFmSecrets.hpp" @ONLY)

add_library(xpcog-appcore STATIC ${XPCOG_APP_SOURCES})

target_include_directories(xpcog-appcore
    PUBLIC  src
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_link_libraries(xpcog-appcore PUBLIC
    xpcog-core
    xpcog-codecs
    xpcog-platform
    XPCog::wx
    PRIVATE XPCog::warnings)

set_target_properties(xpcog-appcore PROPERTIES FOLDER app)

# ---------------------------------------------------------------------------
# Resources.
#
# What qt_add_resources() used to do, minus the toolkit -- see
# cmake/XPCogResources.cmake for why wx has no equivalent and what replaced it.
#
# Attached to the library rather than to the executable because the code that
# reads them is here and the tests link the library. Without that, a test could
# only check that the sizes list is well-formed, never that the files behind it
# exist -- which is the failure that actually happens: a renamed PNG degrades the
# icon silently, because a request for 16px is answered with a scaled 256px and
# nothing reports an error.
# ---------------------------------------------------------------------------
xpcog_embed_resources(
    TARGET xpcog-appcore
    NAME   icons
    BASE   "${CMAKE_CURRENT_SOURCE_DIR}/icons"
    FILES
        icons/xpcog-16.png
        icons/xpcog-24.png
        icons/xpcog-32.png
        icons/xpcog-48.png
        icons/xpcog-64.png
        icons/xpcog-128.png
        icons/xpcog-256.png
        # Lucide (https://lucide.dev), ISC -- see icons/lucide/LICENSE. Kept as
        # SVG source rather than baked to PNGs because the stroke colour is not
        # knowable until the system appearance is.
        icons/lucide/play.svg
        icons/lucide/pause.svg
        icons/lucide/square.svg
        icons/lucide/skip-back.svg
        icons/lucide/skip-forward.svg
        icons/lucide/audio-lines.svg
        icons/lucide/sliders-vertical.svg
        icons/lucide/info.svg
        icons/lucide/panel-left.svg
        icons/lucide/folder-open.svg
        icons/lucide/x.svg
)

# The translations. See cmake/XPCogCatalogs.cmake for why they are compiled in
# rather than installed as .mo files beside the executable, and app/locale/README
# for how a language is added.
#
# Attached to the library rather than to the executable for the reason the icons
# are: the code that reads them is here, and the tests link the library -- which
# is what lets a test assert that the catalogue really does assemble to something
# wxMsgCatalog will accept, rather than only that the .po parsed.
xpcog_add_catalogs(
    TARGET xpcog-appcore
    NAME   catalogs
    FILES  locale/es.po
)

# The SC-55's front panel, and the photograph of the hardware it is drawn onto.
# Conditional because both come from the MIDI build: without XPCOG_WITH_MIDI there
# is no emulator to render a panel state and nothing that produces one.
if(XPCOG_WITH_MIDI)
    target_compile_definitions(xpcog-appcore PUBLIC XPCOG_HAVE_SC55_PANEL=1)
    target_sources(xpcog-appcore PRIVATE src/Sc55Panel.cpp src/Sc55Panel.hpp)
    # 776 KiB of raw RGBA, compiled in rather than found at runtime: it is part of
    # the emulator the way its ROMs are not -- shippable, and useless on its own.
    xpcog_embed_resources(
        TARGET xpcog-appcore
        NAME   sc55
        BASE   "${CMAKE_SOURCE_DIR}/vendor/nuked-sc55/data"
        FILES  "${CMAKE_SOURCE_DIR}/vendor/nuked-sc55/data/back.data")
endif()

add_executable(xpcog-app src/main.cpp)

target_link_libraries(xpcog-app PRIVATE xpcog-appcore XPCog::warnings)

# ---------------------------------------------------------------------------
# The crash handler.
#
# crashpad is out-of-process by design -- that is the whole reason it can report
# a crash that has already taken the player's own address space with it -- so it
# ships as a second executable that has to sit somewhere the first one can find.
# sentry-native sets *no* default for its path, and the failure without it is the
# quiet kind: sentry_init() succeeds, messages are still sent, and actual crashes
# simply never appear.
#
# So it is staged next to XPCog, which is where SentryCrashReporter.cpp looks --
# beside the *executable*, not with the assets, because inside a macOS bundle
# those are two different directories and a helper program in Resources is wrong
# for both the loader and the signature.
#
# vcpkg_copy_tools() puts it under tools/<port>/, which is the same place
# tests/CMakeLists.txt finds mpcenc. Named rather than found on PATH: what has to
# run is the handler from the same sentry-native this linked against.
if(XPCOG_WITH_SENTRY)
    set(_handler
        "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/sentry-native/crashpad_handler${CMAKE_EXECUTABLE_SUFFIX}")

    if(EXISTS "${_handler}")
        if(APPLE)
            # Contents/MacOS, beside the binary, which is where a bundled helper
            # belongs and where --deep signing will find it.
            set(_handler_dir "$<TARGET_BUNDLE_CONTENT_DIR:xpcog-app>/MacOS")
        else()
            set(_handler_dir "$<TARGET_FILE_DIR:xpcog-app>")
        endif()
        add_custom_command(TARGET xpcog-app POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${_handler}" "${_handler_dir}"
            COMMENT "Staging crashpad_handler"
            VERBATIM)
        if(UNIX AND NOT APPLE)
            install(PROGRAMS "${_handler}"
                    DESTINATION "${CMAKE_INSTALL_BINDIR}")
        endif()
    else()
        # A message rather than a fatal error: a build with no handler is a
        # player that still plays and still reports the problems it notices
        # itself. Said out loud because the alternative is finding out from an
        # empty Sentry project months later.
        message(STATUS
            "XPCog: crashpad_handler not found at ${_handler} -- this build will "
            "report messages but not crashes.")
    endif()
endif()

# ---------------------------------------------------------------------------
# The icon.
#
# Everything in this section is toolkit-independent and survived the move from Qt
# to wxWidgets untouched: it is CMake, `xcrun` and a resource script, none of
# which ever knew what drew the windows.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# The copyright line, in one place because two platforms show it and a third
# already had its own.
#
# macOS reads NSHumanReadableCopyright in Get Info and in the About panel a
# bundle gets for free; Windows reads LegalCopyright on the Details tab of the
# file's properties. Both were blank. The plist has carried
# ${MACOSX_BUNDLE_COPYRIGHT} since it was Qt's template and nothing ever set it,
# so it configured to an empty string; the version resource had no such field at
# all.
#
# The wording follows app/src/AboutDialog.cpp, which is the third place a
# listener can read it, and it names Cog for the reason the About box does: this
# is a port, and most of what is under the interface is still theirs.
#
# CMake has no \uXXXX escape, so the sign and the dash below are the characters
# themselves. This file is UTF-8 and so is the plist; the resource script has to
# be told, which is what the code page pragma in XPCog.rc.in is for.
# ---------------------------------------------------------------------------
set(XPCOG_COPYRIGHT "Copyright © 2026 the XPCog authors. Copyright © 2005–2026 Vincent Spader, Christopher Snowhill and the Cog authors.")

if(WIN32)
    # Identity, so the OS can say what this program is called. The version
    # resource is the Windows counterpart of the MACOSX_BUNDLE_* properties below;
    # see XPCog.rc.in for why it is hand-written and for what breaks without it.
    #
    # FILEVERSION is four comma-separated integers, not a dotted string.
    set(XPCOG_RC_VERSION
        "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
    # Absolute, and forward slashes doubled for the resource compiler, which
    # treats a backslash in a string as an escape.
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/icons/xpcog.ico" XPCOG_RC_ICON)
    configure_file(XPCog.rc.in "${CMAKE_CURRENT_BINARY_DIR}/XPCog.rc" @ONLY)
    target_sources(xpcog-app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/XPCog.rc")
    # So the resource is rebuilt when the artwork changes, which it would not be
    # otherwise: the .rc only names the file.
    set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/XPCog.rc"
        APPEND PROPERTY OBJECT_DEPENDS "${XPCOG_RC_ICON}")

    # The .rc now carries wx's application manifest -- the DPI-awareness one, see
    # XPCog.rc.in -- and MSVC's linker generates a manifest of its own by default.
    # Both land as resource type MANIFEST, name 1, and the build fails at the
    # resource-to-COFF step with "duplicate resource", which reads like a corrupt
    # object file rather than like two people writing the same thing. Turning the
    # generated one off is the documented way to embed your own.
    if(MSVC)
        target_link_options(xpcog-app PRIVATE "/MANIFEST:NO")
    endif()
elseif(APPLE)
    # The bundle icon, from the Icon Composer package rather than from a
    # flattened bitmap.
    #
    # icons/xpcog.icon is icon.json plus three SVGs -- the artwork the PNG
    # masters in this directory are rendered from, and the same source Cog
    # ships as Play.icon. Handing macOS the layers instead of a picture of
    # them is what earns the modern treatment: the system composes the
    # rounded-rect container, the specular highlight and the dark and tinted
    # appearances itself, at any size. A .icns can only ever be the light
    # variant, already flattened, at the ten sizes someone thought to bake.
    #
    # actool compiles it to three things -- Assets.car (the layered form),
    # a .icns rendered from the same source, and the two Info.plist keys that
    # name them. Both keys are needed, and they are not alternatives:
    # CFBundleIconName finds the catalog entry on macOS 26 and later, and
    # CFBundleIconFile is what everything older reads. A bundle carrying only
    # the first shows a blank icon on macOS 13.
    #
    # actool lives in Xcode, not in the Command Line Tools, so xcrun is asked
    # for it rather than find_program -- it is not on PATH even when it is
    # installed. Without Xcode the build falls back to the committed
    # xpcog.icns from make-icons.py. That is a degraded path and not an equal
    # one: it is hand-composed from xpcog-tile.png, whose gear overruns the
    # tile and whose tile fills the canvas edge to edge, where Apple's
    # container insets it.
    #
    # Being present is not enough: Icon Composer arrived with Xcode 26, and an
    # older actool does not know what a .icon is. It is asked its version and
    # held to that, because the alternative is a build that fails at the
    # compile step on any machine carrying an older Xcode. CI runs macos-26 so
    # that it exercises this path rather than the fallback -- an older image
    # would still go green, which is exactly the problem.
    set(XPCOG_ICON_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/icons/xpcog.icon")
    set(XPCOG_ACTOOL_MIN_VERSION 26)

    execute_process(
        COMMAND xcrun -f actool
        OUTPUT_VARIABLE XPCOG_ACTOOL
        RESULT_VARIABLE XPCOG_ACTOOL_FOUND
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

    set(XPCOG_ACTOOL_VERSION 0)
    if(XPCOG_ACTOOL_FOUND EQUAL 0)
        # --version answers with a plist, not a version string.
        execute_process(
            COMMAND "${XPCOG_ACTOOL}" --version
            OUTPUT_VARIABLE XPCOG_ACTOOL_VERSION_PLIST
            ERROR_QUIET)
        if(XPCOG_ACTOOL_VERSION_PLIST MATCHES
                "<key>short-bundle-version</key>[ \t\r\n]*<string>([0-9]+)")
            set(XPCOG_ACTOOL_VERSION "${CMAKE_MATCH_1}")
        endif()
    endif()

    if(XPCOG_ACTOOL_FOUND EQUAL 0
            AND XPCOG_ACTOOL_VERSION GREATER_EQUAL XPCOG_ACTOOL_MIN_VERSION
            AND IS_DIRECTORY "${XPCOG_ICON_SOURCE}")
        set(XPCOG_ICON_DIR "${CMAKE_CURRENT_BINARY_DIR}/icon")
        # So editing a layer rebuilds the icon. The command names the package
        # directory, and a directory's timestamp does not move when a file
        # inside it is written.
        file(GLOB_RECURSE XPCOG_ICON_DEPS CONFIGURE_DEPENDS
            "${XPCOG_ICON_SOURCE}/*")
        add_custom_command(
            OUTPUT "${XPCOG_ICON_DIR}/Assets.car" "${XPCOG_ICON_DIR}/xpcog.icns"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${XPCOG_ICON_DIR}"
            # --app-icon names the entry, and has to match the package's own
            # basename; that name is what lands in both Info.plist keys.
            COMMAND "${XPCOG_ACTOOL}" "${XPCOG_ICON_SOURCE}"
                    --compile "${XPCOG_ICON_DIR}"
                    --platform macosx
                    --minimum-deployment-target "${CMAKE_OSX_DEPLOYMENT_TARGET}"
                    --app-icon xpcog
                    --output-partial-info-plist "${XPCOG_ICON_DIR}/icon.plist"
                    --errors --warnings
            DEPENDS ${XPCOG_ICON_DEPS}
            COMMENT "Compiling xpcog.icon"
            VERBATIM)
        set(XPCOG_BUNDLE_ICONS
            "${XPCOG_ICON_DIR}/Assets.car" "${XPCOG_ICON_DIR}/xpcog.icns")
        # The partial plist actool emits is deliberately not consumed. Its two
        # keys are known and constant, and merging a generated fragment into
        # the template would buy nothing but a build ordering problem.
        set(XPCOG_INFO_PLIST_ICON_NAME
            "\t<key>CFBundleIconName</key>\n\t<string>xpcog</string>")
    else()
        set(XPCOG_BUNDLE_ICONS "${CMAKE_CURRENT_SOURCE_DIR}/icons/xpcog.icns")
        # Absent rather than empty. LaunchServices treats an empty
        # CFBundleIconName as a name to look up, fails to find it, and the
        # fallback it then makes is not the one CFBundleIconFile names.
        set(XPCOG_INFO_PLIST_ICON_NAME "")
        if(XPCOG_ACTOOL_FOUND EQUAL 0)
            message(STATUS
                "XPCog: actool is Xcode ${XPCOG_ACTOOL_VERSION}, and .icon needs "
                "${XPCOG_ACTOOL_MIN_VERSION} -- bundling the committed xpcog.icns "
                "instead of icons/xpcog.icon.")
        else()
            message(STATUS
                "XPCog: actool not found -- bundling the committed xpcog.icns "
                "instead of icons/xpcog.icon. Install Xcode for the layered icon.")
        endif()
    endif()

    # MACOSX_PACKAGE_LOCATION is what copies these into Contents/Resources,
    # which is where CFBundle looks.
    target_sources(xpcog-app PRIVATE ${XPCOG_BUNDLE_ICONS})
    set_source_files_properties(${XPCOG_BUNDLE_ICONS} PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)

    # The banks go in beside them, in a subdirectory of their own, which is what
    # assetPath() looks for: Contents/Resources/soundfonts. Cog puts its at the
    # top of Resources instead, which is fine for three files and stops being
    # fine when a second kind of asset arrives.
    target_sources(xpcog-app PRIVATE ${XPCOG_SOUNDFONTS})
    set_source_files_properties(${XPCOG_SOUNDFONTS} PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources/soundfonts
        HEADER_FILE_ONLY ON)

    # The equaliser's preset library, under the same arrangement. Cog puts its
    # copy at the top of Resources; this follows the subdirectory rule the banks
    # already established, so assetPath("equalizer/Cog.q1.json") is one lookup
    # everywhere rather than one per platform.
    target_sources(xpcog-app PRIVATE ${XPCOG_EQ_PRESETS})
    set_source_files_properties(${XPCOG_EQ_PRESETS} PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources/equalizer
        HEADER_FILE_ONLY ON)
endif()

# Everywhere else the lookup is relative to the executable, so the staged copy
# beside it is the one that is found -- in the build tree and, on Windows, in
# the deployed folder, which is that same directory.
add_dependencies(xpcog-app xpcog-assets)

set_target_properties(xpcog-app PROPERTIES
    OUTPUT_NAME "XPCog"
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
    MACOSX_BUNDLE_BUNDLE_NAME "XPCog"
    MACOSX_BUNDLE_GUI_IDENTIFIER "co.losno.xpcog"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_ICON_FILE "xpcog"
    MACOSX_BUNDLE_COPYRIGHT "${XPCOG_COPYRIGHT}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    FOLDER app)

# ---------------------------------------------------------------------------
# The one bundled library.
#
# The macOS triplet is static, so every dependency is linked into the executable
# and the bundle needs no Frameworks directory -- with one exception, and it
# cannot stop being one. Only vgmstream's *shared* target carries install rules
# and a CMake export; the static library it also builds is for its own CLI and
# is never installed. So ports/vgmstream forces BUILD_SHARED_LIBS and vcpkg
# hands back libvgmstream.dylib whatever the triplet asked for. See
# ports/vgmstream/portfile.cmake for the rest of why that port looks like it
# does.
#
# Its install name is @rpath/libvgmstream.dylib, and left alone CMake resolves
# that with the absolute path of this build's vcpkg_installed/lib. That works on
# the machine that produced it and nowhere else: the bundle cannot be moved to
# another Mac, and it cannot be moved to another directory on this one once the
# build tree is gone. Two things fix it together, and neither is enough alone.
#
# The dylib is staged into Contents/Frameworks, which is where the loader, a
# --deep signature and notarisation all expect a bundled library to live -- not
# beside the binary in MacOS/, which is for helper *programs* like
# crashpad_handler above.
#
# And BUILD_WITH_INSTALL_RPATH replaces the build-tree rpath with
# @executable_path/../Frameworks rather than adding to it. Adding would leave
# the absolute path first in the search order, so the staged copy would never be
# the one loaded and the local build would keep testing the arrangement that is
# not shipped. There is nothing else in that rpath to preserve: libvgmstream is
# the only @rpath dependency this executable has, and the dylib itself needs
# nothing but system libraries.
#
# Guarded on the target rather than on XPCOG_WITH_VGMSTREAM so that a build with
# the codec off gets no Frameworks directory and no rpath it has no use for.
# ---------------------------------------------------------------------------
if(APPLE AND TARGET vgmstream::vgmstream)
    set_target_properties(xpcog-app PROPERTIES
        BUILD_WITH_INSTALL_RPATH ON
        INSTALL_RPATH "@executable_path/../Frameworks")

    add_custom_command(TARGET xpcog-app POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_BUNDLE_CONTENT_DIR:xpcog-app>/Frameworks"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:vgmstream::vgmstream>"
                "$<TARGET_BUNDLE_CONTENT_DIR:xpcog-app>/Frameworks"
        COMMENT "Staging libvgmstream into Contents/Frameworks"
        VERBATIM)
endif()

# ---------------------------------------------------------------------------
# Deployment: there is none, and that is the point.
#
# The Qt build needed `cmake --build <dir> --target deploy` before the binary
# could leave the build tree: windeployqt copied the Qt runtime and its platform
# plugins beside XPCog.exe, macdeployqt copied the frameworks into the bundle and
# rewrote every load command. Windows has no rpath, so a freshly built exe could
# not even start from Explorer without it.
#
# wxWidgets comes from vcpkg like everything else, and vcpkg's applocal post-build
# step already copies every dependent DLL beside the executable -- the same step
# that has been placing FFmpeg's and TagLib's all along. On macOS the triplet is
# static, so the only thing to copy is libvgmstream, which the section above
# does.
#
# What remains is signing, which was never the deploy tool's real job -- and
# which lives in packaging/macos/ now, beside the disk image it is a step of.
# `sign`, `dmg` and `notarize` are defined there; XPCOG_CODESIGN_IDENTITY is
# declared there too. This file's business with the bundle ends at the bytes
# going into it, which is what the two sections above are.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Installing, which is Linux only.
#
# Windows harvests the whole output directory (packaging/windows/Harvest.cmake
# says why that beats install() there) and macOS ships the bundle the sections
# above assembled, so neither has ever needed a rule here. Linux has no such
# container: the player is a file in bin/, its data is a directory under share/,
# and the desktop only finds the second from the first because
# core/include/xpcog/core/AssetPath.hpp looks at <exe dir>/../share/xpcog.
#
# That layout is the one the build tree already uses -- assets/CMakeLists.txt
# stages the same files beside the same binary -- so an installed player
# exercises the lookup the developer has been exercising all along, rather than
# a second one that only runs on someone else's machine.
#
# The desktop integration that goes with this -- the .desktop file, the icons a
# launcher and a task list resolve through the hicolor theme, and the AppStream
# metainfo -- is in packaging/linux/, beside the identity all three have to
# share. Not the tray icon, which sends its artwork as pixmaps over D-Bus and
# needs no installed theme icon; see SniTrayIcon.cpp.
# ---------------------------------------------------------------------------
if(UNIX AND NOT APPLE)
    install(TARGETS xpcog-app RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    # Without this the installed binary does not start: see
    # cmake/XPCogInstallRuntime.cmake for which library and why.
    xpcog_install_linux_runtime(xpcog-app)
endif()
