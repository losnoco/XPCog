// Running something on the thread that owns the user interface.
//
// The same shape as platform::Dispatcher, and for the same reason: a component
// whose work finishes on a thread the application did not pick has to hand the
// hop back to the application rather than decide for itself what "the interface
// thread" means. Core does not know what the interface is built with, and this
// is how it stays that way.
//
// Its own header now because it has a third user. ScanTask declared it inline
// and said so; the remote control is the second here and the tenth reason to
// stop restating it. platform::Dispatcher is left where it is rather than made
// an alias of this one: core must not depend on platform, and the two are
// deliberately the same type spelled twice rather than one leaking upward.

#pragma once

#include <functional>

namespace xpcog {

/// Runs a callable on the thread that owns the user interface.
///
/// Supplied by whoever hosts the component. In the application it wraps
/// wxEvtHandler::CallAfter; in xpcog-cli it posts to the SerialExecutor that
/// stands in for an interface thread; in tests it is usually a queue the test
/// drains where it chooses.
///
/// It must remain safe to call from any thread for as long as the component
/// holding it lives, and it must not run anything after that component is gone.
using Dispatcher = std::function<void(std::function<void()>)>;

}  // namespace xpcog
