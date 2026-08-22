# libgme ships no CMake config package -- vcpkg installs the header and the
# library and nothing else -- so the target is assembled here rather than
# imported. Everything else in this tree uses find_package(); this is the
# exception, and it is the port's shape rather than a choice.
#
# Which is not free. A config package would carry libgme's own dependencies;
# assembling the target by hand means naming them, and getting that wrong is
# invisible on a platform where the library happens to be shared. zlib is one:
# GME reads VGZ, which is gzipped VGM. On Windows vcpkg builds a DLL that
# resolves zlib internally and the omission never shows; on Linux and macOS it
# builds libgme.a, a static library carries nothing, and the link fails on
# gztell/gzseek/gzclose. Found by CI rather than here, which is the point of it.
# The system branch: libgme.pc, when XPCOG_USE_SYSTEM_LIBS found one new enough.
# It answers the same question the searches below do and answers it about the
# copy that is installed rather than about the port's layout.
xpcog_system_dep(gme _gme_from_system)
if(NOT _gme_from_system)
    find_package(ZLIB REQUIRED)
    find_path(GME_INCLUDE_DIR NAMES gme/gme.h REQUIRED)
    find_library(GME_LIBRARY_RELEASE NAMES gme REQUIRED)
    find_library(GME_LIBRARY_DEBUG NAMES gme
                 PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib"
                 NO_DEFAULT_PATH)

    if(NOT TARGET GME::gme)
        add_library(GME::gme UNKNOWN IMPORTED GLOBAL)
        set_target_properties(GME::gme PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${GME_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES ZLIB::ZLIB
            IMPORTED_LOCATION "${GME_LIBRARY_RELEASE}")
        if(GME_LIBRARY_DEBUG)
            set_target_properties(GME::gme PROPERTIES
                IMPORTED_LOCATION_DEBUG "${GME_LIBRARY_DEBUG}")
        endif()
    endif()
endif()

xpcog_add_codec(
    NAME     gme
    REGISTER xpcog_register_gme
    SOURCES  GmeDecoder.cpp
    DEPS     GME::gme xpcog-codec-common
)
