# The application.
#
# Two targets, and the split is load-bearing: everything but main() lives in a
# library so the models, the formatters and the embedded resources can be tested
# without opening a window. Several of the sharpest bugs this project has had were
# in exactly that code.

set(XPCOG_APP_SOURCES
    src/AboutDialog.cpp
    src/AboutDialog.hpp
    src/AppIcon.cpp
    src/AppIcon.hpp
    src/CommandIds.cpp
    src/EqualizerPanel.cpp
    src/EqualizerPanel.hpp
    src/FileTree.cpp
    src/FileTree.hpp
    src/InfoPanel.cpp
    src/InfoPanel.hpp
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
    src/PlaylistColumns.cpp
    src/PlaylistColumns.hpp
    src/PlaylistDataModel.cpp
    src/PlaylistDataModel.hpp
    src/PreferencesDialog.cpp
    src/PreferencesDialog.hpp
    src/SeekBar.cpp
    src/SeekBar.hpp
    src/SingleInstance.cpp
    src/SingleInstance.hpp
    src/OscilloscopePanel.cpp
    src/SpectrumPanel.cpp
    src/SpectrumPanel.hpp
    src/SpeedPanel.cpp
    src/SpeedPanel.hpp
    src/StatusPresence.cpp
    src/StatusPresence.hpp
    src/Text.hpp
    src/WxMenus.cpp
    src/WxMenus.hpp
    src/XPCogApp.cpp
    src/XPCogApp.hpp
)

add_library(xpcog-appcore STATIC ${XPCOG_APP_SOURCES})

target_include_directories(xpcog-appcore PUBLIC src)

target_link_libraries(xpcog-appcore PUBLIC
    xpcog-uicore
    xpcog-codecs
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

# The translations used to be attached here and are now on xpcog-uicore, which
# is where the lookup that reads them lives -- and where both frontends can
# reach them. See uicore/CMakeLists.txt.

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

# The WinUI island prototype; see cmake/XPCogWinAppSdk.cmake.
if(XPCOG_WITH_WINUI_ISLAND)
    include(XPCogWinAppSdk)
    target_sources(xpcog-appcore PRIVATE src/WinUIIsland.cpp src/WinUIIsland.hpp)
    target_link_libraries(xpcog-appcore PUBLIC XPCog::winappsdk)
    target_compile_definitions(xpcog-appcore PUBLIC XPCOG_WITH_WINUI_ISLAND=1)
    # Beside the executable, where the loader looks for it. It is the only part
    # of the Windows App SDK that ships with the program.
    add_custom_command(TARGET xpcog-app POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${XPCOG_WINAPPSDK_BOOTSTRAP_DLL}" "$<TARGET_FILE_DIR:xpcog-app>")
endif()

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
# beside the *executable*, not with the assets.
#
# vcpkg_copy_tools() puts it under tools/<port>/, which is the same place
# tests/CMakeLists.txt finds mpcenc. Named rather than found on PATH: what has to
# run is the handler from the same sentry-native this linked against.
if(XPCOG_WITH_SENTRY)
    set(_handler
        "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/sentry-native/crashpad_handler${CMAKE_EXECUTABLE_SUFFIX}")

    if(EXISTS "${_handler}")
        add_custom_command(TARGET xpcog-app POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${_handler}" "$<TARGET_FILE_DIR:xpcog-app>"
            COMMENT "Staging crashpad_handler"
            VERBATIM)
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
# The copyright line. Windows reads LegalCopyright on the Details tab of the
# file's properties, which the version resource once had no field for.
#
# The wording follows app/src/AboutDialog.cpp, which is the other place a
# listener can read it, and it names Cog for the reason the About box does: this
# is a port, and most of what is under the interface is still theirs.
#
# CMake has no \uXXXX escape, so the sign and the dash below are the characters
# themselves. This file is UTF-8; the resource script has to
# be told, which is what the code page pragma in XPCog.rc.in is for.
# ---------------------------------------------------------------------------
set(XPCOG_COPYRIGHT "Copyright © 2026 the XPCog authors. Copyright © 2005–2026 Vincent Spader, Christopher Snowhill and the Cog authors.")

if(WIN32)
    # Identity, so the OS can say what this program is called. The version
    # resource is what Explorer's Details tab reads;
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
endif()

# Everywhere else the lookup is relative to the executable, so the staged copy
# beside it is the one that is found -- in the build tree and, on Windows, in
# the deployed folder, which is that same directory.
add_dependencies(xpcog-app xpcog-assets)

set_target_properties(xpcog-app PROPERTIES
    OUTPUT_NAME "XPCog"
    WIN32_EXECUTABLE ON
    FOLDER app)

# ---------------------------------------------------------------------------
# Deployment: there is none, and that is the point.
#
# The Qt build needed `cmake --build <dir> --target deploy` before the binary
# could leave the build tree: windeployqt copied the Qt runtime and its platform
# plugins beside XPCog.exe. Windows has no rpath, so a freshly built exe could
# not even start from Explorer without it.
#
# wxWidgets comes from vcpkg like everything else, and vcpkg's applocal post-build
# step already copies every dependent DLL beside the executable -- the same step
# that has been placing FFmpeg's and TagLib's all along. The installer harvests
# that directory whole; packaging/windows/Harvest.cmake says why that beats
# install() there.
# ---------------------------------------------------------------------------
