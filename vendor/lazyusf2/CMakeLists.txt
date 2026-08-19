# lazyusf2, from Cog Frameworks/lazyusf2 (kode54's fork of mupen64plus, cut down
# to "boot a Project64 save state and render the audio it was making").
#
# Vendored rather than an overlay port: upstream ships an Xcode project and
# nothing else, so the build below is hand-written either way, and Cog's copy is
# the one HighlyComplete is written against.
#
# ---------------------------------------------------------------------------
# Why no recompiler
# ---------------------------------------------------------------------------
# mupen64plus carries three MIPS execution cores -- pure interpreter, cached
# interpreter, and a dynamic recompiler that emits x86 machine code. Only the
# recompiler is architecture-bound, and it is reached only when `DYNAREC` is
# defined. Cog defines it for exactly one architecture:
#
#     "GCC_PREPROCESSOR_DEFINITIONS[arch=i386]" = ( "$(inherit)", DYNAREC );
#
# So every architecture Cog actually ships -- x86_64 and arm64 -- has been
# running the interpreter for years. That is the configuration this build takes,
# which is why `r4300/x86/` and `r4300/x86_64/` compile no sources here and
# `r4300/empty_dynarec.c` does instead: it is upstream's file of do-nothing
# stubs for the code generators, for precisely this case. Cog compiles the real
# generators and never calls them.
#
# Their *headers* are still needed. `r4300/recomp.h` picks one of the two
# `assemble_struct.h` files unconditionally -- `__x86_64__` or else the 32-bit
# one -- to size fields in `precomp_block`, so the directories stay even though
# no `.c` in them is built.
#
# Emulation is deterministic and every bit of state lives in the `usf_state_t`
# the caller allocates, so nothing here is a singleton and two tracks can decode
# at once.

# zlib, and not transitively. `r4300/interpreter_tlb.def` -- included into both
# cached_interp.c and pure_interp.c -- checksums each TLB-mapped code page with
# adler32() to notice self-modifying code. It is the library's only zlib call and
# the easiest kind to miss: psflib next door already links zlib, so on a platform
# where that arrives first this builds and links either way, and only a job with
# a different include order finds it. Same failure mode as the one recorded in
# codecs/gme/CMakeLists.txt.
find_package(ZLIB REQUIRED)

# Everything the Xcode project compiles, minus the two code-generator
# directories, plus empty_dynarec.c in their place. `instr_counters.c`,
# `rsp_lle/bench.c` and `r4300/new_dynarec/` are upstream files that project does
# not build either -- profiling and a second recompiler, both unreachable here.
add_library(vendor-lazyusf2 STATIC
    lazyusf2/ai/ai_controller.c
    lazyusf2/api/callbacks.c
    lazyusf2/debugger/dbg_decoder.c
    lazyusf2/main/main.c
    lazyusf2/main/rom.c
    lazyusf2/main/savestates.c
    lazyusf2/main/util.c
    lazyusf2/memory/memory.c
    lazyusf2/pi/cart_rom.c
    lazyusf2/pi/pi_controller.c
    lazyusf2/r4300/cached_interp.c
    lazyusf2/r4300/cp0.c
    lazyusf2/r4300/cp1.c
    lazyusf2/r4300/empty_dynarec.c
    lazyusf2/r4300/exception.c
    lazyusf2/r4300/interupt.c
    lazyusf2/r4300/mi_controller.c
    lazyusf2/r4300/pure_interp.c
    lazyusf2/r4300/r4300.c
    lazyusf2/r4300/r4300_core.c
    lazyusf2/r4300/recomp.c
    lazyusf2/r4300/reset.c
    lazyusf2/r4300/tlb.c
    lazyusf2/rdp/rdp_core.c
    lazyusf2/ri/rdram.c
    lazyusf2/ri/rdram_detection_hack.c
    lazyusf2/ri/ri_controller.c
    lazyusf2/rsp/rsp_core.c
    lazyusf2/rsp_hle/alist.c
    lazyusf2/rsp_hle/alist_audio.c
    lazyusf2/rsp_hle/alist_naudio.c
    lazyusf2/rsp_hle/alist_nead.c
    lazyusf2/rsp_hle/audio.c
    lazyusf2/rsp_hle/cicx105.c
    lazyusf2/rsp_hle/hle.c
    lazyusf2/rsp_hle/hvqm.c
    lazyusf2/rsp_hle/jpeg.c
    lazyusf2/rsp_hle/memory.c
    lazyusf2/rsp_hle/mp3.c
    lazyusf2/rsp_hle/musyx.c
    lazyusf2/rsp_hle/plugin.c
    lazyusf2/rsp_hle/re2.c
    lazyusf2/rsp_lle/rsp.c
    lazyusf2/si/cic.c
    lazyusf2/si/game_controller.c
    lazyusf2/si/n64_cic_nus_6105.c
    lazyusf2/si/pif.c
    lazyusf2/si/si_controller.c
    lazyusf2/usf/barray.c
    lazyusf2/usf/resampler.c
    lazyusf2/usf/usf.c
    lazyusf2/vi/vi_controller.c)

# Two include roots, and both are load-bearing. Consumers say
# <lazyusf2/usf/usf.h>, matching the vendored psflib next door; the sources say
# "usf/usf.h" and "api/m64p_types.h" among themselves, which only resolves from
# inside the tree.
target_link_libraries(vendor-lazyusf2 PUBLIC ZLIB::ZLIB)
target_include_directories(vendor-lazyusf2 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(vendor-lazyusf2 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/lazyusf2")
set_target_properties(vendor-lazyusf2 PROPERTIES FOLDER vendor)

# Warnings off: upstream C, and the tree being warning-free is a statement about
# our code. Same reasoning as vendor/psflib.
if(MSVC)
    target_compile_definitions(vendor-lazyusf2 PRIVATE _CRT_NONSTDC_NO_WARNINGS
                                                       _CRT_SECURE_NO_WARNINGS)
    target_compile_options(vendor-lazyusf2 PRIVATE /w)
else()
    # -fno-strict-aliasing is not tidiness. The N64 memory map is read through
    # whichever width the guest instruction used -- the same RDRAM byte arrives
    # as uint8_t, uint16_t and uint32_t through different pointers within one
    # function -- which is exactly what the aliasing rules say cannot happen.
    # Upstream mupen64plus builds with it for this reason; at -O2 without it the
    # compiler is entitled to reorder those accesses and the emulator quietly
    # renders the wrong audio.
    target_compile_options(vendor-lazyusf2 PRIVATE -w -fno-strict-aliasing)
endif()
