#include "xpcog/core/Settings.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <map>
#include <mutex>
#include <optional>

namespace xpcog {
namespace {

// --- string <-> value conversions used by the generated accessors ----------

[[nodiscard]] std::string toStorage(const std::string& value) { return value; }
[[nodiscard]] std::string toStorage(bool value) { return value ? "true" : "false"; }
[[nodiscard]] std::string toStorage(int value) { return std::to_string(value); }
[[nodiscard]] std::string toStorage(double value) { return std::to_string(value); }

[[nodiscard]] std::string fromStorage(const std::string& text, const std::string&) {
    return text;
}

[[nodiscard]] bool fromStorage(const std::string& text, bool fallback) {
    // Accept Cog's plist forms as well as our own, so an imported preferences
    // file reads correctly rather than silently falling back.
    if (text == "true" || text == "YES" || text == "1") return true;
    if (text == "false" || text == "NO" || text == "0") return false;
    return fallback;
}

[[nodiscard]] int fromStorage(const std::string& text, int fallback) {
    int        value = 0;
    const auto end   = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    return (result.ec == std::errc{} && result.ptr == end) ? value : fallback;
}

[[nodiscard]] double fromStorage(const std::string& text, double fallback) {
    try {
        std::size_t consumed = 0;
        const double value   = std::stod(text, &consumed);
        return (consumed == text.size()) ? value : fallback;
    } catch (...) {
        return fallback;
    }
}

class MemorySettingsStore final : public ISettingsStore {
public:
    [[nodiscard]] std::optional<std::string> getRaw(std::string_view key) const override {
        std::lock_guard lock(mutex_);
        const auto      it = values_.find(std::string{key});
        return (it == values_.end()) ? std::nullopt : std::optional{it->second};
    }

    void setRaw(std::string_view key, std::string_view value) override {
        std::lock_guard lock(mutex_);
        values_[std::string{key}] = std::string{value};
    }

    void remove(std::string_view key) override {
        std::lock_guard lock(mutex_);
        values_.erase(std::string{key});
    }

private:
    mutable std::mutex                 mutex_;
    std::map<std::string, std::string> values_;
};

}  // namespace

std::unique_ptr<ISettingsStore> makeMemorySettingsStore() {
    return std::make_unique<MemorySettingsStore>();
}

// --- accessors ------------------------------------------------------------

#define XPCOG_SETTING(Ident, Type, Key, Default)                    \
    Type Settings::Ident() const {                                  \
        const Type fallback = Type(Default);                        \
        if (const auto raw = store_.getRaw(Key)) {                  \
            return fromStorage(*raw, fallback);                     \
        }                                                           \
        return fallback;                                            \
    }                                                               \
    void Settings::set##Ident(const Type& value) {                  \
        store_.setRaw(Key, toStorage(value));                       \
    }
#include "xpcog/core/settings.def"
#undef XPCOG_SETTING

std::span<const Settings::Desc> Settings::all() noexcept {
#define XPCOG_SETTING(Ident, Type, Key, Default) +1
    constexpr std::size_t kCount = 0
#include "xpcog/core/settings.def"
        ;
#undef XPCOG_SETTING

    static const std::array<Desc, kCount> kDescriptors = {{
#define XPCOG_SETTING(Ident, Type, Key, Default) \
    Desc{#Ident, Key, #Type, #Default},
#include "xpcog/core/settings.def"
#undef XPCOG_SETTING
    }};
    return kDescriptors;
}

std::string Settings::defaultValue(std::string_view key) {
    for (const Desc& descriptor : all()) {
        if (descriptor.key != key) {
            continue;
        }
        std::string text{descriptor.defaultValue};
        // Stringifying the macro argument keeps the quotes around a string
        // literal, so "albumGainWithPeak" arrives with them attached.
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
            text = text.substr(1, text.size() - 2);
        }
        return text;
    }
    return {};
}

std::string Settings::rawValue(std::string_view key) const {
    if (auto stored = store_.getRaw(key)) {
        return *stored;
    }
    return defaultValue(key);
}

void Settings::setRawValue(std::string_view key, std::string_view value) {
    store_.setRaw(key, value);
}

void Settings::resetAll() {
    for (const Desc& descriptor : all()) {
        store_.remove(descriptor.key);
    }
}

void Settings::applyMigrations() {
    // Versioned rather than ad-hoc, so a migration runs exactly once even if the
    // user downgrades and upgrades again.
    struct Migration {
        int  toVersion;
        void (*apply)(ISettingsStore&);
    };

    static constexpr Migration kMigrations[] = {
        // v1: Cog stored volume as a percentage; the engine wants linear 0..1.
        {1, [](ISettingsStore& store) {
             if (const auto raw = store.getRaw("volume")) {
                 const double value = fromStorage(*raw, 1.0);
                 if (value > 1.0) {
                     store.setRaw("volume", toStorage(value / 100.0));
                 }
             }
         }},
        // v2: the equaliser gained an on/off switch, and its default is off --
        // Cog's default, for a setting that is Cog's. That is right for somebody
        // installing XPCog today and wrong for everybody already running it,
        // because until this existed a non-flat curve was simply always on. Left
        // alone, the upgrade would have silently killed every curve anyone had
        // built.
        //
        // So: a stored curve that is not flat turns the switch on, once. A flat
        // one does not, because a flat equaliser is skipped either way and
        // enabling it would only be a checkbox appearing ticked for no reason.
        //
        // Found by adding the setting and asking what it does to a settings file
        // that predates it, which is a question every default needs asked of it
        // and this one answered badly.
        {2, [](ISettingsStore& store) {
             // Untouched if the user already has an opinion, which is the case
             // for a settings file that has been through a Cog import.
             if (store.getRaw("GraphicEQenable")) {
                 return;
             }
             // By prefix, over the declared list, rather than against a copy of
             // the 31 band names -- the same reason the engine reads them that
             // way. `eqPreamp` and the bands all start "eq"; nothing else does.
             for (const Desc& descriptor : all()) {
                 if (!descriptor.key.starts_with("eq")) {
                     continue;
                 }
                 const auto raw = store.getRaw(descriptor.key);
                 if (raw && fromStorage(*raw, 0.0) != 0.0) {
                     store.setRaw("GraphicEQenable", toStorage(true));
                     return;
                 }
             }
         }},
    };

    int version = SettingsSchemaVersion();
    for (const Migration& migration : kMigrations) {
        if (version < migration.toVersion) {
            migration.apply(store_);
            version = migration.toVersion;
        }
    }
    setSettingsSchemaVersion(version);
    store_.sync();
}

}  // namespace xpcog
