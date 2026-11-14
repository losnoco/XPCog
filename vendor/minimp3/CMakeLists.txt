# minimp3 -- https://github.com/lieff/minimp3 (CC0 / public domain).
#
# The copies from Cog Plugins/minimp3/ThirdParty, which is the point: this is
# the decoder Cog ships, and its behaviour on the malformed MP3s that exist in
# the wild is what a Cog library has been listened to through. Vendored rather
# than taken from vcpkg because there is no vcpkg port, and because a
# header-only decoder with no upstream releases is exactly what vendor/ is for.
#
# ---------------------------------------------------------------------------
# Local changes to the vendored sources
# ---------------------------------------------------------------------------
# Listed here so a re-vendor does not silently drop them; each is marked in
# place with an "XPCog local change" comment.
#
#   minimp3.h  Two `static const f4 g_scale = { c, c, c, c }` became
#              `const f4 g_scale = VSET(c)`, in mp3d_synth and in
#              mp3dec_f32_to_s16. Both are the SIMD path with MINIMP3_FLOAT_OUTPUT
#              on, which is the configuration this target builds. Upstream has
#              only ever compiled the NEON side with gcc/clang, whose
#              float32x4_t takes element initializers; MSVC's is an opaque
#              `__n128` that takes none, and cl.exe calls the braces C2078 "too
#              many initializers". VSET is the header's own broadcast macro --
#              every SIMD flavour defines it, and the NEON blocks beside these
#              already use it -- and both constants are broadcasts of one value,
#              so the arithmetic is unchanged. `static` goes because an intrinsic
#              call is not a constant initializer. The SSE spellings are left
#              alone: cl.exe accepts them, and it is the only compiler here.

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
