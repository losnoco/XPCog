# DSD to PCM, extracted from Cog Audio/Chain/ChunkList.m (Sebastian Gesemann).
# Part of core rather than a codec: the format arrives from more than one
# decoder -- WavPack carries DSD, and so do .dsf and .dff -- and what they all
# need is the same eight-to-one decimation on the way to the device.
add_library(vendor-dsd2pcm STATIC dsd2pcm.c)
target_include_directories(vendor-dsd2pcm PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(vendor-dsd2pcm PROPERTIES FOLDER vendor)
if(NOT MSVC)
    target_compile_options(vendor-dsd2pcm PRIVATE -w)
endif()
