// Replaces NSURL in the plugin API.
//
// Cog's CogNormalizeURL() handles macOS file-reference URLs (file:///.file/id=),
// which no other platform produces; that concept is gone. Fragments are load-bearing
// here as in Cog: they carry cue-sheet track and subsong indices.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace xpcog {

class Url {
public:
    Url() = default;

    /// Parses an absolute URL. Returns nullopt if there is no scheme or it is
    /// malformed. Input that looks like a bare path is rejected -- use
    /// fromLocalPath() for that, so callers stay explicit about intent.
    [[nodiscard]] static std::optional<Url> parse(std::string_view text);

    /// Builds a file:// URL. The path is made absolute and lexically normal.
    [[nodiscard]] static Url fromLocalPath(const std::filesystem::path& path);

    /// The same from UTF-8 text. Sources here are compiled as UTF-8 on every
    /// platform, so a literal is UTF-8 too.
    [[nodiscard]] static Url fromLocalPath(const char* utf8);

    /// Deleted, because they would have compiled and been wrong.
    ///
    /// Everything above this layer keeps paths as UTF-8 in a std::string --
    /// QString::toStdString(), a playlist file's lines, a URL body. Handing one
    /// to std::filesystem::path lets it read those bytes in the platform's
    /// *narrow* encoding, which on Windows is the active code page, and the
    /// implicit conversion made that silent: a folder named "Björk - Post"
    /// reached the scanner as "BjÃ¶rk - Post" and every track in it read as
    /// unopenable. Say which it is -- fromLocalPath(pathFromUtf8(text)) -- so
    /// the next person reading the call knows without checking.
    static Url fromLocalPath(const std::string&) = delete;
    static Url fromLocalPath(std::string_view)   = delete;

    /// Lowercase, no trailing colon: "file", "http", "zip".
    [[nodiscard]] std::string_view scheme() const noexcept { return scheme_; }

    /// Everything after '#', still percent-encoded. Empty when absent.
    [[nodiscard]] std::string_view fragment() const noexcept { return fragment_; }

    /// Lowercase, no dot, fragment-stripped. Empty when there is no extension.
    [[nodiscard]] std::string extension() const;

    /// Filesystem path for file:// URLs, percent-decoded, with native separators.
    /// nullopt for every other scheme.
    [[nodiscard]] std::optional<std::filesystem::path> localPath() const;

    /// Canonical serialization. This is what gets stored in the library database,
    /// so it must round-trip through parse().
    [[nodiscard]] std::string toString() const;

    [[nodiscard]] Url withFragment(std::string_view fragment) const;
    [[nodiscard]] Url withoutFragment() const;

    [[nodiscard]] bool empty() const noexcept { return scheme_.empty(); }

    [[nodiscard]] friend bool operator==(const Url&, const Url&) = default;

private:
    std::string scheme_;
    /// Scheme-specific part, percent-encoded, fragment excluded.
    std::string body_;
    std::string fragment_;
};

/// Percent-decoding helpers, exposed for tests and for source implementations
/// that need to build request paths.
[[nodiscard]] std::string percentDecode(std::string_view text);
[[nodiscard]] std::string percentEncodePath(std::string_view text);

}  // namespace xpcog
