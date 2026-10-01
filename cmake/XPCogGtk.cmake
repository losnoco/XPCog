# Finding GTK4 and libadwaita, for the GTK frontend in app-gtk/.
#
# From the system, never from vcpkg: vcpkg has a gtk port and a libadwaita
# port, and asking for them builds the whole GNOME stack beneath -- glib, pango, cairo, harfbuzz,
# graphene, gdk-pixbuf, at-spi2, the X11 and Wayland libraries -- on a machine
# that already has every one of them. pkg-config finds the distribution's copy,
# the same way platform/CMakeLists.txt takes gio-2.0.
#
# One target comes out, XPCog::gtk, so app-gtk/ links a name rather than a list.
#
# The floor is libadwaita 1.9 and GTK 4.22 -- what this was written against --
# and it is compiled in as well as checked here. GLib, GTK and libadwaita each
# honour a *_VERSION_MAX_ALLOWED macro: calling a function newer than it emits a
# deprecation-style warning, so a build on a machine with newer headers still
# says, at compile time, when the code reaches past the floor. Raising the floor
# is editing the four numbers below, and nothing else.

if(NOT XPCOG_BUILD_GTK_APP)
    return()
endif()

if(TARGET XPCog::gtk)
    return()
endif()

set(XPCOG_GTK_MIN_VERSION 4.22)
set(XPCOG_ADW_MIN_VERSION 1.9)
set(XPCOG_GLIB_MIN_VERSION 2.88)

find_package(PkgConfig REQUIRED)
pkg_check_modules(XPCOG_GTK4 IMPORTED_TARGET
    "gtk4>=${XPCOG_GTK_MIN_VERSION}"
    "libadwaita-1>=${XPCOG_ADW_MIN_VERSION}"
    "glib-2.0>=${XPCOG_GLIB_MIN_VERSION}"
    gio-2.0 gobject-2.0)

if(NOT XPCOG_GTK4_FOUND)
    message(FATAL_ERROR
            "GTK ${XPCOG_GTK_MIN_VERSION} and libadwaita ${XPCOG_ADW_MIN_VERSION} "
            "or newer were not found.\n"
            "  The GTK frontend takes them from the distribution: install "
            "libgtk-4-dev and libadwaita-1-dev (Debian/Ubuntu), gtk4-devel and "
            "libadwaita-devel (Fedora) or gtk4 and libadwaita (Arch).\n"
            "  A headless build of core, codecs and xpcog-cli needs neither: "
            "configure with -D XPCOG_BUILD_APP=OFF, or use the linux-headless preset.")
endif()

add_library(xpcog-gtk-deps INTERFACE)
target_link_libraries(xpcog-gtk-deps INTERFACE PkgConfig::XPCOG_GTK4)

# The floor as the compiler sees it. MIN_REQUIRED silences the deprecation
# warnings for anything deprecated *before* the floor, which this code is
# allowed to use; MAX_ALLOWED warns on anything introduced *after* it, which it
# is not. The numbers are spelled twice -- here and in the pkg-config request
# above -- because the macro names are versions, not variables.
target_compile_definitions(xpcog-gtk-deps INTERFACE
    GLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_88
    GLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_88
    GDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_22
    GDK_VERSION_MAX_ALLOWED=GDK_VERSION_4_22
    ADW_VERSION_MIN_REQUIRED=ADW_VERSION_1_9
    ADW_VERSION_MAX_ALLOWED=ADW_VERSION_1_9)

add_library(XPCog::gtk ALIAS xpcog-gtk-deps)

# gtk4 first in the request above, so this is GTK's version.
message(STATUS "XPCog: GTK ${XPCOG_GTK4_gtk4_VERSION}, "
               "libadwaita ${XPCOG_GTK4_libadwaita-1_VERSION} found")
