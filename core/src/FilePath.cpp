#include "xpcog/core/FilePath.hpp"

namespace xpcog {
namespace {

/// char8_t exists to make "these bytes are UTF-8" a type rather than a comment,
/// which is exactly the distinction this file is about -- but it is a distinct
/// type from char, and every API on either side of here speaks char. The two
/// casts below are that gap and nothing more: same size, same object
/// representation, different name.
[[nodiscard]] std::string narrow(const std::u8string& text) {
    return std::string{text.begin(), text.end()};
}

}  // namespace

std::string pathToUtf8(const std::filesystem::path& path) {
    return narrow(path.u8string());
}

std::string pathToUtf8Generic(const std::filesystem::path& path) {
    return narrow(path.generic_u8string());
}

std::filesystem::path pathFromUtf8(std::string_view text) {
    // Constructed from char8_t, which is what tells std::filesystem the bytes
    // are UTF-8 instead of whatever the platform would have assumed.
    return std::filesystem::path{std::u8string{text.begin(), text.end()}};
}

}  // namespace xpcog
