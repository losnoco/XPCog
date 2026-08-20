# Two layering rules, both enforced rather than documented.
#
# Invoked in script mode (cmake -P) by the xpcog-no-toolkit target.
#
# **1. Nothing anywhere includes Qt.** Qt was removed in the wxWidgets port, and
# this is what stops it coming back by accident -- a file copied from an older
# branch, or a habit. If Qt is ever wanted again that is a decision, and a
# decision can edit this file.
#
# **2. core, codecs and platform's public headers link no UI toolkit at all.**
# This is the rule that actually matters, and it is the one the port was a test
# of. core stays embeddable and testable without a display; platform's *interface*
# stays free of any toolkit, which is what makes the application above it
# replaceable. platform's implementations are exempt -- they talk to Win32,
# C++/WinRT, CoreFoundation and GDBus, which is their whole job -- but nothing
# they do may leak into a header the application includes.
#
# The check that replaced this one was CheckNoQt.cmake, scoped to core alone. It
# had become tautological: with Qt gone from the tree it could only ever pass.

if(NOT DEFINED XPCOG_ROOT_DIR)
    message(FATAL_ERROR "CheckNoToolkit.cmake: XPCOG_ROOT_DIR not set")
endif()

get_filename_component(XPCOG_ROOT_DIR "${XPCOG_ROOT_DIR}" ABSOLUTE)

# vendor/ is third-party source that is not ours to police, and build trees hold
# generated copies of everything.
set(_excluded "/vendor/" "/build/" "/.vcpkg-cache/" "/vcpkg_installed/")

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
xpcog_gather(_everything core codecs platform app tools tests)
xpcog_scan(
    "Qt was removed from this project, but Qt includes were found:"
    "^[ \t]*#[ \t]*include[ \t]*[<\"]Q"
    "If Qt is coming back, that is a decision -- edit cmake/CheckNoToolkit.cmake."
    ${_everything})

# --- 2. No UI toolkit below the application --------------------------------
xpcog_gather(_below core codecs)
xpcog_scan(
    "xpcog-core and xpcog-codecs must link no UI toolkit, but toolkit includes were found:"
    "^[ \t]*#[ \t]*include[ \t]*[<\"]wx/"
    "Move the toolkit-dependent code to app/."
    ${_below})

# platform's *headers* only. Its implementations talk to the OS and may include
# whatever the OS needs; what must not happen is any of that reaching the
# application through an interface.
xpcog_gather(_platform_headers platform/include)
xpcog_scan(
    "platform's public headers must name no UI toolkit, but toolkit includes were found:"
    "^[ \t]*#[ \t]*include[ \t]*[<\"]wx/"
    "The interface is what makes app/ replaceable -- keep the toolkit inside the implementation."
    ${_platform_headers})
