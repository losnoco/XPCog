// A build configured without XPCOG_WITH_SENTRY.
//
// Four definitions rather than an #ifdef in every caller. The application asks
// crashReportingAvailable() once, to decide whether to offer the switch at all;
// everything else it does unconditionally, and lands here doing nothing.
//
// reportProblem() in particular has to exist and be free: its call sites are
// error paths that already have a message to hand, and making each of them test
// a build option first is how one of them ends up testing the wrong one.

#include "xpcog/platform/CrashReporter.hpp"

namespace xpcog::platform {

bool crashReportingAvailable() noexcept { return false; }

bool crashReportingRunning() noexcept { return false; }

bool startCrashReporting() { return false; }

void stopCrashReporting() {}

void reportProblem(std::string_view message) { (void)message; }

}  // namespace xpcog::platform
