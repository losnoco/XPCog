# GLOBAL, or the imported target stays inside this directory scope and the
# xpcog-codecs aggregation cannot see it. See cmake/XPCogCodec.cmake.
find_package(vgmstream CONFIG REQUIRED GLOBAL)

xpcog_add_codec(
    NAME     vgmstream
    REGISTER xpcog_register_vgmstream
    SOURCES  VgmStreamDecoder.cpp
    DEPS     vgmstream::vgmstream xpcog-codec-common
)
