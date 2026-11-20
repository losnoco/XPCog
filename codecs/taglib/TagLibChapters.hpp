// Chapter reading and writing through TagLib, shared by the reader and the
// writer. The behaviour is Cog's (Plugins/TagLib/TagLibMetadataWriter.m on
// TagLib 2.3.2): ID3v2 CHAP frames in the top-level CTOC's order, Matroska's
// default edition, MP4's QuickTime list preferred over its Nero one; on write,
// ordered by start, each chapter ending where the next begins and the last
// where the file does, identities kept so a renamed chapter stays itself.

#pragma once

#include "xpcog/core/Chapters.hpp"

#include <fileref.h>
#include <tfile.h>

#include <cstdint>

namespace xpcog {

/// Whether this container holds chapters at all: an ID3v2 tag is reachable
/// (MP3, WAV, AIFF, DSF, DSDIFF, TrueAudio), or it is MP4 or Matroska.
[[nodiscard]] bool tagLibSupportsChapters(const TagLib::File* file);

/// Cheap presence check, for the reader's title->album retitling of a file
/// whose chapters make it an album of them.
[[nodiscard]] bool hasTagLibChapters(TagLib::File* file);

/// The file's chapters. The FileRef must have been opened with audio
/// properties: the last MP4 chapter ends at the file's length.
[[nodiscard]] ChapterList readTagLibChapters(TagLib::FileRef& file);

/// Replaces the file's chapters with `chapters`, normalized first: stable
/// order by start, each ending where the next begins and the last where the
/// file does (`lengthMs`, clamped to the start when the length is unknown).
/// An empty list removes them. Applied to the open file before it is saved.
void writeTagLibChapters(TagLib::File* file, const ChapterList& chapters,
                         std::uint64_t lengthMs);

}  // namespace xpcog
