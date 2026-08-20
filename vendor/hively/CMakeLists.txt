# The Hively / AHX replayer, from Cog Frameworks/HivelyPlayer.
#
# Vendored rather than made an overlay port, and the rule in ../../ports/README.md
# is what decides it: this is a Cog-modified copy with no usable upstream to
# pin. Upstream is HivelyTracker, a GUI application rather than a released
# library, and Cog's copy has grown a blip_buf resampler and had every one of
# its symbols renamed `hvl_blip_*` -- which is what lets the replayer run at a
# device's own sample rate instead of the Amiga's. Take that away and the
# decoder's sample-rate handling goes with it.
#
# sinc_generator.c is deliberately NOT built. It is a standalone program with
# its own main(), which prints the `bl_step` table already baked into
# blip_buf.c line 241 -- a build-time tool, kept for provenance so the table is
# not a wall of unexplained integers. Adding it to the sources below links a
# second main() into whatever consumes this.
add_library(vendor-hively STATIC
    hvl_replay.c
    blip_buf.c
)
target_include_directories(vendor-hively PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(vendor-hively PROPERTIES FOLDER vendor)

# Third-party warnings are not ours to fix, and reading like ours is the harm.
if(NOT MSVC)
    target_compile_options(vendor-hively PRIVATE -w)
else()
    target_compile_options(vendor-hively PRIVATE /w)
endif()
