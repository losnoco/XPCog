# Blueprint -> GtkBuilder XML -> GResource, for the GTK frontend.
#
# The interface in app-gtk/ is written in Blueprint (https://gnome.pages.gitlab.gnome.org/blueprint-compiler/),
# a declarative language for GtkBuilder that reads as the widget tree it
# describes, and is compiled to the XML GtkBuilder actually loads. The XML goes
# into a GResource, which is GTK's own blob store: gtk_builder_new_from_resource()
# reads it, the icon theme reads a resource path, and nothing has to be found on
# disk at run time. cmake/XPCogResources.cmake -- the C-array embedder the wx
# app uses -- is the wrong tool here for that reason: GTK wants the GResource
# form, and glib-compile-resources produces exactly that.
#
# Three tools, all from the distribution:
#
#   blueprint-compiler    pure Python, reads the toolkit's .typelib files
#                         directly (no PyGObject), and checks every property and
#                         signal it compiles against them. That check is the
#                         build-time validation of the .blp files: a property
#                         libadwaita does not have fails here rather than at
#                         gtk_builder_new. gtk4-builder-tool cannot do this job
#                         -- it loads GTK's types alone and rejects every Adw*
#                         object -- and instantiating the XML for real needs a
#                         display, which is what tests/gtk does under Xvfb.
#   glib-compile-resources  from libglib2.0-dev-bin / glib2, already a
#                         dependency of platform/.
#   python3               blueprint-compiler's interpreter.
#
# The resource manifest is *generated* from the argument list rather than kept
# by hand, so the set of .ui files in the resource and the set of .blp files
# compiled cannot drift apart -- the failure otherwise is a page that compiles,
# is never bundled, and fails at run time with "resource not found".

# Compile BLUEPRINTS (paths relative to the current source directory) and bundle
# the resulting .ui files, plus any RESOURCES (also relative, served at the same
# relative path -- or `served/path=source/path` for a file that lives outside
# this directory), into one GResource under PREFIX, added as a source of TARGET.
#
#   xpcog_add_blueprints(
#       TARGET     xpcog-gtkcore
#       NAME       xpcog
#       PREFIX     /co/losno/XPCog
#       BLUEPRINTS ui/window.blp ui/menus.blp
#       RESOURCES  icons/xpcog-foo-symbolic.svg
#                  sc55/back.data=../vendor/nuked-sc55/data/back.data)
#
# ui/window.blp becomes /co/losno/XPCog/ui/window.ui; the SVG is served at
# /co/losno/XPCog/icons/xpcog-foo-symbolic.svg. NAME is the manifest's basename,
# the generated C file's, and the prefix of the two functions it declares in
# `<NAME>-resources.h`: `<NAME>_register_resource()` and `<NAME>_get_resource()`.
#
# The program has to call the first, and this is not the default. Left to
# itself glib-compile-resources registers the bundle from a constructor, and a
# constructor in a static library is exactly what cmake/XPCogCodec.cmake warns
# about for self-registering codecs: nothing references its object file, the
# linker drops it, and the resource is "not found" at run time with every build
# green. --manual-register generates a plain function instead, and a plain
# function is referenced or it is a link error.
# ICONS takes `<size>=<path>` pairs and serves each at
# icons/hicolor/<size>x<size>/apps/<ICON_NAME>.png under the prefix -- the
# layout gtk_icon_theme_add_resource_path() reads, so the application icon
# resolves by name in the build tree exactly as an installed one would.
function(xpcog_add_blueprints)
    cmake_parse_arguments(BP "" "TARGET;NAME;PREFIX;ICON_NAME" "BLUEPRINTS;RESOURCES;ICONS" ${ARGN})
    foreach(_required TARGET NAME PREFIX BLUEPRINTS)
        if(NOT BP_${_required})
            message(FATAL_ERROR "xpcog_add_blueprints: ${_required} is required")
        endif()
    endforeach()

    _xpcog_find_blueprint_tools()

    set(_src "${CMAKE_CURRENT_SOURCE_DIR}")
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/resources")
    file(MAKE_DIRECTORY "${_out}")

    # --- 1. Blueprint -> XML, one command for all of them ------------------
    #
    # batch-compile writes each output at the input's path relative to
    # input-dir, so ui/window.blp lands at ${_out}/ui/window.ui, which is also
    # its path inside the resource. One command rather than one per file
    # because the compiler's start-up -- loading two typelibs -- is most of its
    # run time, and it takes the list.
    set(_blp_sources "")
    set(_ui_outputs "")
    set(_manifest_entries "")
    foreach(_blp IN LISTS BP_BLUEPRINTS)
        if(NOT _blp MATCHES "\\.blp$")
            message(FATAL_ERROR "xpcog_add_blueprints: ${_blp} is not a .blp file")
        endif()
        string(REGEX REPLACE "\\.blp$" ".ui" _ui "${_blp}")
        list(APPEND _blp_sources "${_src}/${_blp}")
        list(APPEND _ui_outputs "${_out}/${_ui}")
        # xml-stripblanks: GtkBuilder does not need the whitespace and the
        # bundle is smaller without it; it is also what every GNOME app does.
        string(APPEND _manifest_entries
               "    <file preprocess=\"xml-stripblanks\">${_ui}</file>\n")
    endforeach()

    add_custom_command(
        OUTPUT ${_ui_outputs}
        COMMAND "${XPCOG_BLUEPRINT_COMPILER}" batch-compile
                "${_out}" "${_src}" ${_blp_sources}
        DEPENDS ${_blp_sources}
        COMMENT "Compiling Blueprint interface files for ${BP_TARGET}"
        VERBATIM)

    # --- 2. The manifest --------------------------------------------------
    set(_resource_sources "")
    foreach(_res IN LISTS BP_RESOURCES)
        if(_res MATCHES "^([^=]+)=(.+)$")
            set(_alias "${CMAKE_MATCH_1}")
            get_filename_component(_absolute "${CMAKE_MATCH_2}" ABSOLUTE BASE_DIR "${_src}")
            list(APPEND _resource_sources "${_absolute}")
            string(APPEND _manifest_entries "    <file alias=\"${_alias}\">${_absolute}</file>\n")
        else()
            list(APPEND _resource_sources "${_src}/${_res}")
            string(APPEND _manifest_entries "    <file>${_res}</file>\n")
        endif()
    endforeach()

    foreach(_icon IN LISTS BP_ICONS)
        string(REPLACE "=" ";" _pair "${_icon}")
        list(GET _pair 0 _size)
        list(GET _pair 1 _file)
        get_filename_component(_absolute "${_file}" ABSOLUTE BASE_DIR "${_src}")
        list(APPEND _resource_sources "${_absolute}")
        # An alias, because the source lives outside this directory and its
        # own path would be the resource's name otherwise.
        string(APPEND _manifest_entries
               "    <file alias=\"icons/hicolor/${_size}x${_size}/apps/${BP_ICON_NAME}.png\">${_absolute}</file>\n")
    endforeach()

    set(_manifest "${_out}/${BP_NAME}.gresource.xml")
    file(WRITE "${_manifest}.in"
         "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<gresources>\n"
         "  <gresource prefix=\"${BP_PREFIX}\">\n"
         "${_manifest_entries}"
         "  </gresource>\n"
         "</gresources>\n")
    # Through configure_file so an unchanged manifest keeps its timestamp and
    # does not rebuild the bundle on every configure.
    configure_file("${_manifest}.in" "${_manifest}" COPYONLY)

    # --- 3. The bundle, as a C source of the target -------------------------
    #
    # Two source directories: the compiled .ui files are in the build tree and
    # the extra resources are in the source tree, and the manifest names both
    # by the same relative paths.
    set(_c_name "${BP_NAME}-resources")
    set(_c_source "${_out}/${_c_name}.c")
    set(_c_header "${_out}/${_c_name}.h")
    add_custom_command(
        OUTPUT "${_c_source}"
        COMMAND "${XPCOG_GLIB_COMPILE_RESOURCES}"
                --generate-source
                --manual-register
                --target "${_c_source}"
                --c-name "${BP_NAME}"
                --sourcedir "${_out}"
                --sourcedir "${_src}"
                --sourcedir "/"
                "${_manifest}"
        DEPENDS "${_manifest}" ${_ui_outputs} ${_resource_sources}
        COMMENT "Bundling ${BP_NAME}.gresource for ${BP_TARGET}"
        VERBATIM)
    add_custom_command(
        OUTPUT "${_c_header}"
        COMMAND "${XPCOG_GLIB_COMPILE_RESOURCES}"
                --generate-header
                --manual-register
                --target "${_c_header}"
                --c-name "${BP_NAME}"
                --sourcedir "${_out}"
                --sourcedir "${_src}"
                --sourcedir "/"
                "${_manifest}"
        # The same inputs as the source: the tool checks every file the
        # manifest names exists, header or not.
        DEPENDS "${_manifest}" ${_ui_outputs} ${_resource_sources}
        COMMENT "Declaring ${BP_NAME}.gresource for ${BP_TARGET}"
        VERBATIM)

    target_sources(${BP_TARGET} PRIVATE "${_c_source}" "${_c_header}")
    # Generated code, not ours to warn about; XPCog::warnings would.
    set_source_files_properties("${_c_source}" PROPERTIES COMPILE_OPTIONS "-w")
    # So `#include "xpcog-resources.h"` resolves for the target and its tests.
    target_include_directories(${BP_TARGET} PUBLIC "${_out}")
endfunction()

# Locate the compilers once per configure and cache the answer.
#
# XPCOG_BLUEPRINT_COMPILER can be set to a checkout's blueprint-compiler.py for a
# machine whose packaged compiler is older than the .blp files need -- it runs
# from its source tree, with python3 and the Gtk-4.0 and Adw-1 typelibs as its
# whole requirement.
function(_xpcog_find_blueprint_tools)
    if(NOT XPCOG_BLUEPRINT_COMPILER)
        find_program(XPCOG_BLUEPRINT_COMPILER NAMES blueprint-compiler
                     DOC "The Blueprint compiler (blueprint-compiler)")
    endif()
    if(NOT XPCOG_BLUEPRINT_COMPILER)
        message(FATAL_ERROR
                "blueprint-compiler was not found, and the GTK frontend's "
                "interface is written in Blueprint.\n"
                "  Install blueprint-compiler (Debian/Ubuntu, Fedora and Arch "
                "all package it under that name), or set "
                "XPCOG_BLUEPRINT_COMPILER to a checkout's blueprint-compiler.py.")
    endif()

    # The floor is the syntax in app-gtk/ui: anything older than this rejects
    # something written there. Read from the program rather than assumed,
    # because Ubuntu 24.04 ships 0.12 and it is the first thing to be wrong.
    set(_min 0.16)
    execute_process(COMMAND "${XPCOG_BLUEPRINT_COMPILER}" --version
                    OUTPUT_VARIABLE _version
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(NOT _version)
        message(WARNING "XPCog: blueprint-compiler's version could not be read, "
                        "so the ${_min} minimum is unchecked.")
    elseif(_version VERSION_LESS _min)
        message(FATAL_ERROR
                "blueprint-compiler ${_version} is too old: ${_min} or newer is "
                "needed for the syntax in app-gtk/ui. Set XPCOG_BLUEPRINT_COMPILER "
                "to a newer checkout's blueprint-compiler.py.")
    endif()

    find_program(XPCOG_GLIB_COMPILE_RESOURCES NAMES glib-compile-resources
                 DOC "GLib's resource bundler (glib-compile-resources)")
    if(NOT XPCOG_GLIB_COMPILE_RESOURCES)
        message(FATAL_ERROR
                "glib-compile-resources was not found. It ships with GLib's "
                "development package (libglib2.0-dev-bin, glib2-devel, glib2), "
                "which platform/ already needs.")
    endif()

    get_property(_reported GLOBAL PROPERTY XPCOG_BLUEPRINT_REPORTED)
    if(NOT _reported)
        message(STATUS "XPCog: blueprint-compiler ${_version} at ${XPCOG_BLUEPRINT_COMPILER}")
        set_property(GLOBAL PROPERTY XPCOG_BLUEPRINT_REPORTED ON)
    endif()
endfunction()
