// Settings backed by QSettings: a plist on macOS, the registry on Windows, an
// INI file on Linux -- the native place each platform expects to find them.
//
// This is the only implementation of ISettingsStore that touches the OS. Core
// takes the interface, so the CLI and the tests use an in-memory store and never
// need a QCoreApplication.
//
// Keys are Cog's, unchanged, so an existing Cog preferences plist is read as-is
// on macOS rather than resetting the user's setup on first launch.

#pragma once

#include "xpcog/core/Settings.hpp"

#include <QSettings>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace xpcog::platform {

class QSettingsStore final : public ISettingsStore {
public:
    /// Uses the organisation and application names set on QCoreApplication.
    QSettingsStore();

    /// Reads and writes a specific file. Used by tests, and by an import that
    /// wants to read Cog's own plist rather than ours.
    explicit QSettingsStore(const QString& filePath);

    [[nodiscard]] std::optional<std::string> getRaw(std::string_view key) const override;
    void setRaw(std::string_view key, std::string_view value) override;
    void remove(std::string_view key) override;
    void sync() override;

private:
    /// mutable because QSettings::value() is non-const, while reading a setting
    /// plainly is a const operation from the caller's point of view.
    mutable QSettings settings_;
};

/// Where the library database belongs on this platform. Core takes the path by
/// injection precisely so it does not have to know this.
[[nodiscard]] std::string libraryDatabasePath();

}  // namespace xpcog::platform
