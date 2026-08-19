# snes9x, as the SNES core behind SNSF.
#
# Cog's copy, and it is not the whole emulator: eighteen sources, no video
# output, no frontend, no save-state UI. What is here is the 65816, the memory
# map, DMA, the S-DD1 decompressor, enough PPU to keep the CPU's timing honest,
# and bsnes's SPC700 and S-DSP for the audio. That is the entire job for a rip
# format -- an SNSF is the cartridge plus a save state, and the only output
# anyone wants is what the DSP produces.
#
# Vendored rather than an overlay port: this is a cut-down and modified snes9x
# whose includes have been rewritten to `<snes9x/...>`, which per
# ../../ports/README.md is the `vendor/` case.
#
# ---------------------------------------------------------------------------
# Why the CPU matters here as much as the DSP
# ---------------------------------------------------------------------------
# An SPC dump is a snapshot of the SPC700's 64 KB of RAM, and there are tracks
# it physically cannot hold. Tales of Phantasia's vocal theme is the famous one:
# a sung verse does not fit in 64 KB even in the DSP's own BRR encoding, so
# Wolf Team's driver streams sample chunks from cartridge ROM through the
# CPU-APU I/O ports while the music plays, swapping them under a driver that
# computes sixteen virtual voices and sounds the loudest eight.
#
# That is why SNSF exists as a format and why this build needs the 65816 rather
# than just the sound chip -- and it makes such a track the sharpest test of the
# core there is. If the CPU-APU handshake drifts, the stream starves and the
# vocal breaks up, while every other track in the set plays perfectly.
add_library(vendor-snes9x STATIC
    src/apu/apu.cpp
    src/apu/bapu/dsp/SPC_DSP.cpp
    src/apu/bapu/dsp/sdsp.cpp
    src/apu/bapu/smp/algorithms.cpp
    src/apu/bapu/smp/core.cpp
    src/apu/bapu/smp/iplrom.cpp
    src/apu/bapu/smp/memory.cpp
    src/apu/bapu/smp/smp.cpp
    src/apu/bapu/smp/timing.cpp
    src/cpu.cpp
    src/cpuexec.cpp
    src/cpuops.cpp
    src/dma.cpp
    src/getset.cpp
    src/globals.cpp
    src/memmap.cpp
    src/ppu.cpp
    src/sdd1.cpp)

# SYSTEM for consumers, so third-party headers are not read under XPCog's own
# warning flags -- the tree being warning-free is a statement about our code.
#
# `<snes9x/snes9x.h>` for this library and its consumers, matching the rewritten
# includes; `src/` privately for the handful of relative includes between the
# APU sources.
target_include_directories(vendor-snes9x SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# The header tree is flat here too, and for the same reason as vendor/vio2sf:
# Cog's framework copies every header into one `snes9x.framework/Headers/`, and
# the sources were rewritten to `#include <snes9x/snes.hpp>` for a file that
# lives under `apu/bapu/snes/`. `snes9x/` is also on the path privately, for the
# quote-includes the sources use among themselves.
target_include_directories(vendor-snes9x PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/snes9x")
target_compile_features(vendor-snes9x PUBLIC cxx_std_17)
set_target_properties(vendor-snes9x PROPERTIES FOLDER vendor)

if(MSVC)
    # NOMINMAX for the same reason vendor/vio2sf needs it: std::max meets
    # <windows.h>, and vendored targets do not link XPCog::warnings.
    target_compile_definitions(vendor-snes9x PRIVATE _CRT_SECURE_NO_WARNINGS
                                                     _CRT_NONSTDC_NO_WARNINGS
                                                     NOMINMAX
                                                     WIN32_LEAN_AND_MEAN)
    target_compile_options(vendor-snes9x PRIVATE /w)
else()
    target_compile_options(vendor-snes9x PRIVATE -w -fwrapv)
endif()
