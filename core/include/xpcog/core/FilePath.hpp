// Filesystem paths as text, without losing anything on the way.
//
// std::filesystem::path stores the platform's native form: bytes on POSIX,
// UTF-16 on Windows. Its conversions to and from std::string go through the
// *native narrow encoding*, which on Windows is the active code page -- so
//
//     std::string text = path.string();          // wide -> CP-1252, lossy
//     std::filesystem::path back{text};          // CP-1252 -> wide
//
// silently mangles any name the code page cannot spell, and the pair only looks
// symmetric because it usually round-trips. Everything above this layer speaks
// UTF-8: QString::toStdString() produces it, playlist files and the library
// database store it, and a URL's percent-encoded bytes have to be it or the
// library is unreadable on the next machine. So the conversion has to say UTF-8
// rather than take the platform's word for it.
//
// This cost a bug: a folder named "Björk - Post" reached the scanner as
// "BjÃ¶rk - Post", named nothing that exists, and every track in it showed as
// unreadable. See tests/core/test_paths.cpp.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace xpcog {

/// A path's UTF-8 bytes, on every platform.
[[nodiscard]] std::string pathToUtf8(const std::filesystem::path& path);

/// The same with forward slashes, for the forms that are read on a different
/// platform than they were written: URLs, playlist files, the library database.
[[nodiscard]] std::string pathToUtf8Generic(const std::filesystem::path& path);

/// A path from UTF-8 bytes, on every platform.
[[nodiscard]] std::filesystem::path pathFromUtf8(std::string_view text);

}  // namespace xpcog
