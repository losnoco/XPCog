# libgme ships no CMake config package -- vcpkg installs the header and the
# import library and nothing else -- so the target is assembled here rather
# than imported. Everything else in this tree uses find_package(); this is the
# exception, and it is the port's shape rather than a choice.
find_path(GME_INCLUDE_DIR NAMES gme/gme.h REQUIRED)
find_library(GME_LIBRARY_RELEASE NAMES gme REQUIRED)
find_library(GME_LIBRARY_DEBUG NAMES gme
             PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib"
             NO_DEFAULT_PATH)

if(NOT TARGET GME::gme)
    add_library(GME::gme UNKNOWN IMPORTED GLOBAL)
    set_target_properties(GME::gme PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${GME_INCLUDE_DIR}"
        IMPORTED_LOCATION "${GME_LIBRARY_RELEASE}")
    if(GME_LIBRARY_DEBUG)
        set_target_properties(GME::gme PROPERTIES
            IMPORTED_LOCATION_DEBUG "${GME_LIBRARY_DEBUG}")
    endif()
endif()

xpcog_add_codec(
    NAME     gme
    REGISTER xpcog_register_gme
    SOURCES  GmeDecoder.cpp
    DEPS     GME::gme xpcog-codec-common
)
