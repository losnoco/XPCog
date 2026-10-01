# The GTK4/libadwaita application.
#
# Same two-target split as app/, and for the same reason: everything but main()
# lives in a library so the models, the adapters and the bundled resources can be
# tested without opening a window. The wx application has caught its sharpest
# bugs in exactly that code, and this one will too.
#
# What this links, and what it must not: xpcog-uicore -- the application layer
# both frontends share, and through it core and platform -- plus xpcog-codecs,
# GTK and libadwaita beside it, and never wx: cmake/CheckNoToolkit.cmake fails
# the build on a wx include under app-gtk/.

set(XPCOG_GTK_SOURCES
    src/Accelerators.cpp
    src/Accelerators.hpp
    src/Actions.cpp
    src/Actions.hpp
    src/Dialogs.cpp
    src/Dialogs.hpp
    src/FileTreePane.cpp
    src/FileTreePane.hpp
    src/Glib.hpp
    src/GtkApp.cpp
    src/GtkApp.hpp
    src/Localization.cpp
    src/Localization.hpp
    src/MainWindow.cpp
    src/MainWindow.hpp
    src/Menus.cpp
    src/Menus.hpp
    src/MiniWindow.cpp
    src/MiniWindow.hpp
    src/Panes.cpp
    src/Panes.hpp
    src/PlaylistModel.cpp
    src/PlaylistModel.hpp
    src/PlaylistPane.cpp
    src/PlaylistPane.hpp
    src/PreferencesDialog.cpp
    src/PreferencesDialog.hpp
    src/Presence.cpp
    src/Presence.hpp
    src/SeekBar.cpp
    src/SeekBar.hpp
    src/Painting.hpp
    src/Visualizers.cpp
    src/Visualizers.hpp
)

add_library(xpcog-gtkcore STATIC ${XPCOG_GTK_SOURCES})

target_include_directories(xpcog-gtkcore PUBLIC src)

target_link_libraries(xpcog-gtkcore PUBLIC
    xpcog-uicore
    xpcog-codecs
    XPCog::gtk
    PRIVATE XPCog::warnings)

set_target_properties(xpcog-gtkcore PROPERTIES FOLDER app-gtk)

# The interface, compiled from Blueprint into a GResource. See
# cmake/XPCogBlueprint.cmake for the pipeline and for why the wx app's resource
# embedder is not used here. The prefix is the desktop ID with the dots turned
# into a path, which is the GNOME convention and keeps the resource namespace
# in step with the D-Bus one.
# The SC-55's front panel, and the photograph of the hardware it is drawn onto.
# Conditional because both come from the MIDI build, as in app/: without
# XPCOG_WITH_MIDI there is no emulator to render a panel state.
set(_gtk_extra_resources "")
if(XPCOG_WITH_MIDI)
    target_compile_definitions(xpcog-gtkcore PUBLIC XPCOG_HAVE_SC55_PANEL=1)
    target_sources(xpcog-gtkcore PRIVATE src/Sc55View.cpp src/Sc55View.hpp)
    list(APPEND _gtk_extra_resources "sc55/back.data=../vendor/nuked-sc55/data/back.data")
endif()

xpcog_add_blueprints(
    TARGET     xpcog-gtkcore
    NAME       xpcog
    PREFIX     /co/losno/XPCog
    BLUEPRINTS
        ui/mini.blp
        ui/window.blp
    RESOURCES  ${_gtk_extra_resources}
    # The application icon, at the sizes app/icons has, laid out as an icon
    # theme so the window and the tray find it by the desktop ID in the build
    # tree the way an installed copy is found.
    ICON_NAME  "${XPCOG_DESKTOP_ID}"
    ICONS
        16=../app/icons/xpcog-16.png
        24=../app/icons/xpcog-24.png
        32=../app/icons/xpcog-32.png
        48=../app/icons/xpcog-48.png
        64=../app/icons/xpcog-64.png
        128=../app/icons/xpcog-128.png
        256=../app/icons/xpcog-256.png
)

add_executable(xpcog-gtk src/main.cpp)

target_link_libraries(xpcog-gtk PRIVATE xpcog-gtkcore XPCog::warnings)

# Beside the assets, like the other two executables: AssetPath.hpp resolves
# them relative to the binary.
add_dependencies(xpcog-gtk xpcog-assets)

# `XPCog`, the name the Linux player has always installed under: the .desktop
# file launches it, and a packaged install that upgrades from the wx build
# keeps working. The target stays `xpcog-gtk`, because that is what says which
# frontend it is in a build tree.
set_target_properties(xpcog-gtk PROPERTIES
    OUTPUT_NAME "XPCog"
    FOLDER app-gtk)

install(TARGETS xpcog-gtk RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
xpcog_install_linux_runtime(xpcog-gtk)

# crashpad's handler, beside the executable, which is where
# platform/src/common/SentryCrashReporter.cpp looks. sentry-native sets no
# default for its path, and the failure without it is the quiet kind:
# messages are still sent, and actual crashes simply never appear.
# vcpkg_copy_tools() puts it under tools/<port>/; named rather than found on
# PATH, because what has to run is the handler from the same sentry-native
# this linked against.
if(XPCOG_WITH_SENTRY)
    set(_handler
        "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/sentry-native/crashpad_handler")
    if(EXISTS "${_handler}")
        add_custom_command(TARGET xpcog-gtk POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${_handler}" "$<TARGET_FILE_DIR:xpcog-gtk>"
            COMMENT "Staging crashpad_handler"
            VERBATIM)
        install(PROGRAMS "${_handler}" DESTINATION "${CMAKE_INSTALL_BINDIR}")
    else()
        message(STATUS
            "XPCog: crashpad_handler not found at ${_handler} -- this build will "
            "report messages but not crashes.")
    endif()
endif()
