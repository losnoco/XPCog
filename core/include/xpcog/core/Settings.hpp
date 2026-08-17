// Typed settings, replacing Cog's 339 scattered NSUserDefaults call sites.
//
// Core reads settings by injection rather than from a global: AudioEngine takes a
// const Settings& and passes it down. That is slightly more plumbing than Cog's
// [NSUserDefaults standardUserDefaults] everywhere, and it is exactly what lets a
// test run the engine with its own settings.

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog {

/// Where settings actually live. The app backs this with QSettings (native plist,
/// registry or INI); tests use an in-memory implementation.
class ISettingsStore {
public:
    virtual ~ISettingsStore() = default;

    [[nodiscard]] virtual std::optional<std::string> getRaw(std::string_view key) const = 0;
    virtual void setRaw(std::string_view key, std::string_view value)                   = 0;
    virtual void remove(std::string_view key)                                           = 0;
    virtual void sync() {}
};

/// A store held entirely in memory. Used by tests and by the CLI.
[[nodiscard]] std::unique_ptr<ISettingsStore> makeMemorySettingsStore();

class Settings {
public:
    explicit Settings(ISettingsStore& store) : store_(store) {}

    // Generated accessors: one getter and one setter per entry in settings.def.
#define XPCOG_SETTING(Ident, Type, Key, Default)     \
    [[nodiscard]] Type Ident() const;                \
    void set##Ident(const Type& value);
#include "xpcog/core/settings.def"
#undef XPCOG_SETTING

    /// Describes every setting, for the preferences UI and reset-to-defaults -- so
    /// neither has to repeat the list.
    struct Desc {
        std::string_view ident;
        std::string_view key;
        std::string_view type;
        std::string_view defaultValue;
    };
    [[nodiscard]] static std::span<const Desc> all() noexcept;

    /// Access by key rather than by name, for callers that iterate all() and so
    /// cannot name a generated accessor -- the preferences UI, and the engine
    /// reading the equaliser's 31 bands by their table. Returns the stored value,
    /// or the declared default when nothing is stored.
    [[nodiscard]] std::string rawValue(std::string_view key) const;
    void setRawValue(std::string_view key, std::string_view value);

    /// The declared default for `key`, with the quotes that stringifying a
    /// string literal leaves behind already removed. Empty for an unknown key.
    [[nodiscard]] static std::string defaultValue(std::string_view key);

    /// Restores every setting to its default.
    void resetAll();

    /// Flushes to the backing store. QSettings writes lazily, so without this a
    /// setting changed just before quitting can be lost.
    void sync() { store_.sync(); }

    /// Runs any pending versioned migrations, then records the new version.
    /// Replaces Cog's ad-hoc renaming block in AppController.m:775-855.
    void applyMigrations();

private:
    ISettingsStore& store_;
};

}  // namespace xpcog
