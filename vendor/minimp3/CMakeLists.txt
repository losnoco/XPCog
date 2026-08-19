# minimp3 -- https://github.com/lieff/minimp3 (CC0 / public domain).
#
# The copies from Cog Plugins/minimp3/ThirdParty, which is the point: this is
# the decoder Cog ships, and its behaviour on the malformed MP3s that exist in
# the wild is what a Cog library has been listened to through. Vendored rather
# than taken from vcpkg because there is no vcpkg port, and because a
# header-only decoder with no upstream releases is exactly what vendor/ is for.
add_library(vendor-minimp3 STATIC minimp3.c)

target_include_directories(vendor-minimp3 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# PUBLIC, and load-bearing. MINIMP3_FLOAT_OUTPUT redefines mp3d_sample_t from
# int16_t to float, so a consumer that did not see it would size every sample
# buffer at half what the implementation writes -- and would link without
# complaint. MINIMP3_NO_STDIO drops the file-opening helpers, which would
# otherwise pull in fopen and mmap for an API nothing here calls: XPCog reads
# through ISource, never from a path.
target_compile_definitions(vendor-minimp3 PUBLIC
    MINIMP3_FLOAT_OUTPUT
    MINIMP3_NO_STDIO)

set_target_properties(vendor-minimp3 PROPERTIES FOLDER vendor)

# Vendored code is not held to the project's warning settings.
if(NOT MSVC)
    target_compile_options(vendor-minimp3 PRIVATE -w)
endif()
