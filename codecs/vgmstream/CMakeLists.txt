# vgmstream. An overlay port pinned to r2117 -- see ../../ports/vgmstream for what
# it patches and which optional codecs it leaves off -- or the distribution's copy
# where there is one. That copy ships no pkg-config file and no config package,
# only /usr/include/vgmstream/ and libvgmstream.so, so the system branch is a
# header and a library found by name; cmake/XPCogSystemDeps.cmake probes for those
# before vcpkg is asked for the port, and holds what it finds to the API version
# libvgmstream.h states, since a distribution's vgmstream tracks upstream master.
xpcog_system_dep(vgmstream _vgmstream_from_system)
if(NOT _vgmstream_from_system)
    # GLOBAL, or the imported target stays inside this directory scope and the
    # xpcog-codecs aggregation cannot see it. See cmake/XPCogCodec.cmake. The
    # system branch creates vgmstream::vgmstream with the same reach.
    find_package(vgmstream CONFIG REQUIRED GLOBAL)
endif()

xpcog_add_codec(
    NAME     vgmstream
    REGISTER xpcog_register_vgmstream
    SOURCES  VgmStreamDecoder.cpp
    DEPS     vgmstream::vgmstream xpcog-codec-common
)
