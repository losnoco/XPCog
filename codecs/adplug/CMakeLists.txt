# adplug ships no CMake config package -- the overlay port installs the archive
# and the headers and nothing else -- so the target is assembled here, the same
# way codecs/gme assembles libgme's.
find_path(ADPLUG_INCLUDE_DIR NAMES adplug/adplug.h REQUIRED)
find_library(ADPLUG_LIBRARY_RELEASE NAMES adplug REQUIRED)
find_library(ADPLUG_LIBRARY_DEBUG NAMES adplug
             PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib"
             NO_DEFAULT_PATH)

# AdPlug's own headers include binio.h unqualified, so libbinio has to be on the
# include path of anything that includes them -- which is this codec.
find_path(BINIO_INCLUDE_DIR NAMES binio.h REQUIRED)
find_library(BINIO_LIBRARY_RELEASE NAMES binio REQUIRED)
find_library(BINIO_LIBRARY_DEBUG NAMES binio
             PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib"
             NO_DEFAULT_PATH)

if(NOT TARGET AdPlug::binio)
    add_library(AdPlug::binio UNKNOWN IMPORTED GLOBAL)
    set_target_properties(AdPlug::binio PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${BINIO_INCLUDE_DIR}"
        IMPORTED_LOCATION "${BINIO_LIBRARY_RELEASE}")
    if(BINIO_LIBRARY_DEBUG)
        set_target_properties(AdPlug::binio PROPERTIES
            IMPORTED_LOCATION_DEBUG "${BINIO_LIBRARY_DEBUG}")
    endif()
endif()

if(NOT TARGET AdPlug::adplug)
    add_library(AdPlug::adplug UNKNOWN IMPORTED GLOBAL)
    set_target_properties(AdPlug::adplug PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${ADPLUG_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES AdPlug::binio
        IMPORTED_LOCATION "${ADPLUG_LIBRARY_RELEASE}")
    if(ADPLUG_LIBRARY_DEBUG)
        set_target_properties(AdPlug::adplug PROPERTIES
            IMPORTED_LOCATION_DEBUG "${ADPLUG_LIBRARY_DEBUG}")
    endif()
endif()

# The song database, compiled in rather than installed beside the binary.
#
# It is 7.8 KB and it is the only thing standing between forty formats that
# carry no metadata and a playlist of filenames -- see AdPlugDatabase.cpp. Turned
# into a byte array here with file(READ ... HEX), which needs no tool beyond
# CMake and keeps `adplug.db` in the tree as the upstream artefact rather than as
# a wall of hex in a committed header.
set(ADPLUG_DB "${CMAKE_CURRENT_SOURCE_DIR}/adplug.db")
set(ADPLUG_DB_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/AdPlugDatabaseBytes.cpp")

file(READ "${ADPLUG_DB}" ADPLUG_DB_HEX HEX)
string(LENGTH "${ADPLUG_DB_HEX}" ADPLUG_DB_HEX_LENGTH)
math(EXPR ADPLUG_DB_LENGTH "${ADPLUG_DB_HEX_LENGTH} / 2")
# Two hex digits to one `0x..,`, then a newline every sixteen bytes so the
# generated file is something a person can open without regretting it.
string(REGEX REPLACE "(..)" "0x\\1," ADPLUG_DB_BYTES "${ADPLUG_DB_HEX}")
string(REGEX REPLACE "((0x..,){16})" "\\1\n" ADPLUG_DB_BYTES "${ADPLUG_DB_BYTES}")

# `extern` on both definitions, and it is load-bearing rather than tidy: a
# namespace-scope `const` has *internal* linkage in C++, and extern "C" changes
# the mangling without changing that -- so without it the array is a file-local
# constant nothing else can see, and the compiler says so with an unused-variable
# warning while the link fails.
file(WRITE "${ADPLUG_DB_SOURCE}"
"// Generated from adplug.db by codecs/adplug/CMakeLists.txt. Do not edit.\n"
"extern \"C\" {\n"
"extern const unsigned char kAdPlugDatabaseBytes[];\n"
"extern const unsigned int kAdPlugDatabaseSize;\n"
"extern const unsigned char kAdPlugDatabaseBytes[] = {\n${ADPLUG_DB_BYTES}\n};\n"
"extern const unsigned int kAdPlugDatabaseSize = ${ADPLUG_DB_LENGTH}u;\n"
"}\n")

# So a replaced database is picked up rather than silently kept.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ADPLUG_DB}")

xpcog_add_codec(
    NAME     adplug
    REGISTER xpcog_register_adplug
    SOURCES  AdPlugDecoder.cpp AdPlugDatabase.cpp "${ADPLUG_DB_SOURCE}"
    DEPS     AdPlug::adplug xpcog-codec-common
)
