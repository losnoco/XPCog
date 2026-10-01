# The WinUI 3 application -- the Windows player's successor to app/.
#
# Same two-target split as app/ and app-gtk/, for the same reason: everything
# but main() lives in a library, so what can be tested without a window can be.
#
# What this links, and what it must not: xpcog-uicore -- the application layer
# every frontend shares, and through it core and platform -- plus xpcog-codecs
# and the Windows App SDK, and never wx: cmake/CheckNoToolkit.cmake fails the
# build on a wx include under app-winui/.
#
# There is no XAML compiler, and so no .xaml files and no MSBuild. The interface
# is built in code, with the odd template loaded from XAML text at run time --
# see cmake/XPCogWinAppSdk.cmake for how the SDK itself is brought in.

set(XPCOG_WINUI_SOURCES
    src/App.cpp
    src/App.hpp
    src/CommandMenus.cpp
    src/CommandMenus.hpp
    src/FileTreePane.cpp
    src/FileTreePane.hpp
    src/Instance.cpp
    src/Instance.hpp
    src/MainWindow.cpp
    src/MainWindow.hpp
    src/Oscilloscope.cpp
    src/Oscilloscope.hpp
    src/Panes.cpp
    src/Painting.hpp
    src/Panes.hpp
    src/PlaylistTable.cpp
    src/PlaylistTable.hpp
    src/PreferencesGeneral.cpp
    src/PreferencesRows.hpp
    src/PreferencesServices.cpp
    src/PreferencesSound.cpp
    src/PreferencesWindow.cpp
    src/PreferencesWindow.hpp
    src/Runtime.cpp
    src/Runtime.hpp
    src/SeekBar.cpp
    src/SeekBar.hpp
    src/Sizer.cpp
    src/Sizer.hpp
    src/Visualizers.cpp
    src/Visualizers.hpp
    src/Win2D.cpp
    src/Win2D.hpp
    src/WinRT.hpp
)

add_library(xpcog-winuicore STATIC ${XPCOG_WINUI_SOURCES})

target_include_directories(xpcog-winuicore PUBLIC src)

target_link_libraries(xpcog-winuicore PUBLIC
    xpcog-uicore
    xpcog-codecs
    XPCog::winappsdk
    PRIVATE XPCog::warnings
    # The cover art's memory stream (SHCreateMemStream, and the WinRT
    # stream over it) and the Music folder's path.
    Shlwapi.lib ShCore.lib Shell32.lib Ole32.lib)

set_target_properties(xpcog-winuicore PROPERTIES FOLDER app-winui)

# The SC-55's front panel, as app/ has it: only with the MIDI build, which is
# what produces a panel state to draw, and with the photograph of the hardware
# embedded for it to be drawn onto.
if(XPCOG_WITH_MIDI)
    target_compile_definitions(xpcog-winuicore PUBLIC XPCOG_HAVE_SC55_PANEL=1)
    target_sources(xpcog-winuicore PRIVATE src/Sc55View.cpp src/Sc55View.hpp)
    xpcog_embed_resources(
        TARGET xpcog-winuicore
        NAME   sc55
        BASE   "${CMAKE_SOURCE_DIR}/vendor/nuked-sc55/data"
        FILES  "${CMAKE_SOURCE_DIR}/vendor/nuked-sc55/data/back.data")
endif()

add_executable(xpcog-winui src/main.cpp)

target_link_libraries(xpcog-winui PRIVATE xpcog-winuicore XPCog::warnings)

# Identity, icon and manifest: the resource script is the wx player's minus wx's
# own resources, which is where that one gets its manifest from. See XPCog.rc.in.
# XPCOG_COPYRIGHT is app/CMakeLists.txt's, repeated because a directory's
# variables do not reach a sibling; it goes when app/ does.
set(XPCOG_COPYRIGHT "Copyright © 2026 the XPCog authors. Copyright © 2005–2026 Vincent Spader, Christopher Snowhill and the Cog authors.")
set(XPCOG_RC_VERSION
    "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/app/icons/xpcog.ico" XPCOG_RC_ICON)
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/XPCog.manifest" XPCOG_RC_MANIFEST)
configure_file(XPCog.rc.in "${CMAKE_CURRENT_BINARY_DIR}/XPCog.rc" @ONLY)
target_sources(xpcog-winui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/XPCog.rc")
set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/XPCog.rc"
    APPEND PROPERTY OBJECT_DEPENDS "${XPCOG_RC_ICON};${XPCOG_RC_MANIFEST}")
target_link_options(xpcog-winui PRIVATE "/MANIFEST:NO")

# The bootstrapper beside the executable, where the loader looks for it, and
# the window icon, which AppWindow takes as a file. Both land in the same bin/
# as XPCog.exe; the assets beside it are shared with the wx player.
add_custom_command(TARGET xpcog-winui POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${XPCOG_WINAPPSDK_BOOTSTRAP_DLL}" "$<TARGET_FILE_DIR:xpcog-winui>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${XPCOG_RC_ICON}" "$<TARGET_FILE_DIR:xpcog-winui>/xpcog.ico"
    # Win2D, which the painted panes draw with; see src/Win2D.cpp for how its
    # classes are found without being registered.
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${XPCOG_WIN2D_DLL}" "$<TARGET_FILE_DIR:xpcog-winui>")
add_dependencies(xpcog-winui xpcog-assets)
xpcog_stage_crashpad(xpcog-winui)

# XPCog-WinUI.exe rather than XPCog.exe, so the two players can share a bin/
# while both exist.
set_target_properties(xpcog-winui PROPERTIES
    OUTPUT_NAME "XPCog-WinUI"
    WIN32_EXECUTABLE ON
    FOLDER app-winui)
