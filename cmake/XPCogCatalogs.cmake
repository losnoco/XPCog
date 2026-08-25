# Compiling app/locale/*.po into the binary.
#
# The counterpart of xpcog_embed_resources(), and it is separate for a reason
# beyond tidiness: a catalogue is not a blob. Embedding the .po bytes and parsing
# them at startup would put a text parser on the launch path and turn a
# mistyped escape into a run-time surprise, where this turns it into a compiler
# error on the generated source.
#
#   xpcog_add_catalogs(
#       TARGET xpcog-appcore
#       NAME   catalogs                     # -> xpcog::app::catalogs()
#       FILES  locale/es.po ...)
#
# The generated pair lands in the target's binary directory, which is added to
# its include path, so the header is reachable as <NAME.hpp>.
#
# Catalogues are compiled *in* rather than installed beside the executable, for
# the reason the icons are: a translation that can go missing is an application
# that silently falls back to English on one machine and not another, with
# nothing anywhere reporting a problem. It also means the Windows installer, the
# macOS bundle and a Linux package all carry them without three separate rules.

function(xpcog_add_catalogs)
    cmake_parse_arguments(ARG "" "TARGET;NAME" "FILES" ${ARGN})

    foreach(_required TARGET NAME FILES)
        if(NOT ARG_${_required})
            message(FATAL_ERROR "xpcog_add_catalogs: ${_required} is required")
        endif()
    endforeach()

    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/locale")
    set(_cpp "${_dir}/${ARG_NAME}.cpp")
    set(_hpp "${_dir}/${ARG_NAME}.hpp")

    set(_abs "")
    foreach(_f IN LISTS ARG_FILES)
        get_filename_component(_a "${_f}" ABSOLUTE)
        list(APPEND _abs "${_a}")
    endforeach()

    add_custom_command(
        OUTPUT "${_cpp}" "${_hpp}"
        COMMAND "${CMAKE_COMMAND}"
                -DXPCOG_PO_NAME=${ARG_NAME}
                "-DXPCOG_PO_FILES=${_abs}"
                -DXPCOG_PO_CPP=${_cpp}
                -DXPCOG_PO_HPP=${_hpp}
                -P "${CMAKE_SOURCE_DIR}/cmake/CompileCatalog.cmake"
        DEPENDS ${_abs} "${CMAKE_SOURCE_DIR}/cmake/CompileCatalog.cmake"
        COMMENT "Compiling ${ARG_NAME}"
        VERBATIM)

    target_sources(${ARG_TARGET} PRIVATE "${_cpp}" "${_hpp}")
    target_include_directories(${ARG_TARGET} PUBLIC "${_dir}")
endfunction()
