// Crash reporting on sentry-native, started only with consent.
//
// One file for all three platforms, which is the reason sentry-native was worth
// taking over anything hand-rolled: the C API is the same everywhere, and the
// per-OS parts -- the minidump writer, the unwinder, the transport -- are the
// library's problem rather than this tree's. Cog uses the Cocoa SDK because Cog
// is macOS; the options set below are that SDK's options, one for one, minus the
// ones with no counterpart here (see the notes at each).
//
// ---------------------------------------------------------------------------
// The two paths this has to get right
// ---------------------------------------------------------------------------
// **The database.** sentry_options_new() defaults it to the relative path
// ".sentry-native", which resolves against the *working directory* -- for a
// player launched by double-clicking a file that is wherever the file was, and
// for one launched from a package manager's shim it may be somewhere unwritable.
// So it is set explicitly, beside the library database, which is already the
// per-user writable directory this layer knows how to find.
//
// **The handler.** The crashpad backend is a separate process, and
// sentry_options_new() sets *no* default for its path: leave it and the backend
// declines to start, silently, and only messages are ever reported. It is the
// copy staged next to the executable by app/CMakeLists.txt. Both are given as
// wide strings on Windows, because both run through a user's profile directory
// and a name the active code page cannot spell would otherwise be mangled into a
// path that does not exist -- the same failure core/FilePath.hpp was written for.

#include "xpcog/platform/CrashReporter.hpp"

#include "xpcog/core/AssetPath.hpp"
#include "xpcog/core/FilePath.hpp"
#include "xpcog/core/Version.hpp"
#include "xpcog/platform/SettingsStore.hpp"

#include <sentry.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>

namespace xpcog::platform {
namespace {

/// Guards start against stop. Both are called from the interface thread today --
/// launch, and a checkbox -- but sentry_init() and sentry_close() are not things
/// to leave depending on that, because the whole point of the object they set up
/// is that it also runs on threads nobody scheduled.
std::mutex& lock() {
    static std::mutex mutex;
    return mutex;
}

/// Read without the lock by crashReportingRunning(), which is only ever asked to
/// draw a checkbox and does not want to wait behind a flush.
std::atomic<bool> running{false};

/// The `logger` field on every event this sends. Sentry groups by it, and the
/// alternative -- the SDK's default of none -- puts messages from the player in
/// with anything else that ever reports to the same project.
constexpr std::string_view kLogger = "xpcog";

/// Where reports queue up until they are sent.
///
/// Beside the library rather than in a directory of its own: it is per-user
/// state belonging to this installation, which is exactly what
/// libraryDatabasePath() already resolves -- %APPDATA%\LoSnoCo\XPCog and its
/// equivalents. Deriving it means there is one answer to "where does XPCog keep
/// things" rather than two that can disagree.
[[nodiscard]] std::filesystem::path databaseDirectory() {
    const std::filesystem::path library = pathFromUtf8(libraryDatabasePath());
    if (!library.has_parent_path()) {
        // libraryDatabasePath() falls back to a bare "library.db" when the
        // platform will not name a data directory. Keeping the reports beside it
        // is the same fallback, and no worse.
        return "crash-reports";
    }
    return library.parent_path() / "crash-reports";
}

/// The crashpad helper, staged next to the executable by the build.
///
/// Empty when it is not there, which is a degraded build rather than a broken
/// one: sentry_init() still succeeds and reportProblem() still works, and what
/// is lost is the reporting of actual crashes. Returning empty rather than a
/// path to nothing is what lets that be said in the log once, here, instead of
/// being discovered from an empty Sentry project.
[[nodiscard]] std::filesystem::path handlerPath() {
    const std::filesystem::path directory = executableDirectory();
    if (directory.empty()) {
        return {};
    }
#if defined(_WIN32)
    const std::filesystem::path handler = directory / L"crashpad_handler.exe";
#else
    const std::filesystem::path handler = directory / "crashpad_handler";
#endif
    std::error_code error;
    return std::filesystem::is_regular_file(handler, error) ? handler
                                                            : std::filesystem::path{};
}

}  // namespace

bool crashReportingAvailable() noexcept { return true; }

bool crashReportingRunning() noexcept { return running.load(std::memory_order_acquire); }

bool startCrashReporting() {
    const std::lock_guard<std::mutex> guard(lock());
    if (running.load(std::memory_order_relaxed)) {
        return true;
    }

    sentry_options_t* options = sentry_options_new();
    sentry_options_set_dsn_n(options, kCrashReportingDsn.data(),
                             kCrashReportingDsn.size());

    // "xpcog@0.1.0". Sentry's own convention for a release name, and what makes
    // a stack trace resolvable against the symbols for the build it came from.
    const std::string release = "xpcog@" + std::string{kVersionString};
    sentry_options_set_release(options, release.c_str());

    // So a crash from someone's working tree does not land in the same bucket as
    // one from a build a listener is running.
#ifdef NDEBUG
    sentry_options_set_environment(options, "release");
#else
    sentry_options_set_environment(options, "debug");
#endif

    const std::filesystem::path database = databaseDirectory();
    const std::filesystem::path handler  = handlerPath();
#if defined(_WIN32)
    sentry_options_set_database_pathw(options, database.c_str());
    if (!handler.empty()) {
        sentry_options_set_handler_pathw(options, handler.c_str());
    }
#else
    sentry_options_set_database_path(options, database.c_str());
    if (!handler.empty()) {
        sentry_options_set_handler_path(options, handler.c_str());
    }
#endif

    // Cog sets debug unconditionally -- "Enabled debug when first installing is
    // always helpful" (AppController.m:390). Not copied unconditionally, because
    // the SDK's debug output goes to stderr and this is a WIN32_EXECUTABLE with
    // no console attached: on the platform where it would be most helpful it goes
    // nowhere at all. A debug build has a console and a developer reading it.
#ifdef NDEBUG
    sentry_options_set_debug(options, 0);
#else
    sentry_options_set_debug(options, 1);
#endif

    // Not set, and each for a reason:
    //
    //   require_user_consent -- the SDK's own opt-in gate, which starts the
    //     client and withholds events. Consent here decides whether there is a
    //     client at all, which is the stronger promise; see CrashReporter.hpp.
    //
    //   traces_sample_rate -- Cog turns tracing on and instruments it, with
    //     transactions around loading playlist entries and reading tags
    //     (PlaylistLoader.m:465-800). Nothing here starts a transaction, so a
    //     sample rate would only cost requests carrying nothing.
    //
    //   app-hang tracking -- Cog turns it *off* ("lots of false positives
    //     still"). sentry-native has no such feature, so there is nothing to
    //     turn off, and if it ever gains one this is the note saying not to.
    //
    // Session tracking is left on, which is the SDK's default and Cog's: it is
    // the "usage data" half of what the preference says it sends.

    if (sentry_init(options) != 0) {
        return false;
    }
    running.store(true, std::memory_order_release);
    return true;
}

void stopCrashReporting() {
    const std::lock_guard<std::mutex> guard(lock());
    if (!running.load(std::memory_order_relaxed)) {
        return;
    }
    // Stores false first: sentry_close() flushes, which takes a moment, and
    // nothing should be handing it a new event while it does.
    running.store(false, std::memory_order_release);
    sentry_close();
}

void reportProblem(std::string_view message) {
    // Under the lock, on an error path, deliberately: sentry_close() flushes,
    // and an event must not be handed to a client that is in the middle of going
    // away. Something has already gone wrong by the time this is called, so a
    // wait costs nothing anyone will notice.
    const std::lock_guard<std::mutex> guard(lock());
    if (!running.load(std::memory_order_relaxed)) {
        return;
    }
    // The `_n` forms because `message` is a view and need not be terminated.
    sentry_capture_event(sentry_value_new_message_event_n(
        SENTRY_LEVEL_ERROR, kLogger.data(), kLogger.size(), message.data(),
        message.size()));
}

}  // namespace xpcog::platform
