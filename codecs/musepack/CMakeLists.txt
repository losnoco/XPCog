# Musepack, on libmpcdec. An overlay port rather than a vendored copy; see
# ../../ports/libmpcdec/portfile.cmake for what it patches and why.
# libmpcdec ships no pkg-config file on any distribution, so the system branch is
# a header and a library found by name -- see cmake/XPCogSystemDeps.cmake, which
# probes for exactly those two before vcpkg is asked for the port.
xpcog_system_dep(musepack _musepack_from_system)
if(NOT _musepack_from_system)
    find_package(libmpcdec CONFIG REQUIRED GLOBAL)
endif()

xpcog_add_codec(
    NAME     musepack
    REGISTER xpcog_register_musepack
    SOURCES  MusepackDecoder.cpp
    DEPS     libmpcdec::mpcdec
)
