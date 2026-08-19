// ID3v2, parsed from a block of bytes rather than from a file.
//
// This exists for tags that are not in a header. HLS carries its now-playing
// metadata as a complete ID3v2 tag per segment -- inline before the audio for a
// packed-audio rendition, or in a separate `AV_CODEC_ID_TIMED_ID3` elementary
// stream inside MPEG-TS. In the second case the tag arrives as the payload of an
// ordinary packet, and libavformat has no public API that will parse one:
// `ff_id3v2_read_dict` is internal, and the alternative -- opening a nested
// AVFormatContext over each packet -- is a demuxer per tag.
//
// So: a parser that takes bytes. Deliberately narrow. It reads what a station
// puts in a timed tag, which is text frames and comments, and skips the rest.
// PRIV in particular is skipped on purpose: every HLS transport-stream segment
// carries `com.apple.streaming.transportStreamTimestamp` in one, and surfacing
// that would rename the track once per segment forever.
//
// Not a replacement for TagLib, which reads tags from files and does the whole
// standard. This is for the streaming path, where TagLib has nothing to open.

#pragma once

#include "xpcog/core/MetadataMap.hpp"

#include <cstddef>
#include <span>

namespace xpcog::codecs {

/// Total length of the ID3v2 tag at the start of `data` -- header, body and
/// footer -- or 0 when `data` does not begin with one.
///
/// Answers "how much of this is not audio", which is what a format sniffer wants
/// and what a reader needs to step over consecutive tags.
[[nodiscard]] std::size_t id3v2TagLength(std::span<const std::byte> data);

/// Parses the ID3v2 tag at the start of `data` into `out`, replacing any values
/// for the keys it names and leaving the rest alone. Returns false when there is
/// no readable tag there.
///
/// Keys match the names FFmpeg's own ID3 reader produces (`TIT2` -> `title`,
/// `TPE1` -> `artist`, ...), so a tag parsed here merges with one the demuxer
/// harvested rather than sitting beside it under a different name.
bool parseId3v2(std::span<const std::byte> data, MetadataMap& out);

}  // namespace xpcog::codecs
