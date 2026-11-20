// Chapters as the tag editor reads and writes them: a typed side channel
// beside MetadataMap, never a value inside it. A map entry is a tag -- it
// promotes into PlaylistEntry's columns and persists in the library -- while
// chapters are the file's structure: they expand into playlist rows of their
// own (FFmpegContainer's "#N" URLs) and are read fresh from the file whenever
// the editor opens. Keeping them out of the map is what keeps them out of
// entry_tag, the REST metadata answers and the CLI's tag output, as Cog keeps
// them out of the playlist's stored metadata.
//
// Times are milliseconds, the unit the container APIs convert to and from:
// ID3v2 CHAP stores ms directly, Matroska stores ns, MP4's chapter lists ms.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xpcog {

struct Chapter {
    std::uint64_t startMs = 0;
    /// Where the chapter ends. Derived on write -- the next chapter's start,
    /// the last one the file's length -- so a reader's value is informational
    /// and a writer normalizes it away.
    std::uint64_t endMs = 0;
    std::string title;
    /// Opaque identity kept across saves so a chapter stays the same chapter
    /// when its title or time is edited: the ID3v2 element ID's bytes
    /// (Latin-1 text) or the Matroska ChapterUID in decimal. Empty means
    /// "mint one on write". MP4 chapters carry no identity.
    std::string id;

    [[nodiscard]] friend bool operator==(const Chapter&, const Chapter&) = default;
};

using ChapterList = std::vector<Chapter>;

}  // namespace xpcog
