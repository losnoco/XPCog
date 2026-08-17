# HDCD decoder, from Cog Audio/ThirdParty/hdcd (Christopher Snowhill, BSD).
# Portable C with no Apple dependencies -- vendored unmodified.
add_library(vendor-hdcd STATIC hdcd_decode2.c)
target_include_directories(vendor-hdcd PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(vendor-hdcd PROPERTIES FOLDER vendor)
if(NOT MSVC)
    target_compile_options(vendor-hdcd PRIVATE -w)
endif()
