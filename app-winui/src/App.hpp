#pragma once

namespace xpcog::winui {

/// Runs the WinUI application until its last window closes: the settings, the
/// codecs and the session are built when it launches, the session is saved
/// when the window closes, and all of it is torn down before this returns --
/// after the dispatcher has stopped, so nothing posted to it can run against a
/// session that is already gone.
///
/// The Windows App Runtime has to be bootstrapped first; see Runtime.hpp.
int runApplication();

}  // namespace xpcog::winui
