#include "xpcog/core/AssetPath.hpp"

#include "xpcog/core/FilePath.hpp"

#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace xpcog {
namespace {

/// The running executable's own path.
///
/// Two platforms, two APIs, and each has a way of failing that the obvious
/// call does not report. GetModuleFileNameW truncates rather than failing when
/// the buffer is too small, and only says so through GetLastError; and
/// /proc/self/exe is a link whose target can be longer than any guess.
[[nodiscard]] std::filesystem::path executablePath() {
#if defined(_WIN32)
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD written =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size()) {
            return std::filesystem::path{
                std::wstring{buffer.data(), static_cast<std::size_t>(written)}};
        }
        if (buffer.size() >= 64 * 1024) {
            return {};  // a path this long is a fault, not a deep directory
        }
        buffer.resize(buffer.size() * 2);
    }
#else
    std::error_code error;
    std::filesystem::path resolved =
        std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path{} : resolved;
#endif
}

/// Where assets sit relative to the executable, most specific first.
[[nodiscard]] std::vector<std::filesystem::path> candidates() {
    const std::filesystem::path exe = executablePath();
    if (exe.empty()) {
        return {};
    }
    const std::filesystem::path dir = exe.parent_path();

#if defined(_WIN32)
    return {dir};
#else
    // An installed layout puts the binary in <prefix>/bin and its data in
    // <prefix>/share/xpcog. The build tree has neither, and finds them beside
    // the binary because CMake copies them there.
    return {dir, dir / ".." / "share" / "xpcog"};
#endif
}

}  // namespace

std::filesystem::path executableDirectory() {
    const std::filesystem::path exe = executablePath();
    return exe.empty() ? std::filesystem::path{} : exe.parent_path();
}

std::filesystem::path assetDirectory() {
    const auto paths = candidates();
    if (paths.empty()) {
        return {};
    }

    // The first that exists, so a bundle's Resources wins over a stray copy
    // beside the binary. Falls back to the first candidate when none exists, so
    // a caller that wants to report a path has one to report.
    std::error_code error;
    for (const std::filesystem::path& path : paths) {
        if (std::filesystem::is_directory(path, error)) {
            return path.lexically_normal();
        }
    }
    return paths.front().lexically_normal();
}

std::filesystem::path assetPath(std::string_view name) {
    if (name.empty()) {
        return {};
    }
    const std::filesystem::path relative = pathFromUtf8(name);

    std::error_code error;
    for (const std::filesystem::path& base : candidates()) {
        const std::filesystem::path full = (base / relative).lexically_normal();
        if (std::filesystem::is_regular_file(full, error)) {
            return full;
        }
    }
    return {};
}

}  // namespace xpcog
