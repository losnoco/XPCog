# In-tree third-party sources.
#
# This directory is only for code that is Cog-modified, single-file, or has no
# usable upstream. Anything with a real upstream becomes a vcpkg overlay port in
# ../ports instead -- that is the pattern the ~32 vendored Cog libraries follow
# when they land in M6.
#
# Planned occupants:
#   lpc/      <- Cog Audio/ThirdParty/lvqcl        -- done. Predicts the samples
#             just outside a resampler run so its filter has real signal to
#             convolve at the edges rather than an implicit step to silence.
#             Not needed per block, which is what an earlier note here expected:
#             soxr keeps its delay line across calls, so chunk edges inside a
#             run are already continuous. The edges that are real are the first
#             block of a run and the last, which is to say every track start and
#             every track seam.
#   hdcd/     <- Cog Audio/ThirdParty/hdcd         (M1c) -- done
#   miniaudio/  vendored                           (M1a) -- done
#   pffft/    <- Cog Audio/ThirdParty/deadbeef     (M5)
#   dsd2pcm/  <- extracted from Cog ChunkList.m    (M6) -- done
#   psflib/   <- Cog Frameworks/psflib             (M6) -- the PSF container
#             every HighlyComplete core sits on. Four files; the emulator
#             cores themselves are far larger and land one at a time.
#   lazyusf2/ <- Cog Frameworks/lazyusf2            (M6) -- the first of those
#             cores: mupen64plus cut down to N64 audio, which is what a USF
#             plays. Seven more to go; see docs/HIGHLYCOMPLETE.md.
#   fsurround/<- Cog Audio/ThirdParty/fsurround    (M4) -- data only, done
#   hively/   <- Cog Frameworks/HivelyPlayer       (M6) -- done. The AHX and
#             HVL replayer, here rather than in ../ports because Cog's copy
#             carries a blip_buf resampler upstream has never had.
#   signalsmith/ <- Cog ThirdParty/signalsmith     -- done. Signalsmith Stretch
#             and the signalsmith-linear FFT under it, the pitch/tempo engine
#             beside Rubber Band. Header-only; vendored from Cog rather than
#             ported because the pair have to match each other and upstream's
#             tags trail what Cog ships against.
#   syntrax/  <- Cog Frameworks/Syntrax-c          (M6) -- done. libjaytrax,
#             here rather than in ../ports because upstream is an untagged
#             Bitbucket repository and Cog's copy has grown the memory loader
#             and the length measurement the decoder is built on.

add_subdirectory(dsd2pcm)
add_subdirectory(fsurround)
add_subdirectory(hdcd)
add_subdirectory(lpc)
if(XPCOG_WITH_HIVELY)
    add_subdirectory(hively)
endif()
add_subdirectory(minimp3)
if(XPCOG_WITH_MIDI)
    add_subdirectory(nuked-opl3)
    add_subdirectory(nuked-sc55)
endif()
add_subdirectory(miniaudio)
add_subdirectory(signalsmith)
if(XPCOG_WITH_SYNTRAX)
    add_subdirectory(syntrax)
endif()
if(XPCOG_WITH_PSF)
    add_subdirectory(psflib)
    add_subdirectory(lazyusf2)
    add_subdirectory(vio2sf)
    add_subdirectory(snes9x)
    add_subdirectory(highlytheoretical)
    add_subdirectory(sseqplayer)
    add_subdirectory(highlyexperimental)
    add_subdirectory(highlyquixotic)
endif()
