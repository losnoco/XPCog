#include "xpcog/core/audio/ReplayGain.hpp"

#include <cmath>
#include <cstdlib>

namespace xpcog {

float dbToScale(float db) noexcept { return std::pow(10.0F, db / 20.0F); }

float replayGainScale(const ReplayGainInfo& info, std::string_view mode) {
    if (mode.empty() || mode == VolumeScaling::kNone) {
        return 1.0F;
    }

    // Cog derives these as a chain of prefix tests, each tier enabling the ones
    // below it. Reproduced exactly: the implication order is the fallback order.
    const bool useAlbum = mode.starts_with(VolumeScaling::kAlbumGain);
    const bool useTrack = useAlbum || mode.starts_with(VolumeScaling::kTrackGain);
    const bool useSoundcheck =
        useAlbum || useTrack || mode == VolumeScaling::kSoundcheck;
    const bool useVolume = useAlbum || useTrack || useSoundcheck ||
                           mode == VolumeScaling::kVolumeScale;
    const bool usePeak = mode.ends_with("WithPeak");

    float scale = 1.0F;
    float peak  = 0.0F;

    if (useVolume && info.volume) {
        scale = *info.volume;
    }
    if (useSoundcheck && info.soundcheck) {
        // iTunes iTunNORM: several hex fields, the first being the base-1000 gain.
        // Cog takes -floatValue of the string, which yields 0 for a hex payload;
        // parsing the first field properly is the behaviour users expect.
        const char* text  = info.soundcheck->c_str();
        char*       after = nullptr;
        const long  raw   = std::strtol(text, &after, 16);
        if (after != text && raw > 0) {
            scale = static_cast<float>(raw) / 1000.0F;
        }
    }
    if (useTrack) {
        if (info.trackGain) {
            scale = dbToScale(*info.trackGain);
        }
        if (info.trackPeak) {
            peak = *info.trackPeak;
        }
    }
    if (useAlbum) {
        if (info.albumGain) {
            scale = dbToScale(*info.albumGain);
        }
        if (info.albumPeak) {
            peak = *info.albumPeak;
        }
    }

    // Pull the gain back so the loudest sample still fits, rather than clipping.
    if (usePeak && peak > 0.0F && scale * peak > 1.0F) {
        scale = 1.0F / peak;
    }

    return scale;
}

}  // namespace xpcog
