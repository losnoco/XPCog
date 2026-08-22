xpcog_system_dep(opus _opus_from_system)
if(NOT _opus_from_system)
    find_package(OpusFile CONFIG REQUIRED GLOBAL)
endif()

xpcog_add_codec(
    NAME     opus
    REGISTER xpcog_register_opus
    SOURCES  OpusDecoder.cpp
    DEPS     OpusFile::opusfile xpcog-codec-common
)
