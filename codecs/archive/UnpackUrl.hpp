// Addressing a file inside an archive.
//
// Cog's format, reproduced exactly:
//
//     unpack://fex|<length>|<archive path>|<member path>
//
// where <length> counts the characters of the archive path. The length prefix is
// what makes the format work at all: both halves are arbitrary filesystem paths
// and either may contain the separator, so there is no character that could
// delimit them. Counting says where the first one ends and nothing has to be
// escaped.
//
// Two reasons to keep Cog's format rather than invent one:
//
//   * A Cog playlist can contain these URLs, and reading one should not turn its
//     archived entries into dead rows. Emitting the same format also means a
//     playlist written here still works if it goes back the other way.
//   * It leaves the fragment free. XPCog already uses `#n` for the subsong of a
//     cue track, a module or a game-music rip, and an archived NSF has both an
//     archive member *and* a track number. Putting the member in the path is
//     what keeps those two from colliding.
//
// The `fex` token names File_Extractor, which is Cog's archive reader and not
// ours -- this uses libarchive. It stays because it is part of a wire format
// shared with Cog, where changing it would break reading in both directions and
// buy nothing.

#pragma once

#include "xpcog/core/Url.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace xpcog::codecs {

struct UnpackTarget {
    /// Filesystem path of the archive itself.
    std::string archive;
    /// Path of the member within it, as the archive spells it.
    std::string member;

    [[nodiscard]] friend bool operator==(const UnpackTarget&,
                                         const UnpackTarget&) = default;
};

/// Reads an `unpack://` URL. nullopt for any other scheme, or a malformed one.
///
/// Percent-decodes before splitting, as Cog does, so the length always counts
/// decoded characters no matter how heavily the URL was encoded in storage.
[[nodiscard]] std::optional<UnpackTarget> parseUnpackUrl(const Url& url);

/// Builds one. `archive` is a filesystem path; `member` is the name the archive
/// gives the entry.
[[nodiscard]] Url makeUnpackUrl(const std::filesystem::path& archive,
                                std::string_view              member);

}  // namespace xpcog::codecs
