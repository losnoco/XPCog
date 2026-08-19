// XPCog local change: silence the emulator's console diagnostics.
//
// nuked-sc55 prints to stdout as it runs -- "ROM set autodetect: SC-55mk2" once,
// and then "Unknown write e400 4" every time the firmware touches a register the
// emulation does not model, which the SC-55mkII's boot code does repeatedly.
// That is right for a standalone emulator with a terminal and wrong inside a
// music player, where it lands in whatever the process's stdout happens to be.
//
// Done by force-include (see ../CMakeLists.txt) rather than by editing the
// thirty-odd call sites, so the vendored sources stay byte-identical to Cog's
// and a future update is a copy rather than a merge. The same mechanism
// vendor/vio2sf uses for its MSVC builtins.
//
// Deliberately not a switch. Nothing in this player can act on "unknown write
// at e400", and a diagnostic nobody reads is one that trains people to ignore
// the console.

#pragma once

#include <stdio.h>

#undef printf
#define printf(...) ((void)0)
