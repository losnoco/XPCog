# Compiling files into the binary, replacing Qt's resource system.
#
# qt_add_resources() is what the icons and the SC-55 panel background used
# before, and there is no equivalent in wxWidgets -- wx has XRC, which is a UI
# layout format, not a blob store. So this generates the obvious thing: one
# `const unsigned char[]` per file plus a lookup keyed on the path relative to
# BASE, so call sites keep asking for "lucide/play.svg" exactly as they asked
# for ":/icons/lucide/play.svg".
#
# Compiled in rather than loaded from disk for the same reason Qt's resources
# were: these files are part of the program, not configuration. An icon that can
# go missing is an icon that silently degrades, and the SC-55 background is as
# much a part of that emulator as its ROMs are not.
#
#   xpcog_embed_resources(
#       TARGET xpcog-appcore
#       NAME   icons                       # -> xpcog::resources::icons(path)
#       BASE   ${CMAKE_CURRENT_SOURCE_DIR}/icons
#       FILES  icons/xpcog-16.png ...)
#
# The generated pair lands in the target's binary directory, which is added to
# its include path, so the header is reachable as <NAME_resources.hpp>.

function(xpcog_embed_resources)
    cmake_parse_arguments(ARG "" "TARGET;NAME;BASE" "FILES" ${ARGN})

    foreach(_required TARGET NAME BASE FILES)
        if(NOT ARG_${_required})
            message(FATAL_ERROR "xpcog_embed_resources: ${_required} is required")
        endif()
    endforeach()

    get_filename_component(ARG_BASE "${ARG_BASE}" ABSOLUTE)

    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/resources")
    set(_cpp "${_dir}/${ARG_NAME}_resources.cpp")
    set(_hpp "${_dir}/${ARG_NAME}_resources.hpp")

    # Absolute, so the generator can read them from its own working directory and
    # so DEPENDS names the same files CMake will stat for rebuilds.
    set(_abs "")
    foreach(_f IN LISTS ARG_FILES)
        get_filename_component(_a "${_f}" ABSOLUTE)
        list(APPEND _abs "${_a}")
    endforeach()

    add_custom_command(
        OUTPUT "${_cpp}" "${_hpp}"
        COMMAND "${CMAKE_COMMAND}"
                -DXPCOG_RES_NAME=${ARG_NAME}
                -DXPCOG_RES_BASE=${ARG_BASE}
                "-DXPCOG_RES_FILES=${_abs}"
                -DXPCOG_RES_CPP=${_cpp}
                -DXPCOG_RES_HPP=${_hpp}
                -P "${CMAKE_SOURCE_DIR}/cmake/EmbedResources.cmake"
        DEPENDS ${_abs} "${CMAKE_SOURCE_DIR}/cmake/EmbedResources.cmake"
        COMMENT "Embedding ${ARG_NAME} resources"
        VERBATIM)

    target_sources(${ARG_TARGET} PRIVATE "${_cpp}" "${_hpp}")
    target_include_directories(${ARG_TARGET} PUBLIC "${_dir}")
endfunction()
