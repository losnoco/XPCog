// The three Commodore 64 ROM images libsidplayfp runs tunes against.
//
// `sidplayfp::setRoms()` takes the KERNAL, BASIC and character ROMs, and
// without them the emulated machine has no operating system: a tune whose play
// routine calls into KERNAL, or one that is a BASIC program, does not run. The
// player will still start PSID tunes that touch neither, which is most of them,
// so the absence shows up as a minority of files failing rather than as nothing
// working -- which is the harder failure to notice.
//
// These are Commodore's copyrighted code, vendored because Cog vendors them
// (`Plugins/sidplay/roms.cpp`), the same decision and the same shape as the
// PlayStation BIOS in vendor/highlyexperimental. Recorded rather than defended.
// The alternative, if it is ever wanted, is reading them from a user data
// directory at runtime and letting the decoder run without them.
#pragma once

extern "C" const unsigned char kernel[8192];
extern "C" const unsigned char basic[8192];
extern "C" const unsigned char chargen[4096];
