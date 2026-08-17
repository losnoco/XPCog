# In-tree third-party sources.
#
# This directory is only for code that is Cog-modified, single-file, or has no
# usable upstream. Anything with a real upstream becomes a vcpkg overlay port in
# ../ports instead -- that is the pattern the ~32 vendored Cog libraries follow
# when they land in M6.
#
# Planned occupants:
#   lpc/      <- Cog Audio/ThirdParty/lvqcl        (M1c)
#   hdcd/     <- Cog Audio/ThirdParty/hdcd         (M1c)
#   miniaudio/                                     (M1a)
#   pffft/    <- Cog Audio/ThirdParty/deadbeef     (M5)
#   dsd2pcm/  <- extracted from Cog ChunkList.m    (M6)
#   fsurround/<- Cog Audio/ThirdParty/fsurround    (M4)

# (nothing yet)
