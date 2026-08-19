# vio2sf: the Nintendo DS core behind 2SF.
#
# The name is Cog's and it is a fossil. vio2sf was originally a DeSmuME
# derivative; kode54 replaced its contents with **melonDS** and kept the
# framework name, so `Frameworks/vio2sf` in Cog contains melonDS and Cog's
# decoder reaches it through `#import <vio2sf/NDS.h>`. Those include paths are
# baked into every one of the source files, which is why the directory below is
# literally named `vio2sf/` rather than being tidied to `melonDS/` -- the same
# reasoning that keeps psflib's nested layout in vendor/psflib.
#
# Vendored rather than an overlay port even though melonDS has a real upstream:
# this copy is modified (the include rewrite above, kode54's Platform shim),
# which ../README.md and ../../ports/README.md both make the `vendor/` case.
#
# ---------------------------------------------------------------------------
# No JIT
# ---------------------------------------------------------------------------
# melonDS carries an ARM recompiler with x86-64 and AArch64 backends, and Cog
# builds it: `JIT_ENABLED=1` is in its Xcode project unconditionally, for both
# configurations. This build leaves it out and runs the interpreter.
#
# That is a deliberate trade rather than an oversight. A JIT has to allocate
# memory that is both writable and executable: on Apple Silicon that means
# MAP_JIT plus pthread_jit_write_protect_np and the
# com.apple.security.cs.allow-jit entitlement on a hardened-runtime build, and
# on Windows it is why melonDS reaches for `onecore`. That is real packaging
# surface for a player whose job here is to render a few minutes of DS audio,
# and none of it is needed while the interpreter keeps up. Revisit if it stops
# keeping up -- the sources are all still here, behind ENABLE_JIT upstream.
# ---------------------------------------------------------------------------
# Local changes to the vendored sources
# ---------------------------------------------------------------------------
# Listed here so a re-vendor does not silently drop them; each is marked in
# place with an "XPCog local change" comment.
#
#   src/GPU2D_Soft.cpp  `asm volatile ("" : : : "memory")` guarded out for MSVC,
#                       which has no GNU inline-asm syntax. It is a compiler
#                       barrier working around a video artifact under LTO on
#                       aarch64, and this build renders no video.
#
#   src/SPU.cpp         SPU::BufferAudio sized its read buffer with a
#                       variable-length array. VLAs are C99, allowed in C++ only
#                       as a GCC/Clang extension, and MSVC has neither. The
#                       length is bounded by the capacity given to blip_new, now
#                       named kBlipBufferSamples and used at both ends.
#
#   vio2sf/GPU.h        GPU::VRAMCaptureBlockFlags gained the `{}` every array
#                       beside it already had. Read before anything sets it, by
#                       a loop the constructor runs, whose body dereferences a
#                       renderer that does not exist yet. Zeroed heap hid it on
#                       macOS and Linux; MSVC's debug heap did not.
#
#   src/sha1/sha1.c     Added _WIN32 to the little-endian architecture list. The
#                       list is BSD vintage -- __i386__, __alpha__, Borland's
#                       __WIN32__ -- so under cl.exe no arm matched and the
#                       file's own "Undefined or invalid BYTE_ORDER" #error
#                       fired.

find_package(ZLIB REQUIRED)

add_library(vendor-vio2sf STATIC
    src/ARCodeFile.cpp
    src/ARDatabaseDAT.cpp
    src/AREngine.cpp
    src/ARM.cpp
    src/ARMInterpreter.cpp
    src/ARMInterpreter_ALU.cpp
    src/ARMInterpreter_Branch.cpp
    src/ARMInterpreter_LoadStore.cpp
    src/CP15.cpp
    src/CRC32.cpp
    src/DMA.cpp
    src/DMA_Timings.cpp
    src/FATIO.cpp
    src/FATStorage.cpp
    src/GBACart.cpp
    src/GBACartMotionPak.cpp
    src/GPU.cpp
    src/GPU_Soft.cpp
    src/GPU2D.cpp
    src/GPU2D_Soft.cpp
    src/GPU3D.cpp
    src/GPU3D_Soft.cpp
    src/GPU3D_Texcache.cpp
    src/Mic.cpp
    src/NDS.cpp
    src/NDSCart.cpp
    src/NDSCartR4.cpp
    src/ROMList.cpp
    src/FreeBIOS.cpp
    src/RTC.cpp
    src/Savestate.cpp
    src/SPI.cpp
    src/SPI_Firmware.cpp
    src/SPU.cpp
    src/Utils.cpp
    src/Wifi.cpp
    src/WifiAP.cpp
    src/fatfs/ff.c
    src/fatfs/ffsystem.c
    src/fatfs/ffunicode.c
    src/sha1/sha1.c
    src/tiny-AES-c/aes.c
    src/xxhash/xxhash.c
    src/blip-buf/blip_buf.c

    platform/Platform.cpp)

# `<vio2sf/NDS.h>` for both this library and its consumers; `src/` privately,
# because a handful of melonDS sources include "version.h" beside themselves.
target_include_directories(vendor-vio2sf SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# The header tree is flat, which is not how it sits in Cog's repository and is
# exactly how Cog's compiler sees it: the framework copies every header into one
# `vio2sf.framework/Headers/`, and the sources were rewritten to match --
# `#include <vio2sf/ff.h>` for a file that lives in `fatfs/`. Preserving the
# subdirectories on disk gives a tree that cannot compile. There are no basename
# collisions across the 73 headers, so flattening is lossless.
target_include_directories(vendor-vio2sf PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/vio2sf")
target_link_libraries(vendor-vio2sf PUBLIC ZLIB::ZLIB)
target_compile_features(vendor-vio2sf PUBLIC cxx_std_17)
set_target_properties(vendor-vio2sf PROPERTIES FOLDER vendor)

# What melonDS's own CMakeLists asks for on Win32, which a hand-written build
# has to ask for too -- the same omission that took the Windows job down for
# mGBA, in the same shape.
#
# `dirent.h` is a POSIX header MSVC does not ship, and FATStorage.cpp includes
# it unconditionally. Upstream reaches for a `dirent` package there and so does
# this; it is a manifest dependency in ../../vcpkg.json, qualified to Windows.
if(WIN32)
    target_link_libraries(vendor-vio2sf PUBLIC ole32 comctl32 wsock32 ws2_32)
    if(MSVC)
        find_path(XPCOG_DIRENT_INCLUDE_DIR "dirent.h" REQUIRED)
        target_include_directories(vendor-vio2sf PRIVATE "${XPCOG_DIRENT_INCLUDE_DIR}")
    endif()
endif()

if(MSVC)
    # NOMINMAX is the one that looks optional and is not. Something in melonDS
    # pulls in <windows.h>, which defines `max` as a macro, and every
    # `std::max(a, b)` in the ARM core then fails to parse:
    #
    #   ARM.h(288): error C2589: '(': illegal token on right side of '::'
    #
    # XPCog::warnings sets it for first-party targets, and vendored code
    # deliberately does not link that -- so it has to be stated here.
    # _USE_MATH_DEFINES because <cmath> on MSVC does not declare M_PI without
    # it, and GBACartMotionPak.cpp uses it. Another of the "Cog only ever built
    # this for macOS" gaps rather than anything subtle.
    target_compile_definitions(vendor-vio2sf PRIVATE _CRT_SECURE_NO_WARNINGS
                                                     _CRT_NONSTDC_NO_WARNINGS
                                                     _USE_MATH_DEFINES
                                                     NOMINMAX
                                                     WIN32_LEAN_AND_MEAN)
    # The GNU bit builtins melonDS uses unguarded; see the header for why this
    # is a force-include rather than edits to the sources.
    #
    # PUBLIC rather than PRIVATE, which /w above deliberately is not. The
    # builtins are not confined to the .cpp files: NonStupidBitfield.h calls
    # __builtin_ctzll from an inline member, and that header comes in with
    # <vio2sf/NDS.h>. So the 2SF decoder -- first-party code, compiled at /W4 --
    # fails on a header it merely includes unless it gets the shim too. Which
    # is the same argument the header makes for being a shim at all: what MSVC
    # lacks is a property of everything that reads these sources, not of the
    # library's own translation units.
    target_compile_options(vendor-vio2sf PRIVATE /w)
    target_compile_options(vendor-vio2sf PUBLIC
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/platform/MsvcBuiltins.h")
else()
    # -fwrapv is upstream's, and PUBLIC there for a reason: the ARM interpreter
    # relies on signed overflow wrapping to compute the V flag, and signed
    # overflow is undefined behaviour without it. At -O2 the compiler is
    # entitled to assume it never happens, and the emulated CPU then takes the
    # wrong branch.
    target_compile_options(vendor-vio2sf PRIVATE -w -fwrapv)
endif()
