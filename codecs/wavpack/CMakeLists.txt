find_package(WavPack CONFIG REQUIRED GLOBAL)

xpcog_add_codec(
    NAME     wavpack
    REGISTER xpcog_register_wavpack
    SOURCES  WavPackDecoder.cpp
    DEPS     WavPack::WavPack
)
