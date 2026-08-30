// The Linux answer: g_set_prgname(), early.
//
// See the header for why the program name rather than anything wx offers, and
// why it has to happen before gtk_init().

#include "xpcog/platform/DesktopIdentity.hpp"

#include <glib.h>

namespace xpcog::platform {

std::string_view desktopApplicationId() {
#ifdef XPCOG_DESKTOP_ID
    return XPCOG_DESKTOP_ID;
#else
    // A build with no install rules configured. Returning empty leaves the
    // toolkit's argv[0]-derived default in place, which is what this has always
    // done and is no worse than a name matching a file nobody installed.
    return {};
#endif
}

void applyDesktopIdentity() {
#ifdef XPCOG_DESKTOP_ID
    // The macro rather than desktopApplicationId(): g_set_prgname takes a C
    // string, a string_view is not obliged to be terminated, and building a
    // std::string to bridge that would allocate to say what the literal already
    // says. GLib copies what it is given, so there is nothing to keep alive.
    //
    // Unconditional rather than checked-then-set. g_get_prgname() is non-null by
    // this point only if something else has already chosen a name, and the two
    // callers who could have are GTK -- which has not run yet, that being the
    // whole timing requirement -- and a wrapper that deliberately renamed the
    // process, which is not a case worth deferring to over the ID that matches
    // the installed .desktop file.
    g_set_prgname(XPCOG_DESKTOP_ID);
#endif
}

}  // namespace xpcog::platform
