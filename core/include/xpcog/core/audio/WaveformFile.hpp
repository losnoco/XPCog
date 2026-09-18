// The on-disk form of a WaveformSummary.
//
// Fixed layout, little-endian, no compression: a complete entry is 36 bytes of
// header and two bytes a bucket, 2084 bytes at kWaveformBuckets, and a zlib
// dependency would cost more than it could save. The source file's stamp is
// repeated in the header although it is already in the cache's file name, so a
// decoded entry can say what it was made from and a stale one can be told
// apart from a wrong one by reading it.
//
//   off  size  field
//     0     4  magic "XPWF"
//     4     1  version, 1
//     5     1  layout: 1 = peak and rms, one byte each
//     6     2  reserved, zero
//     8     4  bucket count N
//    12     8  duration in seconds, IEEE double
//    20     8  source modification time (PluginCache::Stamp::modifiedSeconds)
//    28     8  source size in bytes    (PluginCache::Stamp::sizeBytes)
//    36     N  peak
//  36+N     N  rms
//
// Only complete summaries are written. A partial one is a moment in an
// analysis, not a fact about a track.

#pragma once

#include "xpcog/core/audio/Waveform.hpp"
#include "xpcog/core/library/PluginCache.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace xpcog {

struct WaveformRecord {
    WaveformSummary    summary;
    PluginCache::Stamp stamp;
};

/// The bytes for `summary`, which must be complete.
[[nodiscard]] std::vector<std::byte> encodeWaveform(const WaveformSummary&    summary,
                                                    const PluginCache::Stamp& stamp);

/// The record in `bytes`, or nothing for anything that is not one: wrong magic,
/// a version or layout this build does not read, or a length that does not
/// match the bucket count it claims.
[[nodiscard]] std::optional<WaveformRecord> decodeWaveform(std::span<const std::byte> bytes);

/// Bytes a complete summary of `buckets` occupies.
[[nodiscard]] constexpr std::size_t waveformFileSize(std::uint32_t buckets) noexcept {
    return 36 + (2 * static_cast<std::size_t>(buckets));
}

}  // namespace xpcog
