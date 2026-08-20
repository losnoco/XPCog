# Network is here for QLocalServer, not for anything that touches a network:
# Qt puts the local socket classes in that module. The single-instance handler
# is the only user, and it is why the deployed payload carries Qt6Network.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Svg LinguistTools)

qt_standard_project_setup(REQUIRES 6.5)

# Named so both the library and the translation extraction can use them: the
# strings live here, but the .qm resource has to link into the executable.
set(XPCOG_APP_SOURCES
    src/ActionRegistry.cpp
    src/ActionRegistry.hpp
    src/AppIcon.cpp
    src/AppIcon.hpp
    src/Appearance.cpp
    src/Appearance.hpp
    src/PlaybackController.cpp
    src/PlaybackController.hpp
    src/LucideIcon.cpp
    src/LucideIcon.hpp
    src/FileTree.cpp
    src/FileTree.hpp
    src/SeekSlider.cpp
    src/SeekSlider.hpp
    src/PlaylistModel.cpp
    src/PlaylistModel.hpp
    src/SingleInstance.cpp
    src/SingleInstance.hpp
    src/SpectrumWidget.cpp
    src/SpectrumWidget.hpp
    src/StatusPresence.cpp
    src/StatusPresence.hpp
    src/windows/AboutDialog.cpp
    src/windows/AboutDialog.hpp
    src/windows/MainWindow.cpp
    src/windows/MainWindow.hpp
    src/windows/EqualizerPanel.cpp
    src/windows/EqualizerPanel.hpp
    src/windows/InfoPanel.cpp
    src/windows/InfoPanel.hpp
    src/windows/OpenUrlDialog.cpp
    src/windows/OpenUrlDialog.hpp
    src/windows/MiniWindow.cpp
    src/windows/MiniWindow.hpp
    src/windows/PreferencesDialog.cpp
    src/windows/PreferencesDialog.hpp
)

# Everything but main() lives in a library, so the models can be tested without
# starting a window. A QAbstractItemModel is exactly the kind of code that
# breaks silently -- a sort that scrambles rows looks like a redraw glitch --
# and it is not reachable from xpcog-cli, which links no Qt.
# The SC-55's front panel, and the photograph of the hardware it is drawn onto.
# Conditional because both come from the MIDI build: without XPCOG_WITH_MIDI
# there is no emulator to render a panel state and nothing that produces one.
if(XPCOG_WITH_MIDI)
    list(APPEND XPCOG_APP_SOURCES
        src/windows/Sc55PanelWidget.cpp
        src/windows/Sc55PanelWidget.hpp)
endif()

qt_add_library(xpcog-appcore STATIC ${XPCOG_APP_SOURCES})

if(XPCOG_WITH_MIDI)
    target_compile_definitions(xpcog-appcore PUBLIC XPCOG_HAVE_SC55_PANEL=1)
    # 776 KiB of raw RGBA, compiled in rather than found at runtime: it is part
    # of the emulator the way its ROMs are not -- shippable, and useless on its
    # own.
    qt_add_resources(xpcog-appcore "sc55panel"
        PREFIX "/sc55"
        BASE "${CMAKE_SOURCE_DIR}/vendor/nuked-sc55/data"
        FILES "${CMAKE_SOURCE_DIR}/vendor/nuked-sc55/data/back.data")
endif()

target_include_directories(xpcog-appcore PUBLIC src)

target_link_libraries(xpcog-appcore PUBLIC
    xpcog-core
    xpcog-codecs
    xpcog-platform
    Qt6::Widgets
    Qt6::Network
    # Svg, because the Lucide icons are recoloured from their source before
    # being rasterised -- see src/LucideIcon.cpp for why that cannot be a PNG.
    Qt6::Svg
    PRIVATE XPCog::warnings)

set_target_properties(xpcog-appcore PROPERTIES FOLDER app)

# ---------------------------------------------------------------------------
# The icon.
#
# Attached to the library rather than to the executable -- unlike the
# translations below -- because AppIcon.cpp reads it and the tests link the
# library. Without that, a test could only check that the sizes list is
# well-formed, never that the files behind it exist, which is the failure that
# actually happens: a renamed PNG degrades the icon silently, since Qt answers a
# request for 16px with a scaled 256px and reports no error.
#
# The PNG set is the free-form artwork. The .ico and .icns are not here; they go
# to the platforms' own icon slots further down.
# ---------------------------------------------------------------------------
qt_add_resources(xpcog-appcore "icons"
    PREFIX "/icons"
    BASE icons
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
        # knowable until the palette is.
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

qt_add_executable(xpcog-app
    src/main.cpp
)

# Identity, so the OS can say what this program is called. The version resource
# is the Windows counterpart of the MACOSX_BUNDLE_* properties below; see
# XPCog.rc.in for why it is hand-written rather than left to Qt, and for what
# breaks without it.
if(WIN32)
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
    # Naming a template is what stops Qt reaching for its own; see Info.plist.in
    # for what was added to it and which pass substitutes what.
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    FOLDER app)

target_link_libraries(xpcog-app PRIVATE
    xpcog-appcore
    XPCog::warnings)

# ---------------------------------------------------------------------------
# Translations: none, on purpose.
#
# The interface is English only for now. The tr() calls stay -- they cost
# nothing and are what a catalogue would be extracted from -- but there are no
# .ts files, no qt_add_translations() and no QTranslator, so nothing has to be
# kept in step with strings that are still being written. Adding a language
# back is one qt_add_translations() call and the installTranslations() that
# main.cpp used to have.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# `cmake --build <dir> --target deploy` produces a self-contained bundle.
#
# Without this the application only runs on a machine with Qt installed at the
# exact path baked into its rpath, which is fine for development and useless for
# anyone else. The deploy tools copy the frameworks in and rewrite the load
# commands to @executable_path.
#
# Deliberately a separate target rather than part of ALL: it rewrites the
# binary in place and takes several seconds, and a normal build should not do
# either.
# ---------------------------------------------------------------------------
# Set to a "Developer ID Application: … (TEAMID)" identity to produce a bundle
# that can leave this machine. Left empty, macdeployqt ad-hoc signs, which is
# enough to run locally and refused by Gatekeeper anywhere else.
set(XPCOG_CODESIGN_IDENTITY "" CACHE STRING
    "macOS code signing identity for the deploy target")

if(APPLE)
    find_program(XPCOG_MACDEPLOYQT macdeployqt
        HINTS "${QT_HOST_PATH}/bin" "${Qt6_DIR}/../../../bin")
    if(XPCOG_MACDEPLOYQT)
        if(XPCOG_CODESIGN_IDENTITY)
            # -sign-for-notarization implies the hardened runtime and a secure
            # timestamp, both of which notarisation requires and neither of
            # which is the default. It needs network access for the timestamp.
            set(_sign "-sign-for-notarization=${XPCOG_CODESIGN_IDENTITY}")
        else()
            set(_sign "")
        endif()
        add_custom_target(deploy
            COMMAND "${XPCOG_MACDEPLOYQT}" "$<TARGET_BUNDLE_DIR:xpcog-app>"
                    ${_sign} -verbose=1
            DEPENDS xpcog-app
            COMMENT "Bundling Qt into XPCog.app"
            VERBATIM)
        set_target_properties(deploy PROPERTIES FOLDER app)
    endif()
elseif(WIN32)
    find_program(XPCOG_WINDEPLOYQT windeployqt
        HINTS "${QT_HOST_PATH}/bin" "${Qt6_DIR}/../../../bin")
    if(XPCOG_WINDEPLOYQT)
        # --no-opengl-sw drops opengl32sw.dll, Mesa's software GL fallback, which
        # windeployqt copies unconditionally and which is the single largest thing in
        # the payload. It was kept deliberately for a while: if the visualisation had
        # rendered through Qt Quick, that fallback is exactly what would keep the app
        # working on a machine with no usable GL driver. The spectrum draws with
        # QPainter instead (see SpectrumWidget.hpp), so nothing here needs a GL
        # context and the fallback protects nothing.
        #
        # Reinstate it the moment anything in this application starts using Qt Quick
        # or a QOpenGLWidget.
        add_custom_target(deploy
            COMMAND "${XPCOG_WINDEPLOYQT}" --no-translations --no-opengl-sw
                    "$<TARGET_FILE:xpcog-app>"
            DEPENDS xpcog-app
            COMMENT "Copying the Qt runtime beside XPCog.exe"
            VERBATIM)
        set_target_properties(deploy PROPERTIES FOLDER app)
    endif()
endif()
# Linux has no equivalent in the Qt distribution. linuxdeploy and its Qt plugin
# do the same job for an AppImage, but they are a third-party download rather
# than something to invoke from here on the assumption it exists.
