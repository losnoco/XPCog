# Fails the build if anything under xpcog-core includes Qt.
# Invoked in script mode (cmake -P) by the xpcog-core-no-qt target.

if(NOT DEFINED XPCOG_CORE_DIR)
    message(FATAL_ERROR "CheckNoQt.cmake: XPCOG_CORE_DIR not set")
endif()

# Tolerate a relative path so the check is runnable by hand, not only from the
# build where CMAKE_CURRENT_SOURCE_DIR is already absolute.
get_filename_component(XPCOG_CORE_DIR "${XPCOG_CORE_DIR}" ABSOLUTE)

file(GLOB_RECURSE _sources
    "${XPCOG_CORE_DIR}/include/*.hpp"
    "${XPCOG_CORE_DIR}/include/*.h"
    "${XPCOG_CORE_DIR}/src/*.cpp"
    "${XPCOG_CORE_DIR}/src/*.hpp"
    "${XPCOG_CORE_DIR}/src/*.h")

set(_offenders "")
foreach(_f IN LISTS _sources)
    file(STRINGS "${_f}" _hits REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]Q")
    if(_hits)
        file(RELATIVE_PATH _rel "${XPCOG_CORE_DIR}" "${_f}")
        foreach(_h IN LISTS _hits)
            string(STRIP "${_h}" _h)
            list(APPEND _offenders "  ${_rel}: ${_h}")
        endforeach()
    endif()
endforeach()

if(_offenders)
    string(REPLACE ";" "\n" _msg "${_offenders}")
    message(FATAL_ERROR
        "xpcog-core must not depend on Qt, but Qt includes were found:\n"
        "${_msg}\n"
        "Move the Qt-dependent code to platform/ or app/.")
endif()
