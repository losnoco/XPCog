// What this process calls itself, as far as the desktop is concerned.
//
// Not the same question as wxApp::GetAppName(), and keeping them apart is the
// point of this header. GetAppName() is "XPCog": it is what the user reads in a
// title bar, and on Unix it is what wxStandardPaths derives the configuration
// directory from, so it cannot be changed without moving a user's settings. The
// name here is the reverse-DNS application ID, co.losno.XPCog, and it exists so
// that a desktop shell can match a *running window* to the installed .desktop
// file that describes it.
//
// The match is spelled differently on the two display protocols, and only one of
// them can be fixed from the .desktop side:
//
//   X11      the window carries WM_CLASS, a pair of (instance, class) strings.
//            A shell compares those against the desktop file's StartupWMClass,
//            so a mismatch is repairable by editing that key.
//
//   Wayland  the surface carries an app_id and nothing else, and the shell
//            matches it against the desktop file's *basename*. There is no
//            StartupWMClass equivalent and no key that can bridge a difference:
//            either the app_id is co.losno.XPCog or the window has no icon.
//
// GTK takes both from g_get_prgname(), which gtk_init() fills in from argv[0] --
// "XPCog" -- unless someone has set it first. wxWidgets does not: its GTK port
// calls gdk_set_program_class(), which sets only the X11 *class* half, and never
// touches the program name. So the app_id is argv[0]'s basename by default, and
// the Wayland case above is the failing one.
//
// applyDesktopIdentity() is what sets it, and it has to run before the toolkit
// initialises -- see XPCogApp::Initialize, which is the hook that is early
// enough.

#pragma once

#include <string_view>

namespace xpcog::platform {

/// The reverse-DNS application ID this build installs under, or empty on a
/// platform that has no such concept and on a build that installs no .desktop
/// file. Empty is the answer to act on, not a failure: a caller with no ID has
/// nothing to match a window against and should leave the toolkit's defaults
/// alone.
[[nodiscard]] std::string_view desktopApplicationId();

/// Adopt that ID as the process's name, so that a window created later carries
/// it as its Wayland app_id and its X11 WM_CLASS instance.
///
/// **Must be called before the UI toolkit initialises.** GTK reads the program
/// name once, during gtk_init(), and a window's app_id is fixed when the surface
/// is created; setting this afterwards changes nothing and reports no error.
///
/// Does nothing where desktopApplicationId() is empty.
void applyDesktopIdentity();

}  // namespace xpcog::platform
