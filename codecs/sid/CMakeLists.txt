# SID: Commodore 64 tunes, on libsidplayfp.
#
# The library is an overlay port -- see ../../ports/libsidplayfp for why it is a
# port rather than vendored sources, and for what its hand-written CMakeLists
# has to stand in for. That port installs a header tree and an archive and no
# CMake config package, so the imported target is assembled here, the same way
# and for the same reason as libgme's next door.
#
# C64Roms.cpp is deliberately *not* part of the port. It is not libsidplayfp's
# code and libsidplayfp does not ship it: it is the three ROM images this
# decoder hands to setRoms(), and it belongs with the caller. See C64Roms.hpp.
find_path(SIDPLAYFP_INCLUDE_DIR NAMES sidplayfp/sidplayfp.h REQUIRED)
find_library(SIDPLAYFP_LIBRARY_RELEASE NAMES sidplayfp REQUIRED)
find_library(SIDPLAYFP_LIBRARY_DEBUG NAMES sidplayfp
             PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib"
             NO_DEFAULT_PATH)

if(NOT TARGET sidplayfp::sidplayfp)
    add_library(sidplayfp::sidplayfp UNKNOWN IMPORTED GLOBAL)
    set_target_properties(sidplayfp::sidplayfp PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${SIDPLAYFP_INCLUDE_DIR}"
        IMPORTED_LOCATION "${SIDPLAYFP_LIBRARY_RELEASE}")
    if(SIDPLAYFP_LIBRARY_DEBUG)
        set_target_properties(sidplayfp::sidplayfp PROPERTIES
            IMPORTED_LOCATION_DEBUG "${SIDPLAYFP_LIBRARY_DEBUG}")
    endif()
endif()

xpcog_add_codec(
    NAME     sid
    REGISTER xpcog_register_sid
    SOURCES  SidDecoder.cpp C64Roms.cpp
    DEPS     sidplayfp::sidplayfp xpcog-codec-common
)
