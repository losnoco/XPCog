# Finding wxWidgets.
#
# Unlike the Qt this replaced, there is no XPCOG_WX_ROOT to set and no deploy tool
# to run afterwards, which is most of the point of the move. Where the toolkit
# comes from, though, is not the same on every platform, and deliberately so:
#
#   Windows, macOS  vcpkg, via the `gui` feature in vcpkg.json. Neither platform
#                   packages wx, so building it is the only option, and on both
#                   it is a self-contained build of wx over an SDK that is
#                   already installed.
#   Linux           the distribution. vcpkg's wxwidgets port depends on its gtk3
#                   port, so asking vcpkg for wx on Linux builds 57 packages from
#                   source -- wx, GTK and 55 more beneath them: glib, pango,
#                   cairo, harfbuzz, fontconfig, at-spi2, dbus, seven X11
#                   libraries -- none of which is anything a Linux machine is
#                   short of. 98 packages for that configuration against 41
#                   without. The `gui` feature is therefore absent from the Linux
#                   presets and `libwxgtk3.2-dev` supplies the toolkit.
#
# Two discovery modes follow from that, tried in this order:
#
#   CONFIG  what vcpkg's own `usage` file recommends, and what its wxwidgets port
#           installs. Gives namespaced targets (wx::core and friends).
#   MODULE  CMake's bundled FindwxWidgets, which reads `wx-config` -- the Linux
#           path, and equally the way to build against a hand-built wx anywhere.
#
# Either way the result is one target, XPCog::wx, so nothing downstream has to
# know which was used, and both ask for the same components.

if(NOT XPCOG_BUILD_APP)
    return()
endif()

if(TARGET XPCog::wx)
    return()
endif()

# Components in link order -- dependents before their dependencies, which the
# MODULE path passes through to the linker verbatim.
#
# `adv` is absent, and this is worth stating precisely because the obvious guess is
# wrong in a way that fails silently. wxadv did merge into core -- but during 3.1,
# not at 3.3: `wxTaskBarIcon` and `wxNotificationMessage`, the only two classes
# here that ever lived there, are declared WXDLLIMPEXP_CORE in 3.1.5 and in every
# release since, Ubuntu 24.04's 3.2.4 included. So no supported wx needs the
# component, which is also the floor below: 3.2 is the oldest wx that carries
# these classes in core.
#
# Naming it anyway would not have been caught by a build. `wx-config` drops a
# library it does not know from its answer and still exits 0, and FindwxWidgets
# does not set wxWidgets_<component>_FOUND on the wx-config path at all -- there is
# a FIXME in the module saying so -- so a mistake in this list produces a
# successful configure and an undefined symbol much later, if anything references
# it at all. See `net` below for how that plays out.
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
    # vcpkg's toolchain file prepends its own installed tree to
    # CMAKE_FIND_ROOT_PATH, and FindwxWidgets looks for wx-config with
    # ONLY_CMAKE_FIND_ROOT_PATH -- an option that overrides the
    # CMAKE_FIND_ROOT_PATH_MODE_* variables rather than deferring to them. Every
    # search path is therefore re-rooted inside vcpkg_installed, which is the one
    # directory a system wx-config cannot be in, and the find fails on a machine
    # where `wx-config --version` answers perfectly well from a shell.
    #
    # Adding the real root back for the duration restores the ordinary search.
    # Only that one find_program is affected -- the module's other searches are in
    # the default BOTH mode and always worked -- but the root is put back straight
    # after, so nothing else in the project sees a loosened one.
    set(_xpcog_wx_saved_find_root "${CMAKE_FIND_ROOT_PATH}")
    list(APPEND CMAKE_FIND_ROOT_PATH "/")

    # QUIET rather than REQUIRED so the failure can be explained properly below.
    find_package(wxWidgets 3.2 QUIET COMPONENTS ${_xpcog_wx_components})
    set(CMAKE_FIND_ROOT_PATH "${_xpcog_wx_saved_find_root}")

    if(NOT wxWidgets_FOUND)
        # Worth saying properly rather than letting REQUIRED print that a package
        # was not found, and worth saying two different things: reaching here means
        # a missing apt package on Linux and a missing vcpkg feature anywhere else,
        # which have nothing in common but the symptom.
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            message(FATAL_ERROR
                    "wxWidgets 3.2 or newer was not found.\n"
                    "  On Linux the toolkit comes from the distribution rather "
                    "than from vcpkg: install libwxgtk3.2-dev (Debian/Ubuntu), "
                    "wxGTK-devel (Fedora) or wxgtk3 (Arch).\n"
                    "  To build it through vcpkg instead -- 57 packages, GTK among "
                    "them -- add `gui` to VCPKG_MANIFEST_FEATURES.")
        else()
            message(FATAL_ERROR
                    "wxWidgets 3.2 or newer was not found.\n"
                    "  vcpkg supplies it on this platform, and cheaply: no GTK is "
                    "involved off Linux. Add `gui` to VCPKG_MANIFEST_FEATURES, or "
                    "configure with a preset that already has it -- windows-debug, "
                    "macos-debug and their release counterparts all do.\n"
                    "  A wx from anywhere else is picked up here instead when its "
                    "wx-config is on PATH.")
        endif()
    endif()

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

message(STATUS "XPCog: wxWidgets ${wxWidgets_VERSION} found in "
               "${XPCOG_WX_DISCOVERY} mode")
