#include "xpcog/core/library/FolderArtwork.hpp"

#include "xpcog/core/FilePath.hpp"

#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>

namespace xpcog {
namespace {

/// In order of preference. `cover` is what the rippers write; `folder` is
/// Windows Media Player's and Explorer's; the rest are what people name the
/// file by hand.
constexpr std::array<std::string_view, 5> kStems = {"cover", "folder", "front", "album",
                                                    "albumart"};

/// The two formats every image handler on the three platforms reads. Nothing
/// looser, because a cover found and then not drawn is worse than one not
/// found: it is a blank pane where the fallback would have been a blank pane
/// with a reason.
constexpr std::array<std::string_view, 3> kExtensions = {"jpg", "jpeg", "png"};

/// ASCII only, on purpose. The names being matched are ASCII, so a file whose
/// name differs from them in a non-ASCII character is not a match however it
/// is folded.
[[nodiscard]] std::string asciiLower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

/// The rank of a candidate name, or npos for a file that is not one. Lower is
/// better: the stem decides first, the extension breaks the tie.
[[nodiscard]] std::size_t rankOf(const std::filesystem::path& name) {
    const std::string stem      = asciiLower(pathToUtf8(name.stem()));
    std::string       extension = asciiLower(pathToUtf8(name.extension()));
    if (extension.empty() || extension.front() != '.') {
        return std::string::npos;
    }
    extension.erase(0, 1);

    std::size_t stemRank = std::string::npos;
    for (std::size_t i = 0; i < kStems.size(); ++i) {
        if (stem == kStems[i]) {
            stemRank = i;
            break;
        }
    }
    if (stemRank == std::string::npos) {
        return std::string::npos;
    }
    for (std::size_t i = 0; i < kExtensions.size(); ++i) {
        if (extension == kExtensions[i]) {
            return stemRank * kExtensions.size() + i;
        }
    }
    return std::string::npos;
}

}  // namespace

std::optional<std::filesystem::path> findFolderArtwork(
    const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::directory_iterator it{directory, error};
    if (error) {
        return std::nullopt;
    }

    std::optional<std::filesystem::path> best;
    std::size_t                          bestRank = std::string::npos;
    for (const auto& entry : it) {
        // is_regular_file() rather than !is_directory(): a dangling symlink
        // named cover.jpg is neither, and would be found and then fail to
        // open.
        if (!entry.is_regular_file(error)) {
            continue;
        }
        const std::size_t rank = rankOf(entry.path().filename());
        if (rank < bestRank) {
            bestRank = rank;
            best     = entry.path();
        }
    }
    return best;
}

}  // namespace xpcog
