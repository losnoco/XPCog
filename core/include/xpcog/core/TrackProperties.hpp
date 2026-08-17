// Replaces the properties NSDictionary that Cog decoders return.
//
// A struct rather than a map: the key set is closed and every consumer in Cog
// switches over it (Playlist/PlaylistEntry.m:634-676). Verified against
// Plugins/Flac/FlacDecoder.m:525-536.

#pragma once

#include "xpcog/core/AudioFormat.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace xpcog {

/// Cog stores these as separate properties; grouped here because they are always
/// read together by ConverterNode's volume scaling.
struct ReplayGainInfo {
    std::optional<float>       trackGain;  ///< dB
    std::optional<float>       trackPeak;  ///< linear, 1.0 == full scale
    std::optional<float>       albumGain;  ///< dB
    std::optional<float>       albumPeak;
    std::optional<float>       volume;      ///< Cog's "volume" property
    std::optional<std::string> soundcheck;  ///< iTunes iTunNORM payload

    [[nodiscard]] bool empty() const noexcept {
        return !trackGain && !trackPeak && !albumGain && !albumPeak && !volume &&
               !soundcheck;
    }

    [[nodiscard]] friend bool operator==(const ReplayGainInfo&,
                                         const ReplayGainInfo&) = default;
};

struct TrackProperties {
    AudioFormat  format;
    std::int64_t totalFrames = 0;
    /// Average bitrate in kbps; 0 when unknown.
    std::int32_t bitrateKbps = 0;
    bool         seekable    = false;

    /// Cog derives this from encoding == "lossless" and uses it to gate the
    /// lossless indicator and HDCD detection.
    bool lossless = false;

    std::string codec;     ///< "FLAC"
    std::string encoding;  ///< "lossless" | "lossy" | "synthesized"

    /// Embedded cue sheet, if the container carried one.
    std::optional<std::string> cuesheet;

    ReplayGainInfo replayGain;

    /// Duration in seconds, or 0 when the length is unknown (live streams).
    [[nodiscard]] double duration() const noexcept {
        return (format.sampleRate > 0.0)
                   ? static_cast<double>(totalFrames) / format.sampleRate
                   : 0.0;
    }

    [[nodiscard]] friend bool operator==(const TrackProperties&,
                                         const TrackProperties&) = default;
};

}  // namespace xpcog
