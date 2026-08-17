# Network is here for QLocalServer, not for anything that touches a network:
# Qt puts the local socket classes in that module. The single-instance handler
# is the only user, and it is why the deployed payload carries Qt6Network.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network LinguistTools)

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
    src/FileTree.cpp
    src/FileTree.hpp
    src/SeekSlider.cpp
    src/SeekSlider.hpp
    src/PlaylistCommands.cpp
    src/PlaylistCommands.hpp
    src/PlaylistModel.cpp
    src/PlaylistModel.hpp
    src/ScanTask.cpp
    src/ScanTask.hpp
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
    src/windows/MiniWindow.cpp
    src/windows/MiniWindow.hpp
    src/windows/PreferencesDialog.cpp
    src/windows/PreferencesDialog.hpp
)

# Everything but main() lives in a library, so the models can be tested without
# starting a window. A QAbstractItemModel is exactly the kind of code that
# breaks silently -- a sort that scrambles rows looks like a redraw glitch --
# and it is not reachable from xpcog-cli, which links no Qt.
qt_add_library(xpcog-appcore STATIC ${XPCOG_APP_SOURCES})

target_include_directories(xpcog-appcore PUBLIC src)

target_link_libraries(xpcog-appcore PUBLIC
    xpcog-core
    xpcog-codecs
    xpcog-platform
    Qt6::Widgets
    Qt6::Network
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
    # The tile artwork, as the bundle's icon. Named without its extension in the
    # Info.plist and copied into Contents/Resources, which is where CFBundle
    # looks; MACOSX_PACKAGE_LOCATION is what puts it there.
    set(XPCOG_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/icons/xpcog.icns")
    target_sources(xpcog-app PRIVATE "${XPCOG_ICNS}")
    set_source_files_properties("${XPCOG_ICNS}" PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)
endif()

set_target_properties(xpcog-app PROPERTIES
    OUTPUT_NAME "XPCog"
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
    MACOSX_BUNDLE_BUNDLE_NAME "XPCog"
    MACOSX_BUNDLE_GUI_IDENTIFIER "co.losno.xpcog"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_ICON_FILE "xpcog"
    FOLDER app)

target_link_libraries(xpcog-app PRIVATE
    xpcog-appcore
    XPCog::warnings)

# ---------------------------------------------------------------------------
# Translations.
#
# The .qm files are compiled in as a resource under :/i18n and picked up by
# installTranslations() in main.cpp. Attached to the executable rather than to
# xpcog-appcore because that is where the resource has to end up; SOURCES
# points lupdate at the library's files, which is where the strings are.
#
# Two build targets come out of this:
#   update_translations   rescan the sources into the .ts files
#   release_translations  compile the .ts files to .qm
#
# Spanish because that is the one language Cog ships beyond English
# (Localizable.xcstrings, 284 keys plus 65 in the preferences bundle), so a
# Cog user changing over does not lose their interface language.
# ---------------------------------------------------------------------------
qt_add_translations(xpcog-app
    TS_FILES
        i18n/xpcog_en.ts
        i18n/xpcog_es.ts
    SOURCES ${XPCOG_APP_SOURCES} src/main.cpp
    QM_FILES_OUTPUT_VARIABLE XPCOG_QM_FILES
)

# QM_FILES_OUTPUT_VARIABLE turns the automatic resource embedding off, so build
# it here instead. Doing it by hand is what makes the compiled .qm paths
# available to the tests, which have no resource of their own to read.
qt_add_resources(xpcog-app "translations"
    PREFIX "/i18n"
    BASE "${CMAKE_CURRENT_BINARY_DIR}"
    FILES ${XPCOG_QM_FILES}
)

# Exported so tests/app can load a real .qm rather than assert against the .ts.
set(XPCOG_QM_FILES "${XPCOG_QM_FILES}" PARENT_SCOPE)

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
