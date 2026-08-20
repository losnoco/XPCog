// Settings in the native place each platform expects to find them: the registry
// on Windows, a plist on macOS, a file under $XDG_CONFIG_HOME on Linux.
//
// This is the only implementation of ISettingsStore that touches the OS. Core
// takes the interface, so the CLI and the tests use an in-memory store and need
// nothing from this layer at all.
//
// Keys are Cog's, unchanged, so an existing Cog preferences plist is read as-is
// on macOS rather than resetting the user's setup on first launch. That is also
// why macOS talks to CFPreferences directly rather than to any config library:
// what has to be read is a real plist, written by a different program, whose
// values are typed -- Cog stores `repeat` as an integer, not as a string.
//
// Every key in settings.def is a flat ASCII identifier with no separator in it,
// which is what lets these implementations be three small files rather than a
// dependency. Keep it that way: a key containing '/' or '=' would need a grouping
// or escaping scheme that none of the three currently has.

#pragma once

#include "xpcog/core/Settings.hpp"

#include <memory>
#include <string>

namespace xpcog::platform {

/// The store for this platform. Never null: a platform with no native mechanism
/// gets an in-memory one, so nothing has to branch.
[[nodiscard]] std::unique_ptr<ISettingsStore> makeNativeSettingsStore();

/// Reads and writes a specific file instead of the native location. Used by
/// tests, and by an import that wants to read Cog's own plist rather than ours.
///
/// Null where the platform's native store is not file-backed and the path could
/// only be honoured by ignoring it.
[[nodiscard]] std::unique_ptr<ISettingsStore> makeFileSettingsStore(const std::string& path);

/// Where the library database belongs on this platform. Core takes the path by
/// injection precisely so it does not have to know this.
///
/// Includes the organisation segment: %APPDATA%\LoSnoCo\XPCog on Windows, and the
/// equivalent elsewhere. That is where QStandardPaths::AppDataLocation put it, and
/// an existing installation's library is already there -- dropping the segment
/// would start an empty library beside a real one and say nothing.
[[nodiscard]] std::string libraryDatabasePath();

}  // namespace xpcog::platform
