# The layering rules, enforced rather than documented.
#
# Invoked in script mode (cmake -P) by the xpcog-no-toolkit target.
#
# **1. Nothing anywhere includes Qt.** Qt was removed in the wxWidgets port, and
# this is what stops it coming back by accident -- a file copied from an older
# branch, or a habit. If Qt is ever wanted again that is a decision, and a
# decision can edit this file.
#
# **2. core, codecs, uicore and platform's public headers link no UI toolkit at
# all.** This is the rule that actually matters, and it is the one the port was
# a test of. core stays embeddable and testable without a display; platform's
# *interface* stays free of any toolkit, which is what makes the application
# above it replaceable. platform's implementations are exempt -- they talk to
# Win32, C++/WinRT and GDBus, which is their whole job -- but
# nothing they do may leak into a header the application includes.
#
# **3. The two frontends do not include each other's toolkit.** app/ is the
# Windows player on wxWidgets and app-gtk/ the Linux one on GTK4 and
# libadwaita, and everything they share lives below both. A wx include under
# app-gtk/, or a GTK or GLib one under app/, is the sharing happening in the
# wrong place.
#
# The check that replaced this one was CheckNoQt.cmake, scoped to core alone. It
# had become tautological: with Qt gone from the tree it could only ever pass.

if(NOT DEFINED XPCOG_ROOT_DIR)
    message(FATAL_ERROR "CheckNoToolkit.cmake: XPCOG_ROOT_DIR not set")
endif()

get_filename_component(XPCOG_ROOT_DIR "${XPCOG_ROOT_DIR}" ABSOLUTE)

# vendor/ is third-party source that is not ours to police, and build trees hold
# generated copies of everything.
set(_excluded "/vendor/" "/build/" "/vcpkg_installed/")

function(xpcog_gather out)
    set(_found "")
    foreach(_dir IN LISTS ARGN)
        file(GLOB_RECURSE _batch
            "${XPCOG_ROOT_DIR}/${_dir}/*.c"
            "${XPCOG_ROOT_DIR}/${_dir}/*.cpp"
            "${XPCOG_ROOT_DIR}/${_dir}/*.h"
            "${XPCOG_ROOT_DIR}/${_dir}/*.hpp"
            "${XPCOG_ROOT_DIR}/${_dir}/*.mm")
        list(APPEND _found ${_batch})
    endforeach()

    set(_kept "")
    foreach(_file IN LISTS _found)
        set(_skip FALSE)
        foreach(_pattern IN LISTS _excluded)
            if(_file MATCHES "${_pattern}")
                set(_skip TRUE)
            endif()
        endforeach()
        if(NOT _skip)
            list(APPEND _kept "${_file}")
        endif()
    endforeach()
    set(${out} "${_kept}" PARENT_SCOPE)
endfunction()

function(xpcog_scan label regex advice)
    set(_offenders "")
    foreach(_file IN LISTS ARGN)
        file(STRINGS "${_file}" _hits REGEX "${regex}")
        if(_hits)
            file(RELATIVE_PATH _rel "${XPCOG_ROOT_DIR}" "${_file}")
            foreach(_hit IN LISTS _hits)
                string(STRIP "${_hit}" _hit)
                list(APPEND _offenders "  ${_rel}: ${_hit}")
            endforeach()
        endif()
    endforeach()

    if(_offenders)
        string(REPLACE ";" "\n" _message "${_offenders}")
        message(FATAL_ERROR "${label}\n${_message}\n${advice}")
    endif()
endfunction()

# --- 1. No Qt, anywhere ----------------------------------------------------
xpcog_gather(_everything core codecs platform uicore app app-gtk app-winui tools tests)
xpcog_scan(
    "Qt was removed from this project, but Qt includes were found:"
    # Q followed by a capital, which is what every Qt header is: QString,
    # QObject, QtGlobal, QtCore/QApplication. Matching a bare Q caught our own
    # files too -- Query.hpp in core/src/remote was reported as Qt coming back --
    # and a check that cannot tell a false positive from the thing it guards is
    # one people learn to work around.
    "^[ \t]*#[ \t]*include[ \t]*[<\"]Qt?[A-Z]"
    "If Qt is coming back, that is a decision -- edit cmake/CheckNoToolkit.cmake."
    ${_everything})

# --- 2. No UI toolkit below the applications -------------------------------
#
# uicore/ is in the list from the day it exists: it is the half of the
# application both frontends share, and it is only shareable while it names
# neither toolkit.
xpcog_gather(_below core codecs uicore)
xpcog_scan(
    "xpcog-core, xpcog-codecs and xpcog-uicore must link no UI toolkit, but wx includes were found:"
    "^[ \t]*#[ \t]*include[ \t]*[<\"]wx/"
    "Move the toolkit-dependent code to app/."
    ${_below})

# GLib and GIO are on this list as well as GTK, and that is deliberate. They are
# legitimate in exactly one place, platform/src/linux, which is not scanned;
# anywhere lower they would make core Linux-only in a way that compiles fine on
# Linux and is found on the first Windows build.
set(_gtk_regex "^[ \t]*#[ \t]*include[ \t]*[<\"](gtk/|gdk/|adwaita\\.h|graphene|cairo|gio/|glib)")
xpcog_scan(
    "xpcog-core, xpcog-codecs and xpcog-uicore must link no UI toolkit, but GTK or GLib includes were found:"
    "${_gtk_regex}"
    "Move the toolkit-dependent code to app-gtk/, or the GLib-dependent code to platform/src/linux."
    ${_below})

# platform's *headers* only. Its implementations talk to the OS and may include
# whatever the OS needs; what must not happen is any of that reaching the
# application through an interface.
xpcog_gather(_platform_headers platform/include)
xpcog_scan(
    "platform's public headers must name no UI toolkit, but wx includes were found:"
    "^[ \t]*#[ \t]*include[ \t]*[<\"]wx/"
    "The interface is what makes app/ replaceable -- keep the toolkit inside the implementation."
    ${_platform_headers})
xpcog_scan(
    "platform's public headers must name no UI toolkit, but GTK or GLib includes were found:"
    "${_gtk_regex}"
    "The interface is what makes the frontends replaceable -- keep GLib inside platform/src/linux."
    ${_platform_headers})

# --- 3. Each frontend on its own toolkit -----------------------------------
xpcog_gather(_winui_app app-winui)
xpcog_scan(
    "app-winui/ is the WinUI frontend, but wx includes were found:"
    "^[ \t]*#[ \t]*include[ \t]*[<\"]wx/"
    "It replaces app/ rather than borrowing from it: shared code belongs in uicore/ or platform/."
    ${_winui_app})
xpcog_scan(
    "app-winui/ is the WinUI frontend, but GTK or GLib includes were found:"
    "${_gtk_regex}"
    "Code the frontends share belongs in uicore/ or platform/."
    ${_winui_app})

xpcog_gather(_wx_app app)
xpcog_scan(
    "app/ is the wxWidgets frontend, but GTK or GLib includes were found:"
    "${_gtk_regex}"
    "Code both frontends need belongs in uicore/ or platform/, not in either app directory."
    ${_wx_app})

xpcog_gather(_gtk_app app-gtk)
xpcog_scan(
    "app-gtk/ is the GTK frontend, but wx includes were found:"
    "^[ \t]*#[ \t]*include[ \t]*[<\"]wx/"
    "Code both frontends need belongs in uicore/ or platform/, not in either app directory."
    ${_gtk_app})
