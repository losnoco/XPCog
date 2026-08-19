// A file presented the way a live stream presents itself.
//
// The distinction matters more than it looks: HttpSource answers seekable()
// false for a response with no length, while still allowing a small rewind out
// of its ring. Decoders read both answers and behave differently -- chained Ogg
// decoding is switched on by the first, and the four-byte container peek depends
// on the second -- so a test source that refused every seek would exercise
// neither path, and one that claimed to be seekable would be a file.

#pragma once

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/Url.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog::test {

class StreamingFileSource final : public ISource {
public:
    /// Registrable form: the file comes from the URL, so this can be given a
    /// scheme of its own and reached through PluginRegistry::open() -- which is
    /// what lets a test drive the real decoder selection, MultiDecoder included,
    /// over a source that says it cannot seek.
    StreamingFileSource() = default;

    explicit StreamingFileSource(const std::filesystem::path& path) { load(path); }

    bool open(const Url& url) override {
        url_ = url;
        if (bytes_.empty()) {
            load(pathOf(url));
        }
        return !bytes_.empty();
    }

    [[nodiscard]] bool seekable() const override { return false; }

    bool seek(std::int64_t offset, int whence) override {
        if (whence == SEEK_END) {
            return false;  // no length to measure against
        }
        const std::int64_t target = (whence == SEEK_CUR) ? position_ + offset : offset;
        if (target < 0 || target > static_cast<std::int64_t>(bytes_.size())) {
            return false;
        }
        position_ = target;
        return true;
    }

    [[nodiscard]] std::int64_t tell() const override { return position_; }

    std::int64_t read(void* out, std::int64_t wanted) override {
        const auto available = static_cast<std::int64_t>(bytes_.size()) - position_;
        const auto take      = std::min(wanted, available);
        if (take <= 0) {
            return 0;
        }
        std::memcpy(out, bytes_.data() + position_, static_cast<std::size_t>(take));
        position_ += take;
        return take;
    }

    void close() override {}
    [[nodiscard]] const Url& url() const override { return url_; }

private:
    void load(const std::filesystem::path& path) {
        std::FILE* f = std::fopen(path.string().c_str(), "rb");
        if (f == nullptr) {
            return;
        }
        std::uint8_t buffer[8192];
        std::size_t  got = 0;
        while ((got = std::fread(buffer, 1, sizeof(buffer), f)) > 0) {
            bytes_.insert(bytes_.end(), buffer, buffer + got);
        }
        std::fclose(f);
    }

    /// The URL's body as a filesystem path, whatever its scheme. The empty
    /// authority, the percent-encoding and -- on Windows -- the slash before the
    /// drive letter all need undoing, all three of which Url::fromLocalPath()
    /// put there. Url::localPath() does exactly this and is not usable here: it
    /// answers only for scheme "file", and the point of this source is to be
    /// reached under a scheme of its own.
    [[nodiscard]] static std::filesystem::path pathOf(const Url& url) {
        const std::string text = url.toString();
        const std::size_t colon = text.find(':');
        if (colon == std::string::npos) {
            return {};
        }
        std::string_view body{text};
        body.remove_prefix(colon + 1);
        while (body.starts_with("//")) {
            body.remove_prefix(1);
        }
        std::string decoded = percentDecode(body);
#ifdef _WIN32
        // "/C:/music" -> "C:/music". Without this every fixture opened through a
        // URL rather than a path failed to load, and a source that reads no
        // bytes looks exactly like a decoder that cannot open the file.
        if (decoded.size() >= 3 && decoded[0] == '/' &&
            (std::isalpha(static_cast<unsigned char>(decoded[1])) != 0) &&
            decoded[2] == ':') {
            decoded.erase(0, 1);
        }
#endif
        return std::filesystem::path{decoded};
    }

    std::vector<std::uint8_t> bytes_;
    std::int64_t              position_ = 0;
    Url                       url_;
};

}  // namespace xpcog::test
