// The entry point.
//
// Nothing but the macro. wxIMPLEMENT_APP lives in XPCogApp.cpp, which is in the
// library, so this translation unit exists purely to give the executable
// something of its own to compile -- the same shape app/src/main.cpp had, and for
// the same reason: everything testable stays in the library.
//
// wx supplies the real entry point itself (wWinMain on Windows, main elsewhere)
// from the implementation macro, so there is deliberately no main() written here.

#include "XPCogApp.hpp"
