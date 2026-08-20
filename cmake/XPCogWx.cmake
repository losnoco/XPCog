# Finding wxWidgets.
#
# Unlike the Qt this replaced, the toolkit is an ordinary vcpkg dependency: it is
# declared in vcpkg.json under the `gui` feature and the manifest install puts it
# where the toolchain file already looks. There is no XPCOG_WX_ROOT to set and no
# deploy tool to run afterwards, which is most of the point of the move.
#
# Two discovery modes are tried, in this order:
#
#   CONFIG  what vcpkg's own `usage` file recommends, and what its wxwidgets port
#           installs. Gives namespaced targets (wx::core and friends).
#   MODULE  CMake's bundled FindwxWidgets, for a wx that came from the system
#           package manager instead -- a Linux developer with libwxgtk3.2-dev
#           already installed should not have to build GTK through vcpkg to
#           compile this. vcpkg's wxwidgets port ships a cmake wrapper that makes
#           this path work through vcpkg too.
#
# Either way the result is one target, XPCog::wx, so nothing downstream has to
# know which was used.

if(NOT XPCOG_BUILD_APP)
    return()
endif()

if(TARGET XPCog::wx)
    return()
endif()

# Components in link order -- dependents before their dependencies, which the
# MODULE path passes through to the linker verbatim.
#
# `adv` is deliberately absent: wxWidgets 3.3 merged wxadv into core, and asking
# for it by name fails on exactly the version this project targets.
#
# `net` is here for wxIPC, not for anything that touches a network: wx puts the
# socket classes in that library, and the single-instance handover is their only
# user. Worth knowing how it was found missing, because the failure mode is
# instructive -- the application linked without it, since SingleInstance sits in a
# static library and the linker extracts only the members that resolve an
# undefined symbol. Nothing in the executable referenced it, so the object was
# silently dropped. The test that names it directly is what pulled it in and
# turned a runtime absence into a link error, which is the same hazard
# cmake/XPCogCodec.cmake documents for self-registering codecs.
set(_xpcog_wx_components aui html core net base)

find_package(wxWidgets CONFIG QUIET)

if(wxWidgets_FOUND AND TARGET wx::core)
    set(_xpcog_wx_libs "")
    foreach(_c IN LISTS _xpcog_wx_components)
        if(TARGET wx::${_c})
            list(APPEND _xpcog_wx_libs wx::${_c})
        endif()
    endforeach()
    add_library(xpcog-wx INTERFACE)
    target_link_libraries(xpcog-wx INTERFACE ${_xpcog_wx_libs})
    set(XPCOG_WX_DISCOVERY "CONFIG" CACHE INTERNAL "How wxWidgets was found")
else()
    find_package(wxWidgets REQUIRED COMPONENTS ${_xpcog_wx_components})
    add_library(xpcog-wx INTERFACE)
    # wxWidgets_USE_FILE is a script rather than a target: it edits directory
    # properties. Its definitions and include directories are lifted onto the
    # interface target here so consumers still get them by linking, and so that
    # including this module has no effect on the directory that includes it.
    target_include_directories(xpcog-wx SYSTEM INTERFACE ${wxWidgets_INCLUDE_DIRS})
    target_compile_definitions(xpcog-wx INTERFACE ${wxWidgets_DEFINITIONS})
    target_compile_options(xpcog-wx INTERFACE ${wxWidgets_CXX_FLAGS})
    target_link_libraries(xpcog-wx INTERFACE ${wxWidgets_LIBRARIES})
    set(XPCOG_WX_DISCOVERY "MODULE" CACHE INTERNAL "How wxWidgets was found")
endif()

add_library(XPCog::wx ALIAS xpcog-wx)

message(STATUS "XPCog: wxWidgets found in ${XPCOG_WX_DISCOVERY} mode")
