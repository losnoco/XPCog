# GLOBAL promotes the imported target out of this directory scope so the
# xpcog-codecs aggregation in ../CMakeLists.txt can link it.
#
# The system branch is the distribution's TagLib, when it is 2.0 or newer and
# XPCOG_USE_SYSTEM_LIBS is on; either way what comes out is TagLib::tag. See
# cmake/XPCogSystemDeps.cmake.
xpcog_system_dep(taglib _taglib_from_system)
if(NOT _taglib_from_system)
    find_package(taglib CONFIG REQUIRED GLOBAL)
endif()

xpcog_add_codec(
    NAME     taglib
    REGISTER xpcog_register_taglib
    SOURCES  TagLibReader.cpp
    DEPS     TagLib::tag
)
