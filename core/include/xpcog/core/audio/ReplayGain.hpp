// ReplayGain volume scaling. Port of Cog Audio/Chain/ConverterNode.m
// -refreshVolumeScaling (lines 406-452).

#pragma once

#include "xpcog/core/TrackProperties.hpp"

#include <string_view>

namespace xpcog {

/// The `volumeScaling` setting, mirroring the strings Cog stores.
namespace VolumeScaling {
inline constexpr std::string_view kNone              = "none";
inline constexpr std::string_view kVolumeScale       = "volumeScale";
inline constexpr std::string_view kSoundcheck        = "soundcheck";
inline constexpr std::string_view kTrackGain         = "trackGain";
inline constexpr std::string_view kTrackGainWithPeak = "trackGainWithPeak";
inline constexpr std::string_view kAlbumGain         = "albumGain";
inline constexpr std::string_view kAlbumGainWithPeak = "albumGainWithPeak";
}  // namespace VolumeScaling

/// Linear gain for `mode`, or 1.0 when nothing applies.
///
/// The tiers cascade exactly as Cog's do, and the cascade *is* the fallback
/// behaviour: "albumGain" also consults volume, soundcheck and track gain, each
/// overwriting the last, so album gain wins when present and the best available
/// substitute is used when it is not.
///
/// A "WithPeak" mode additionally clamps the result so `scale * peak` never
/// exceeds 1.0, which is what prevents ReplayGain from clipping a loud track.
[[nodiscard]] float replayGainScale(const ReplayGainInfo& info, std::string_view mode);

/// dB to linear. Cog's db_to_scale().
[[nodiscard]] float dbToScale(float db) noexcept;

}  // namespace xpcog
