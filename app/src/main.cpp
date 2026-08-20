// The entry point, and the only translation unit that has one.
//
// wxIMPLEMENT_APP is here rather than in XPCogApp.cpp, and that is not tidiness.
// The macro expands to three things -- theme support, an entry point, and the
// app factory -- and the entry point is `main()` on macOS and Linux. XPCogApp.cpp
// is compiled into xpcog-appcore, which the test binary also links, so with the
// macro there the static library offered a `main` and the linker took it: on
// macOS `xpcog-app-tests --list-tests` printed *XPCog's* usage message and
// Catch2's test discovery failed with "Unknown long option 'list-tests'".
//
// Windows hid this completely. There wxIMPLEMENT_WXWIN_MAIN expands to
// wWinMain, the test executable is a console program wanting `main`, and the two
// never collide -- so the same library worked for two toolkits' worth of builds
// and broke the first time it was linked on a platform where the entry point is
// spelled the same way in both.
//
// Only the executable compiles this file, so only the executable gets an entry
// point. Nothing in xpcog-appcore calls wxGetApp(), which is what makes moving
// the whole macro possible rather than having to split it and leave the factory
// behind -- and a split would have been its own trap, since a factory registered
// by a static initialiser in a static library is only linked in if something
// references the object file holding it.

#include "XPCogApp.hpp"

wxIMPLEMENT_APP(xpcog::app::XPCogApp);
