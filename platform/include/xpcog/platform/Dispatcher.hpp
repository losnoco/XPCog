// Running something on the thread that owns the user interface.
//
// This lived in MediaIntegration.hpp until the tray icon became the second seam
// in this directory to want it. Both are OS integrations whose callbacks arrive
// on a thread the application did not pick -- a WinRT pool thread, a dispatch
// queue, whichever thread pumps D-Bus -- and both hand the hop back to the
// application rather than deciding for themselves what "the UI thread" means.
//
// Its own header rather than one of the two including the other: a tray icon does
// not implement MediaIntegration and has no business seeing NowPlayingInfo.

#pragma once

#include <functional>

namespace xpcog::platform {

/// Runs a callable on the thread that owns the user interface.
///
/// Supplied by the application, because this layer deliberately does not know
/// what the interface is built with. In XPCog it wraps wxEvtHandler::CallAfter.
using Dispatcher = std::function<void(std::function<void()>)>;

}  // namespace xpcog::platform
