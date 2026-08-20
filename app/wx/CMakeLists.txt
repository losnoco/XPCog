# The wxWidgets application.
#
# Built alongside the Qt one, not instead of it, until the port is finished. That
# is deliberate: a 7,500-line interface that has never once compiled produces a
# wall of errors with no bisect point in it, and having the old window running
# beside the new one is the only way to compare behaviour rather than guess at it.
# The Qt target and everything under app/src/ go in one piece at the end.
#
# Two targets for the same reason app/ has two: everything but main() lives in a
# library so the models and helpers can be tested without opening a window.

add_library(xpcog-wxcore STATIC
    AppIcon.cpp
    AppIcon.hpp
    Commands.cpp
    Commands.hpp
    FileTree.cpp
    FileTree.hpp
    LucideIcon.cpp
    LucideIcon.hpp
    MainFrame.cpp
    MainFrame.hpp
    PlaybackController.cpp
    PlaybackController.hpp
    PlaylistDataModel.cpp
    PlaylistDataModel.hpp
    SeekBar.cpp
    SeekBar.hpp
    Text.hpp
    XPCogApp.cpp
    XPCogApp.hpp
)

target_include_directories(xpcog-wxcore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(xpcog-wxcore PUBLIC
    xpcog-core
    xpcog-codecs
    xpcog-platform
    XPCog::wx
    PRIVATE XPCog::warnings)

set_target_properties(xpcog-wxcore PROPERTIES FOLDER app)

# ---------------------------------------------------------------------------
# Resources.
#
# What qt_add_resources() used to do, minus the toolkit. Attached to the library
# rather than to the executable because the code that reads them is here and the
# tests link the library -- without that, a test could only check that the sizes
# list is well-formed, never that the files behind it exist. That is the failure
# which actually happens: a renamed PNG degrades the icon silently, because a
# request for 16px is answered with a scaled 256px and nothing reports an error.
# ---------------------------------------------------------------------------
xpcog_embed_resources(
    TARGET xpcog-wxcore
    NAME   icons
    BASE   "${CMAKE_CURRENT_SOURCE_DIR}/../icons"
    FILES
        ../icons/xpcog-16.png
        ../icons/xpcog-24.png
        ../icons/xpcog-32.png
        ../icons/xpcog-48.png
        ../icons/xpcog-64.png
        ../icons/xpcog-128.png
        ../icons/xpcog-256.png
        # Lucide (https://lucide.dev), ISC -- see ../icons/lucide/LICENSE. Kept as
        # SVG source rather than baked to PNGs because the stroke colour is not
        # knowable until the system appearance is.
        ../icons/lucide/play.svg
        ../icons/lucide/pause.svg
        ../icons/lucide/square.svg
        ../icons/lucide/skip-back.svg
        ../icons/lucide/skip-forward.svg
        ../icons/lucide/audio-lines.svg
        ../icons/lucide/sliders-vertical.svg
        ../icons/lucide/info.svg
        ../icons/lucide/panel-left.svg
        ../icons/lucide/folder-open.svg
        ../icons/lucide/x.svg
)

# The SC-55's front panel, and the photograph of the hardware it is drawn onto.
# Conditional because both come from the MIDI build: without XPCOG_WITH_MIDI there
# is no emulator to render a panel state and nothing that produces one.
if(XPCOG_WITH_MIDI)
    target_compile_definitions(xpcog-wxcore PUBLIC XPCOG_HAVE_SC55_PANEL=1)
    # 776 KiB of raw RGBA, compiled in rather than found at runtime: it is part of
    # the emulator the way its ROMs are not -- shippable, and useless on its own.
    xpcog_embed_resources(
        TARGET xpcog-wxcore
        NAME   sc55
        BASE   "${CMAKE_SOURCE_DIR}/vendor/nuked-sc55/data"
        FILES  "${CMAKE_SOURCE_DIR}/vendor/nuked-sc55/data/back.data")
endif()

# ---------------------------------------------------------------------------
# The executable.
# ---------------------------------------------------------------------------
add_executable(xpcog-app-wx main.cpp)

target_link_libraries(xpcog-app-wx PRIVATE xpcog-wxcore XPCog::warnings)

set_target_properties(xpcog-app-wx PROPERTIES
    OUTPUT_NAME "XPCogWx"
    WIN32_EXECUTABLE ON
    FOLDER app)

# Identity, so the OS can say what this program is called. Shares the Qt
# executable's version resource template; the icon and the version block are
# toolkit-independent, which is most of app/CMakeLists.txt's Windows and macOS
# blocks and why none of them had to move.
if(WIN32)
    set(XPCOG_RC_VERSION
        "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../icons/xpcog.ico" XPCOG_RC_ICON)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/../XPCog.rc.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/XPCogWx.rc" @ONLY)
    target_sources(xpcog-app-wx PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/XPCogWx.rc")
    set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/XPCogWx.rc"
        APPEND PROPERTY OBJECT_DEPENDS "${XPCOG_RC_ICON}")
endif()

# The banks and anything else staged beside the binary.
add_dependencies(xpcog-app-wx xpcog-assets)
