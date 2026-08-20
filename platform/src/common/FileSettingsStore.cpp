#include "FileSettingsStore.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace xpcog::platform {
namespace {

[[nodiscard]] std::string escape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c; break;
        }
    }
    return out;
}

[[nodiscard]] std::string unescape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 == value.size()) {
            out += value[i];
            continue;
        }
        switch (value[++i]) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case '\\': out += '\\'; break;
            // An unknown escape keeps both characters rather than swallowing
            // one. Nothing this writes produces one, so reaching here means the
            // file was edited by hand, and losing a character silently is a
            // worse answer than reading it literally.
            default:
                out += '\\';
                out += value[i];
                break;
        }
    }
    return out;
}

}  // namespace

FileSettingsStore::FileSettingsStore(std::string path) : path_(std::move(path)) { load(); }

FileSettingsStore::~FileSettingsStore() {
    // Not through sync(): a throwing destructor is worse than a lost write, and
    // the stream below cannot throw with exceptions unset.
    if (dirty_) {
        sync();
    }
}

void FileSettingsStore::load() {
    std::ifstream in(std::filesystem::path{path_}, std::ios::binary);
    if (!in) {
        return;
    }

    std::string line;
    while (std::getline(in, line)) {
        // Written on Windows, read on Linux, or the reverse: the file is ours and
        // may travel with a home directory.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        // Section lines are skipped rather than parsed. Every key in settings.def
        // is flat, so there is exactly one section and its name carries nothing --
        // but a file QSettings wrote has a `[General]` line at the top, and
        // reading it as a key would be the difference between inheriting an
        // existing configuration and silently starting from defaults.
        if (line.empty() || line.front() == '#' || line.front() == '[') {
            continue;
        }
        const std::size_t split = line.find('=');
        if (split == std::string::npos) {
            continue;
        }
        values_.insert_or_assign(line.substr(0, split), unescape(std::string_view{line}.substr(split + 1)));
    }
}

std::optional<std::string> FileSettingsStore::getRaw(std::string_view key) const {
    const auto found = values_.find(std::string{key});
    if (found == values_.end()) {
        return std::nullopt;
    }
    return found->second;
}

void FileSettingsStore::setRaw(std::string_view key, std::string_view value) {
    auto [it, inserted] = values_.insert_or_assign(std::string{key}, std::string{value});
    (void)it;
    (void)inserted;
    dirty_ = true;
}

void FileSettingsStore::remove(std::string_view key) {
    if (values_.erase(std::string{key}) != 0) {
        dirty_ = true;
    }
}

void FileSettingsStore::sync() {
    if (!dirty_) {
        return;
    }

    const std::filesystem::path target{path_};
    std::error_code            ec;
    std::filesystem::create_directories(target.parent_path(), ec);

    // Written beside the target and renamed over it, so an interrupted write
    // leaves the previous settings intact rather than a truncated file. Settings
    // are small and rewritten whole, which is what makes this cheap enough to do
    // every time.
    const std::filesystem::path temporary = target.string() + ".new";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            return;
        }
        // For the reader that is not this one: QSettings puts ungrouped keys under
        // [General], and a file without the line is one it declines to read.
        out << "[General]\n";
        for (const auto& [key, value] : values_) {
            out << key << '=' << escape(value) << '\n';
        }
        if (!out) {
            return;
        }
    }

    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        // rename() across an existing file is fine on POSIX and on Windows since
        // it goes through MoveFileEx; if it failed anyway, the old file is still
        // the good one and the temporary is the casualty.
        std::filesystem::remove(temporary, ec);
        return;
    }
    dirty_ = false;
}

}  // namespace xpcog::platform
