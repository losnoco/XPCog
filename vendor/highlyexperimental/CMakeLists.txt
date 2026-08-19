# HighlyExperimental: the PlayStation core behind PSF and PSF2.
#
# One core, two formats, as HighlyTheoretical is for the Sega pair. It emulates
# the PS2 IOP -- an R3000 with the SPU2 -- and drops it into PS1 compatibility
# mode for PSF, so `psx_get_state_size(1)` builds a PlayStation and `(2)` builds
# a PlayStation 2.
#
# ---------------------------------------------------------------------------
# The BIOS
# ---------------------------------------------------------------------------
# `Core/hebios.h` is 512 KB of Sony's PlayStation 2 BIOS (5.0 North American),
# stripped module by module to the sound-playing code plus PS1 compatibility.
# It is Sony's copyrighted code, it is required for both formats, and upstream
# says so plainly in Core/Readme.txt:
#
#     The unfortunate dirty secret here is that Sony BIOS is used. [...]
#     Making this stuff 100% legal (via IOP kernel and PS1 BIOS HLE) is on
#     the to-do list.
#
# It is vendored here because Cog vendors it and this port follows Cog, which is
# a decision recorded rather than defended. If the HLE work upstream contemplates
# ever lands, this file is what it replaces. The alternative, should it ever be
# wanted, is loading the image from a user data directory at runtime and having
# the decoder decline without it.
add_library(vendor-highlyexperimental STATIC
    Core/bios.c
    Core/iop.c
    Core/ioptimer.c
    Core/psx.c
    Core/r3000.c
    Core/spu.c
    Core/spucore.c
    Core/vfs.c)

target_include_directories(vendor-highlyexperimental SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(vendor-highlyexperimental PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Core"
    "${CMAKE_CURRENT_SOURCE_DIR}/HighlyExperimental")
set_target_properties(vendor-highlyexperimental PROPERTIES FOLDER vendor)

# Cog's defines, from its Xcode project.
target_compile_definitions(vendor-highlyexperimental PRIVATE
    EMU_COMPILE EMU_LITTLE_ENDIAN HAVE_STDINT_H)

if(MSVC)
    target_compile_definitions(vendor-highlyexperimental PRIVATE
        _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS NOMINMAX
        WIN32_LEAN_AND_MEAN)
    target_compile_options(vendor-highlyexperimental PRIVATE /w)
else()
    target_compile_options(vendor-highlyexperimental PRIVATE -w)
endif()
