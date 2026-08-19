// The one translation unit that compiles minimp3. Every other file including
// these headers gets declarations only.
//
// The configuration macros are set by the CMake target as PUBLIC, not here: they
// change the ABI -- MINIMP3_FLOAT_OUTPUT redefines mp3d_sample_t -- so a
// consumer compiled without them would disagree with this file about the size of
// every sample buffer, and link cleanly while doing it.

#define MINIMP3_IMPLEMENTATION 1

#include "minimp3_ex.h"
