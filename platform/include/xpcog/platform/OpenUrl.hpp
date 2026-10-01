// Handing a URL to whatever the desktop opens URLs with.
//
// This was wxLaunchDefaultBrowser, and its one caller is the Last.fm
// authorisation trip: the API's design is that the *listener* approves the token
// in a browser, so there is no way to do it in-process and no reason to want
// one.
//
// Answers nothing. Every platform's version of this is fire-and-forget in
// practice -- a browser that failed to launch has already told the user more
// clearly than this program could -- and a bool nobody can act on is worse than
// none.

#pragma once

#include <string>

namespace xpcog::platform {

/// Opens `url` in whatever handles its scheme.
void openInBrowser(const std::string& url);

}  // namespace xpcog::platform
