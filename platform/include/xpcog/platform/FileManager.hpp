// Handing a file back to the desktop: showing it where it lives, and throwing it
// away.
//
// Two of Cog's playlist commands -- "Show in Finder" and "Move to Trash" -- are
// the only places the player asks the operating system to do something with a
// file it is not going to play. Both are one call on macOS and neither is one
// call anywhere else, which is exactly the shape this layer exists for.
//
// Trashing is **reversible deletion and nothing else**. Where the desktop has no
// trash, or the file is on a volume that has none, this returns false rather than
// falling back to unlink(): a command labelled "Move to Trash" that silently
// destroys the file instead is the worst possible way to be helpful. The caller
// says so and leaves the file alone.
//
// Both take a filesystem path rather than a Url, because both are meaningless for
// anything else -- there is no folder to open for an http:// stream, and nothing
// to move to a trash. Deciding which entries qualify is the caller's, and it has
// the Url to decide from.

#pragma once

#include <filesystem>

namespace xpcog::platform {

/// Opens the desktop's file manager showing `path`, selected within its folder.
///
/// Selected rather than merely opened: the Finder, Explorer and the
/// freedesktop.org file managers all distinguish "open this folder" from "point
/// at this file", and it is the second that answers "where did this track come
/// from" when the folder holds two hundred of them.
///
/// Returns false when there is no file manager to ask, or when it refused.
bool revealInFileManager(const std::filesystem::path& path);

/// Moves `path` to the desktop's trash or recycle bin.
///
/// Returns false when the platform has no trash, the volume has none, or the
/// move failed. Never deletes outright -- see the header comment.
bool moveToTrash(const std::filesystem::path& path);

}  // namespace xpcog::platform
