// GCC/Clang bit builtins, for MSVC. Force-included into the vio2sf target on
// MSVC only (see ../CMakeLists.txt); this file is inert everywhere else.
//
// melonDS reaches for __builtin_popcount and friends unguarded in a handful of
// places -- ARMInterpreter_LoadStore.cpp and NonStupidBitfield.h among them --
// while `BitSet.h` right beside them does have an `#elif defined(_MSC_VER)`
// arm. The inconsistency is not surprising: Cog builds this framework for macOS
// alone, so no MSVC compiler has ever read these files.
//
// A shim rather than edits to the sources, because "MSVC lacks the GNU
// builtins" is a property of the whole translation unit set rather than of two
// particular lines, and the next file to use one should not need another patch.
//
// Software implementations rather than __popcnt: that intrinsic requires the
// POPCNT instruction and MSVC, unlike GCC, will not fall back when the target
// CPU lacks it. melonDS's own MSVC path in BitSet.h says exactly this and does
// the same.
#pragma once

#ifdef _MSC_VER

#include <intrin.h>

static __forceinline int __builtin_popcount(unsigned int value) {
    value = value - ((value >> 1) & 0x55555555u);
    value = (value & 0x33333333u) + ((value >> 2) & 0x33333333u);
    value = (value + (value >> 4)) & 0x0F0F0F0Fu;
    return static_cast<int>((value * 0x01010101u) >> 24);
}

static __forceinline int __builtin_popcountll(unsigned long long value) {
    return __builtin_popcount(static_cast<unsigned int>(value)) +
           __builtin_popcount(static_cast<unsigned int>(value >> 32));
}

// Undefined for zero, exactly as the GNU builtins are.
static __forceinline int __builtin_ctz(unsigned int value) {
    unsigned long index = 0;
    _BitScanForward(&index, value);
    return static_cast<int>(index);
}

static __forceinline int __builtin_ctzll(unsigned long long value) {
    unsigned long index = 0;
    _BitScanForward64(&index, value);
    return static_cast<int>(index);
}

static __forceinline int __builtin_clz(unsigned int value) {
    unsigned long index = 0;
    _BitScanReverse(&index, value);
    return 31 - static_cast<int>(index);
}

// A control-flow intrinsic rather than a value, so a macro rather than a
// function: MSVC's __assume(0) has to appear at the use site to tell the
// optimiser the path is unreachable.
#define __builtin_unreachable() __assume(0)

static __forceinline int __builtin_clzll(unsigned long long value) {
    unsigned long index = 0;
    _BitScanReverse64(&index, value);
    return 63 - static_cast<int>(index);
}

#endif  // _MSC_VER
