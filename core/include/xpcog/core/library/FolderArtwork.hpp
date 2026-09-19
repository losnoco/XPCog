// The cover a folder keeps beside its tracks.
//
// Half the FLAC rips in existence carry no embedded picture at all and a
// `cover.jpg` in the folder instead -- it is what EAC, dBpoweramp and most
// rippers write by default, and it is the one copy an album of twelve tracks
// needs. Cog reads only embedded art (its readers hand back an `albumArt` tag
// and nothing looks at the folder), so those albums play there with a blank
// Info pane. This is the other half.
//
// The rule is a fixed list of names in a fixed order, matched without regard
// to case -- `Folder.jpg` is what Windows Media Player writes and `Cover.JPG`
// is what a camera does -- and nothing looser: a folder scanned for "any image"
// hands back the scan of the liner notes, or a photo, as the album's cover.
// Embedded art wins where both exist, because the picture inside the file is
// the one somebody put there on purpose for that file; the folder's is the
// fallback. That decision is the Scanner's, and this only answers the question
// of which file, if any, is the folder's cover.
//
// Toolkit-free and image-format-agnostic: the bytes are handed on exactly as
// the embedded art would be, and whatever draws them decides whether they are
// a picture.

#pragma once

#include <filesystem>
#include <optional>

namespace xpcog {

/// The image file that stands for `directory`'s album, or nullopt when there
/// is none. Enumerates the directory once; a directory that cannot be read is
/// a directory with no cover.
///
/// Preference is by name first -- `cover`, `folder`, `front`, `album`,
/// `albumart` -- then by extension, `jpg` before `jpeg` before `png`, so two
/// candidates in one folder always resolve the same way.
[[nodiscard]] std::optional<std::filesystem::path> findFolderArtwork(
    const std::filesystem::path& directory);

}  // namespace xpcog
