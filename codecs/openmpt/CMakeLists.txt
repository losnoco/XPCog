find_package(libopenmpt CONFIG REQUIRED GLOBAL)

xpcog_add_codec(
    NAME     openmpt
    REGISTER xpcog_register_openmpt
    SOURCES  OpenMptDecoder.cpp
    DEPS     libopenmpt::libopenmpt xpcog-codec-common
)
