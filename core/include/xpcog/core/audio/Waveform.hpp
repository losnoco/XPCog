// A track's shape, for the seek bar to draw behind the playhead.
//
// No Cog counterpart: Cog's position slider is a plain NSSlider. Nothing in the
// player has whole-track audio either -- the spectrum taps the last fraction of
// a second of *played* output -- so a waveform means decoding the track a second
// time, off the interface thread, and folding it down to something a bar a few
// hundred pixels wide can show.
//
// What is kept is deliberately small. The track is cut into a fixed number of
// equal buckets and each keeps two bytes: the loudest sample and the RMS level,
// channels averaged to one. A thousand buckets is more than a seek bar has
// pixels, and two kilobytes a track is cheap enough to keep on disk for every
// track ever played (WaveformCache). Per-channel and peak-only layouts are left
// room for in the file format but are not built.

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace xpcog {

class IDecoder;

/// Buckets per track. A constant rather than a parameter: the seek bar maps
/// them to whatever width it has, and one size means one cache entry per track.
inline constexpr std::uint32_t kWaveformBuckets = 1024;

struct WaveformSummary {
    std::uint32_t bucketCount = 0;
    /// Buckets filled so far, from the left. Equal to bucketCount when the
    /// whole track has been read; less while the analysis is still running,
    /// which is what lets the bar fill in as it goes.
    std::uint32_t analysed = 0;
    /// The track's length as the decoder declared it, in seconds.
    double duration = 0.0;
    /// The loudest sample in each bucket, |x| on a linear 0..255 scale.
    std::vector<std::uint8_t> peak;
    /// sqrt(mean x^2) of each bucket, on the same scale.
    std::vector<std::uint8_t> rms;

    [[nodiscard]] bool complete() const noexcept {
        return bucketCount > 0 && analysed >= bucketCount;
    }
};

/// Reads `decoder` from where it is to the end and fills `out`.
///
/// Returns false, leaving `out` unspecified, when the track cannot be summarised
/// -- it has no declared length (a stream), or it is DSD, which has no PCM to
/// measure without the decimation filter -- or when `cancelled()` answered true
/// between two reads. The decoder is read where it stands, so open it fresh and
/// with LoopPolicy::Never, or a looping format never ends.
///
/// `progress`, if given, is called on the calling thread at most every hundred
/// milliseconds or so with `out` as far as it has got, `analysed` telling how
/// far that is. The callback may copy it; it must not keep the reference.
bool analyseWaveform(IDecoder& decoder, WaveformSummary& out,
                     const std::function<bool()>&                       cancelled,
                     const std::function<void(const WaveformSummary&)>& progress = {});

}  // namespace xpcog
