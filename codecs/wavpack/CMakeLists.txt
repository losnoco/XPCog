xpcog_system_dep(wavpack _wavpack_from_system)
if(NOT _wavpack_from_system)
    find_package(WavPack CONFIG REQUIRED GLOBAL)
endif()

xpcog_add_codec(
    NAME     wavpack
    REGISTER xpcog_register_wavpack
    SOURCES  WavPackDecoder.cpp
    DEPS     WavPack::WavPack
)
