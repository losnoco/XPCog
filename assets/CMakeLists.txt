# The files XPCog ships beside itself.
#
# One target rather than a rule per consumer: the app, the CLI and the tests all
# find these through xpcog::assetPath(), which looks relative to the running
# executable, and all three land in the same output directory. Copying once into
# that directory means the lookup used in the build tree is the same lookup used
# once installed -- see core/include/xpcog/core/AssetPath.hpp for why that is
# worth arranging rather than special-casing the build tree in the code.

set(XPCOG_SOUNDFONTS
    "${CMAKE_CURRENT_SOURCE_DIR}/soundfonts/GeneralUserXG-SFeTest.sf3"
    "${CMAKE_CURRENT_SOURCE_DIR}/soundfonts/GeneralUserGS-Drums.sf3"
    "${CMAKE_CURRENT_SOURCE_DIR}/soundfonts/tg300b.sflist.json")
set(XPCOG_SOUNDFONTS "${XPCOG_SOUNDFONTS}" PARENT_SCOPE)

set(_staged "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/soundfonts")

# copy_if_different rather than copy: this is 12.8 MB, and a build that rewrote
# it every time would also re-sign and re-deploy it every time.
set(_copied)
foreach(_source IN LISTS XPCOG_SOUNDFONTS)
    get_filename_component(_name "${_source}" NAME)
    add_custom_command(
        OUTPUT "${_staged}/${_name}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_staged}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_source}" "${_staged}/${_name}"
        DEPENDS "${_source}"
        COMMENT "Staging ${_name}"
        VERBATIM)
    list(APPEND _copied "${_staged}/${_name}")
endforeach()

add_custom_target(xpcog-assets ALL DEPENDS ${_copied})
set_target_properties(xpcog-assets PROPERTIES FOLDER assets)

# Installed layouts. Windows deploys by copying the whole output directory, so
# it needs nothing here beyond the staging above; macOS carries them inside the
# bundle, which app/CMakeLists.txt arranges because only it has the bundle.
if(UNIX AND NOT APPLE)
    install(FILES ${XPCOG_SOUNDFONTS} DESTINATION share/xpcog/soundfonts)
endif()
