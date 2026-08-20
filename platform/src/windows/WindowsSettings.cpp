// Settings in the registry, and where the library lives, on Windows.
//
// The key is HKCU\Software\LoSnoCo\XPCog and the database is
// %APPDATA%\LoSnoCo\XPCog\library.db. Both are exactly where QSettings and
// QStandardPaths::AppDataLocation put them, and that is not a detail to get
// approximately right: an installation already has settings and a library at
// those paths, and a store that landed one directory over would present a
// factory-fresh player to someone who had one configured, with no error to
// explain it.
//
// The organisation segment is the part that is easy to lose. QSettings composes
// the key from the organisation *and* the application name, and AppDataLocation
// does the same with the directory -- so it is LoSnoCo\XPCog in both, not XPCog.
// The comment in the file this replaces said "%APPDATA%/XPCog" and was wrong;
// what settled it was reading the disk rather than the source.
//
// Everything is written as REG_SZ, because ISettingsStore's currency is strings
// and Settings does the parsing. Reads are more generous than writes: a DWORD or
// a QWORD is converted rather than refused, so a value someone set with regedit
// is understood instead of being silently treated as absent.

#include "xpcog/platform/SettingsStore.hpp"

#include "../common/FileSettingsStore.hpp"
#include "WinString.hpp"

#include <shlobj.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace xpcog::platform {
namespace {

constexpr const wchar_t* kSettingsKey = L"Software\\LoSnoCo\\XPCog";

class RegistrySettingsStore final : public ISettingsStore {
public:
    RegistrySettingsStore() {
        // Created on construction rather than on first write: the alternative is
        // every setter checking, and an application that starts is one that is
        // about to write something anyway.
        RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, nullptr, &key_,
                        nullptr);
    }

    ~RegistrySettingsStore() override {
        if (key_ != nullptr) {
            RegCloseKey(key_);
        }
    }

    [[nodiscard]] std::optional<std::string> getRaw(std::string_view key) const override {
        if (key_ == nullptr) {
            return std::nullopt;
        }
        const std::wstring name = toWide(key);

        DWORD type = 0;
        DWORD bytes = 0;
        if (RegQueryValueExW(key_, name.c_str(), nullptr, &type, nullptr, &bytes) !=
            ERROR_SUCCESS) {
            return std::nullopt;
        }

        std::vector<BYTE> buffer(bytes);
        if (RegQueryValueExW(key_, name.c_str(), nullptr, &type, buffer.data(), &bytes) !=
            ERROR_SUCCESS) {
            return std::nullopt;
        }

        switch (type) {
            case REG_SZ:
            case REG_EXPAND_SZ: {
                // `bytes` counts the terminator when there is one and does not
                // when the value was written without; trimming by search rather
                // than by arithmetic handles both.
                const auto*       text = reinterpret_cast<const wchar_t*>(buffer.data());
                const std::size_t chars = bytes / sizeof(wchar_t);
                std::wstring_view view{text, chars};
                if (const auto nul = view.find(L'\0'); nul != std::wstring_view::npos) {
                    view = view.substr(0, nul);
                }
                return toUtf8(view);
            }
            case REG_DWORD:
                if (bytes >= sizeof(DWORD)) {
                    DWORD value = 0;
                    std::memcpy(&value, buffer.data(), sizeof(value));
                    return std::to_string(value);
                }
                return std::nullopt;
            case REG_QWORD:
                if (bytes >= sizeof(std::uint64_t)) {
                    std::uint64_t value = 0;
                    std::memcpy(&value, buffer.data(), sizeof(value));
                    return std::to_string(value);
                }
                return std::nullopt;
            default:
                return std::nullopt;
        }
    }

    void setRaw(std::string_view key, std::string_view value) override {
        if (key_ == nullptr) {
            return;
        }
        const std::wstring name = toWide(key);
        const std::wstring wide = toWide(value);
        // The terminator is included in the byte count, as REG_SZ requires.
        RegSetValueExW(key_, name.c_str(), 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(wide.c_str()),
                       static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t)));
    }

    void remove(std::string_view key) override {
        if (key_ != nullptr) {
            RegDeleteValueW(key_, toWide(key).c_str());
        }
    }

    void sync() override {
        // The registry is written through on each call, so there is nothing
        // buffered to flush. RegFlushKey() exists and is deliberately not called:
        // it forces a disk commit of the whole hive and is documented as
        // something an application should not do routinely.
    }

private:
    HKEY key_ = nullptr;
};

/// %APPDATA%, or empty if the shell will not say.
[[nodiscard]] std::filesystem::path roamingAppData() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw))) {
        CoTaskMemFree(raw);
        return {};
    }
    std::filesystem::path path{raw};
    CoTaskMemFree(raw);
    return path;
}

}  // namespace

std::unique_ptr<ISettingsStore> makeNativeSettingsStore() {
    return std::make_unique<RegistrySettingsStore>();
}

std::unique_ptr<ISettingsStore> makeFileSettingsStore(const std::string& path) {
    return std::make_unique<FileSettingsStore>(path);
}

std::string libraryDatabasePath() {
    std::filesystem::path directory = roamingAppData();
    if (directory.empty()) {
        return "library.db";
    }
    directory /= L"LoSnoCo";
    directory /= L"XPCog";

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);

    return toUtf8((directory / L"library.db").wstring());
}

}  // namespace xpcog::platform
