// Settings and paths for Linux, and for any platform with no native mechanism of
// its own.
//
// Both locations are the ones QSettings and QStandardPaths::AppDataLocation used,
// organisation segment included:
//
//   $XDG_CONFIG_HOME/LoSnoCo/XPCog.conf
//   $XDG_DATA_HOME/LoSnoCo/XPCog/library.db
//
// with the specified fallbacks to ~/.config and ~/.local/share. Getting the
// segment right is not cosmetic: an existing installation has files at these
// paths, and landing one directory over would present a factory-fresh player to
// someone who had one configured, with nothing to explain it.
//
// The file format is FileSettingsStore's, which is deliberately close enough to
// what QSettings wrote that an existing file is read rather than ignored. A
// `[General]` line is emitted first and section lines are skipped on the way in,
// which is the whole of the difference for flat scalar keys -- and every key in
// settings.def is one. This is not a general INI implementation and does not try
// to be; it is a best-effort so nobody's volume and equaliser reset on upgrade.

#include "xpcog/platform/SettingsStore.hpp"

#include "FileSettingsStore.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace xpcog::platform {
namespace {

/// `$name`, or empty when unset or empty. An empty XDG variable is specified to
/// mean "unset", which getenv alone does not tell you.
[[nodiscard]] std::string environment(const char* name) {
    const char* value = std::getenv(name);
    return (value != nullptr && *value != '\0') ? std::string{value} : std::string{};
}

[[nodiscard]] std::filesystem::path home() {
    const std::string value = environment("HOME");
    return value.empty() ? std::filesystem::path{"."} : std::filesystem::path{value};
}

[[nodiscard]] std::filesystem::path configHome() {
    const std::string value = environment("XDG_CONFIG_HOME");
    return value.empty() ? home() / ".config" : std::filesystem::path{value};
}

[[nodiscard]] std::filesystem::path dataHome() {
    const std::string value = environment("XDG_DATA_HOME");
    return value.empty() ? home() / ".local" / "share" : std::filesystem::path{value};
}

}  // namespace

std::unique_ptr<ISettingsStore> makeNativeSettingsStore() {
    const std::filesystem::path file = configHome() / "LoSnoCo" / "XPCog.conf";
    return std::make_unique<FileSettingsStore>(file.string());
}

std::unique_ptr<ISettingsStore> makeFileSettingsStore(const std::string& path) {
    return std::make_unique<FileSettingsStore>(path);
}

std::string libraryDatabasePath() {
    const std::filesystem::path directory = dataHome() / "LoSnoCo" / "XPCog";

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);

    return (directory / "library.db").string();
}

}  // namespace xpcog::platform
