# SSEQPlayer: the NCSF core, and the odd one out among the eight.
#
# Every other core in HighlyComplete emulates a machine. This one does not: an
# NCSF holds an SDAT, the Nintendo DS's standard sound archive, and SSEQPlayer
# reads it and *performs* it -- parsing the SSEQ sequence, resolving the SBNK
# instrument bank and the SWAR/SWAV samples it names, and mixing the sixteen
# channels a DS has in software. Nothing here executes ARM code.
#
# That difference reaches the decoder: there is no save state, no ROM image and
# no CPU, so `codecs/ncsf` builds a player rather than a machine, and the
# emulator-shaped parts of the other four cores have no counterpart. The one
# thing it shares is the container, which is the point of `codecs/psf`.
#
# It also throws. SDAT parsing reports a malformed archive by throwing
# std::runtime_error, where the emulator cores return a status, so the decoder
# catches rather than checks. Cog does the same.
add_library(vendor-sseqplayer STATIC
    SSEQPlayer/Channel.cpp
    SSEQPlayer/FATSection.cpp
    SSEQPlayer/INFOEntry.cpp
    SSEQPlayer/INFOSection.cpp
    SSEQPlayer/NDSStdHeader.cpp
    SSEQPlayer/Player.cpp
    SSEQPlayer/SBNK.cpp
    SSEQPlayer/SDAT.cpp
    SSEQPlayer/SSEQ.cpp
    SSEQPlayer/SWAR.cpp
    SSEQPlayer/SWAV.cpp
    SSEQPlayer/SYMBSection.cpp
    SSEQPlayer/Track.cpp)

target_include_directories(vendor-sseqplayer SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(vendor-sseqplayer PUBLIC cxx_std_17)
set_target_properties(vendor-sseqplayer PROPERTIES FOLDER vendor)

if(MSVC)
    target_compile_definitions(vendor-sseqplayer PRIVATE _CRT_SECURE_NO_WARNINGS
                                                         _CRT_NONSTDC_NO_WARNINGS
                                                         _USE_MATH_DEFINES
                                                         NOMINMAX
                                                         WIN32_LEAN_AND_MEAN)
    target_compile_options(vendor-sseqplayer PRIVATE /w)
else()
    target_compile_options(vendor-sseqplayer PRIVATE -w)
endif()
