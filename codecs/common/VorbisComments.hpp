// Vorbis-comment tag handling, shared by FLAC, Ogg Vorbis and Opus.
//
// In Cog this logic is duplicated across FlacDecoder.m, VorbisDecoder.m and
// OpusDecoder.m, including the special cases below. Sharing it here means the
// three formats cannot drift apart in how they name tags.

#pragma once

#include "xpcog/core/MetadataMap.hpp"
#include "xpcog/core/TrackProperties.hpp"

#include <string_view>

namespace xpcog::codecs {

/// Applies one "NAME=value" comment to `tags` / `gain`, honouring the special
/// cases Cog implements:
///   * replaygain_* populate ReplayGainInfo rather than becoming visible tags
///   * "unsynced lyrics" and "lyrics" both normalise to "unsyncedlyrics"
///   * "comments:itunnorm" becomes "soundcheck"
///   * waveformatextensible_channel_mask overrides the channel layout
///
/// `channelConfig` is set only when the comment carries a channel mask.
/// Returns false for a malformed comment with no '='.
bool applyVorbisComment(std::string_view comment, MetadataMap& tags,
                        ReplayGainInfo& gain, std::uint32_t* channelConfig);

/// Decodes a METADATA_BLOCK_PICTURE payload (base64 in Ogg, raw in FLAC) far
/// enough to extract the embedded image. Returns empty on a malformed block.
[[nodiscard]] std::vector<std::byte> parsePictureBlock(std::string_view base64);

}  // namespace xpcog::codecs
