// A settings store backed by one flat text file.
//
// Linux's native store, and the store `makeFileSettingsStore()` hands back on
// every platform -- which is what lets a test round-trip real settings without
// touching the machine's registry or preference domain.
//
// The format is one `key=value` line per setting, UTF-8, under a single
// `[General]` header. The header is there for one reader that is not this one:
// QSettings puts ungrouped keys under [General] and declines a file without it,
// so writing the line -- and skipping section lines on the way in -- is what lets
// an existing Linux configuration be inherited rather than quietly discarded.
//
// One section is enough because every key in settings.def is a flat ASCII
// identifier; see SettingsStore.hpp, which is also where the promise to keep them
// that way lives. This is not a general INI implementation and does not try to be.
//
// Values *are* escaped, and one of them needs it: `UserDefaultURLsKey` holds the
// URL history newline-separated, so a raw write would turn one setting into
// fifteen unparseable lines and lose the lot. Backslash, newline and carriage
// return become `\\`, `\n` and `\r`; nothing else is touched, so a path with
// spaces or a colour with a `#` reads back exactly as written.

#pragma once

#include "xpcog/core/Settings.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace xpcog::platform {

class FileSettingsStore final : public ISettingsStore {
public:
    /// Reads `path` if it exists. A missing or unreadable file is an empty store
    /// rather than an error: first launch is the common case, and a store that
    /// refused to start would take the application with it.
    explicit FileSettingsStore(std::string path);

    /// Writes anything still pending. A store whose values never reached disk
    /// because nobody happened to call sync() would be a setting that silently
    /// forgets.
    ~FileSettingsStore() override;

    [[nodiscard]] std::optional<std::string> getRaw(std::string_view key) const override;
    void setRaw(std::string_view key, std::string_view value) override;
    void remove(std::string_view key) override;
    void sync() override;

private:
    void load();

    std::string                        path_;
    std::map<std::string, std::string> values_;
    bool                               dirty_ = false;
};

}  // namespace xpcog::platform
