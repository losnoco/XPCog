# LPC extrapolation, from Cog Audio/ThirdParty/lvqcl (lvqcl, ISC-style).
# Predicts samples just outside a block so a resampler has real data to work
# from at the edges instead of an implicit step to silence.
add_library(vendor-lpc STATIC lpc.c)
target_include_directories(vendor-lpc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(vendor-lpc PROPERTIES FOLDER vendor)
if(NOT MSVC)
    target_compile_options(vendor-lpc PRIVATE -w)
endif()
