// The crash reporter, checked for the thing it must never do.
//
// Deliberately narrow, and the narrowness is the point: **nothing here starts
// it**. startCrashReporting() initialises a real Sentry client against a real
// DSN and, with session tracking on, announces the launch -- so a test that
// called it would file CI runs as XPCog sessions and would do it from a machine
// nobody consented on. The one behaviour worth pinning is the other one: that
// every function is safe and silent until somebody has said yes.
//
// That is exactly where an opt-in arrangement goes wrong, too. The failure is
// never a loud one; it is a call site that reports something on a path reached
// before consent was read, and the only evidence is events arriving in a project
// from people who never agreed to send any.
//
// Lives in the application suite rather than the core one because xpcog-tests
// links core and codecs and no platform layer at all.

#include "xpcog/platform/CrashReporter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using namespace xpcog;

TEST_CASE("nothing runs until consent starts it", "[crash]") {
    // Nothing in this process has consented, so whatever the build is, the
    // reporter is not running.
    CHECK_FALSE(platform::crashReportingRunning());

    // And the error paths that report problems are callable anyway. Every one of
    // them is a place where something has already gone wrong, and requiring them
    // to test a flag first is how one of them ends up testing it wrongly.
    platform::reportProblem("a test message that must not leave this machine");
    CHECK_FALSE(platform::crashReportingRunning());

    // Stopping what was never started is a no-op, not a crash. XPCogApp::OnExit
    // calls this unconditionally.
    platform::stopCrashReporting();
    CHECK_FALSE(platform::crashReportingRunning());
}

TEST_CASE("the reporter says whether this build has one", "[crash]") {
    // Whichever way this build was configured, the answer has to match it: the
    // Preferences pane greys the switch on a false, and a false in a build that
    // does have a reporter would leave the listener unable to turn it on.
#ifdef XPCOG_TESTS_EXPECT_SENTRY
    CHECK(platform::crashReportingAvailable());
#else
    CHECK_FALSE(platform::crashReportingAvailable());
#endif
}

TEST_CASE("the DSN and the policy are well-formed", "[crash]") {
    // Cheap, and it catches the edit that would otherwise be found by an empty
    // Sentry project: a DSN missing its key, or a project id lost off the end.
    const std::string_view dsn = platform::kCrashReportingDsn;
    CHECK(dsn.starts_with("https://"));
    CHECK(dsn.find('@') != std::string_view::npos);
    // Something after the host's last slash, which is the project id.
    CHECK(dsn.rfind('/') > dsn.find('@'));
    CHECK(dsn.rfind('/') + 1 < dsn.size());

    CHECK(std::string_view{platform::kPrivacyPolicyUrl}.starts_with("https://"));
}
