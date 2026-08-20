# libjaytrax, the Syntrax / JayTrax replayer, from Cog Frameworks/Syntrax-c.
#
# Syntrax was an Amiga-lineage tracker for DOS and Windows by Reinier van Vliet
# (Rhino / Bonzai), and .jxs is what it saved. The replayer is his, released as
# libjaytrax at bitbucket.org/rhinoid/jaytrax.
#
# Vendored rather than made an overlay port, and the rule in ../../ports/README.md
# is what decides it: upstream is a Mercurial repository on Bitbucket with no
# tagged release and no build system to package, and Cog's copy has been changed
# in ways the decoder depends on --
#
#   * jxsfile_readSongMem(), which loads a song from memory. Upstream reads a
#     path with fopen(), which is no use to a decoder handed an archive member
#     or an HTTP stream.
#   * jaytrax_getLength(), which runs the sequencer without mixing to find out
#     how long a subsong is. There is no other way to know: like every tracker
#     format the length is a property of where the order list jumps to.
#   * absPosition and loopStart on the player, which is what makes a chunk's
#     stream timestamp available.
#   * a sinc interpolator alongside upstream's nearest/linear/cubic.
#
# Take those away and there is no decoder. ioutil.h is a four-line error enum
# that jxs.c returns through; it is a header, so it is not listed as a source.
#
# Licence: every file here says GPL-3.0-only, and XPCog is GPL-2.0-or-later.
# Those combine -- "or later" is what makes them combine -- but the combined
# binary is GPL-3.0 rather than GPL-2.0 whenever XPCOG_WITH_SYNTRAX is on. Cog
# ships the same sources under the same project licence and inherits the same
# result; noted here because it is the sort of thing nobody notices until it
# matters.
add_library(vendor-syntrax STATIC
    jaytrax.c
    jxs.c
    mixcore.c
)
target_include_directories(vendor-syntrax PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(vendor-syntrax PROPERTIES FOLDER vendor)

# The replayer asserts on its own overlap bookkeeping in the render loop
# (jaytrax.c, "assert(availOvlap - SELF->overlapCnt == 0)"). Those hold, and
# leaving them armed in a debug build is the point of a debug build.

# Third-party warnings are not ours to fix, and reading like ours is the harm.
if(NOT MSVC)
    target_compile_options(vendor-syntrax PRIVATE -w)
else()
    target_compile_options(vendor-syntrax PRIVATE /w)
endif()
