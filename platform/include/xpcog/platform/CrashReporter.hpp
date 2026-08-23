// Crash reporting, off until the listener says otherwise.
//
// Cog's arrangement, ported whole. Cog carries the Sentry Cocoa SDK, asks once
// on first launch whether it may send crash reports, and starts the SDK only if
// the answer was yes -- observing `sentryConsented` and calling
// `[SentrySDK close]` the moment it goes back to false
// (Application/AppController.m:384-425, Window/MainWindow.m:19-38). This is the
// same shape on sentry-native: nothing is initialised, no database directory is
// created and no request leaves the machine until startCrashReporting() is
// called, and the only thing that calls it is a true `sentryConsented`.
//
// That is a stronger promise than the SDK's own `require_user_consent` option,
// which starts the client and merely holds events back. Here there is no client
// at all, which is what "opt-in" should mean: revoking consent is not a filter
// applied to a running reporter, it is the reporter not existing.
//
// ---------------------------------------------------------------------------
// Why this lives in platform
// ---------------------------------------------------------------------------
// It looks like an application concern, and in Cog it is one. Two things put it
// here instead. The paths -- where a report database may be written, where a
// helper executable shipped beside the binary is -- are exactly the per-OS
// questions this layer already answers for the settings store and the library.
// And the backend is a *process-wide* crash handler: it installs itself under
// the whole program, not under a window, so it has no business depending on
// which toolkit drew one.
//
// What is left in the application is the part that genuinely is UI: asking, and
// offering the switch in Preferences.
//
// ---------------------------------------------------------------------------
// What a build without it does
// ---------------------------------------------------------------------------
// XPCOG_WITH_SENTRY is off by default, so an ordinary `cmake` with no options
// builds no reporter and pulls no dependency; the presets turn it on, the same
// way they do for FFmpeg. Every function below still exists and compiles in that
// build -- see NullCrashReporter.cpp -- so nothing above has to be written twice
// or wrapped in an #ifdef. crashReportingAvailable() is what a UI asks before
// offering a switch that could not do anything.

#pragma once

#include <string_view>

namespace xpcog::platform {

/// Where reports go. Cog names its DSN at the call site
/// (AppController.m:389); named here because this is the only place that sends
/// anything, and a DSN buried in a lambda is a thing nobody can find later.
inline constexpr std::string_view kCrashReportingDsn =
    "https://5e4f175c7f2cb157e3261fe01c6be4f3@cog-analytics.losno.co/22";

/// What the listener is agreeing to, in the words of whoever runs the endpoint.
///
/// Beside the DSN rather than beside the prompt, and for the same reason: the
/// policy describes what the service at that DSN does with what it is sent, so
/// the two belong together and cannot drift apart. Both the consent prompt and
/// the Preferences pane link to it.
inline constexpr std::string_view kPrivacyPolicyUrl =
    "https://www.iubenda.com/privacy-policy/59859310";

/// Whether this build has a crash reporter at all.
///
/// False in a build configured without XPCOG_WITH_SENTRY. The Preferences pane
/// asks so that it can show the switch greyed out with a reason, rather than
/// offering a toggle that silently does nothing.
[[nodiscard]] bool crashReportingAvailable() noexcept;

/// Whether it is running right now.
[[nodiscard]] bool crashReportingRunning() noexcept;

/// Starts reporting to kCrashReportingDsn. Call only with consent.
///
/// Everything else is decided from the running process: the release is XPCog's
/// version, the report database sits beside the library, and the crash handler
/// is the copy staged next to the executable. Idempotent -- a second call while
/// running does nothing.
///
/// Returns false when the build has no reporter, or when the SDK declined to
/// initialise. Not fatal either way, and not worth a dialog: a player that
/// cannot report a crash still plays.
bool startCrashReporting();

/// Stops reporting and flushes what is pending. Safe when not running.
///
/// This is `[SentrySDK close]` -- what Cog does the moment consent is withdrawn,
/// rather than at the next launch.
void stopCrashReporting();

/// Reports something that should not have happened.
///
/// A no-op unless reporting is running, so callers need no guard. Use it the
/// way Cog does and not more: "Sentry captureMessage is too spammy to use for
/// anything but actual errors" is a comment in its own PlaybackController.m:24,
/// written above a file of them commented out. A file the user dropped that no
/// decoder opens is not one of these; a playlist entry that is somehow not in
/// the playlist is.
void reportProblem(std::string_view message);

}  // namespace xpcog::platform
