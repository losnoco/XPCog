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

# The equaliser's preset library: Cog's own Cog.q1.json, shipped unchanged so a
# library curated on either player reads on the other. See
# core/include/xpcog/core/audio/EqualizerPresets.hpp for the format.
set(XPCOG_EQ_PRESETS
    "${CMAKE_CURRENT_SOURCE_DIR}/equalizer/Cog.q1.json")
set(XPCOG_EQ_PRESETS "${XPCOG_EQ_PRESETS}" PARENT_SCOPE)

# copy_if_different rather than copy: the banks are 12.8 MB, and a build that
# rewrote them every time would also re-sign and re-deploy them every time.
#
# One loop over (subdirectory, files) pairs rather than one per kind, because
# every asset lands the same way and the only thing that varies is which
# subdirectory of the output directory it lands in -- which is the part
# assetPath() has to agree with.
set(_copied)
foreach(_group IN ITEMS "soundfonts;XPCOG_SOUNDFONTS" "equalizer;XPCOG_EQ_PRESETS")
    list(GET _group 0 _subdir)
    list(GET _group 1 _list)
    set(_staged "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${_subdir}")
    foreach(_source IN LISTS ${_list})
        get_filename_component(_name "${_source}" NAME)
        add_custom_command(
            OUTPUT "${_staged}/${_name}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_staged}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${_source}" "${_staged}/${_name}"
            DEPENDS "${_source}"
            COMMENT "Staging ${_name}"
            VERBATIM)
        list(APPEND _copied "${_staged}/${_name}")
    endforeach()
endforeach()

add_custom_target(xpcog-assets ALL DEPENDS ${_copied})
set_target_properties(xpcog-assets PROPERTIES FOLDER assets)

# Installed layouts. Windows deploys by copying the whole output directory, so
# it needs nothing here beyond the staging above; macOS carries them inside the
# bundle, which app/CMakeLists.txt arranges because only it has the bundle.
if(UNIX AND NOT APPLE)
    install(FILES ${XPCOG_SOUNDFONTS} DESTINATION share/xpcog/soundfonts)
    install(FILES ${XPCOG_EQ_PRESETS} DESTINATION share/xpcog/equalizer)
endif()
