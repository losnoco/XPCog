#include "xpcog/core/audio/Waveform.hpp"

#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/audio/SampleConvert.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>

namespace xpcog {
namespace {

constexpr auto kProgressInterval = std::chrono::milliseconds{100};

/// One bucket's running totals. Kept in floating point until the end, so the
/// RMS is exact over however many frames the bucket turned out to hold.
struct Accumulator {
    float         peak  = 0.0F;
    double        sumSq = 0.0;
    std::uint64_t count = 0;
};

[[nodiscard]] std::uint8_t quantise(double level) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(level, 0.0, 1.0) * 255.0));
}

void finalise(const std::vector<Accumulator>& sums, std::uint32_t upTo,
              WaveformSummary& out) {
    for (std::uint32_t i = out.analysed; i < upTo; ++i) {
        const Accumulator& a = sums[i];
        out.peak[i] = quantise(a.peak);
        out.rms[i]  = quantise(a.count > 0 ? std::sqrt(a.sumSq / static_cast<double>(a.count))
                                           : 0.0);
    }
    out.analysed = std::max(out.analysed, upTo);
}

}  // namespace

bool analyseWaveform(IDecoder& decoder, WaveformSummary& out,
                     const std::function<bool()>&                       cancelled,
                     const std::function<void(const WaveformSummary&)>& progress) {
    const TrackProperties props = decoder.properties();
    if (props.totalFrames <= 0 || props.format.format == SampleFormat::DSD ||
        props.format.sampleRate <= 0.0) {
        return false;
    }

    out.bucketCount = kWaveformBuckets;
    out.analysed    = 0;
    out.duration    = props.duration();
    out.peak.assign(out.bucketCount, 0);
    out.rms.assign(out.bucketCount, 0);

    std::vector<Accumulator> sums(out.bucketCount);
    const auto totalFrames = static_cast<std::uint64_t>(props.totalFrames);

    AudioChunk         chunk;
    std::vector<float> samples;
    std::uint64_t      frame    = 0;
    auto               lastTold = std::chrono::steady_clock::now();

    while (true) {
        if (cancelled && cancelled()) {
            return false;
        }
        if (!decoder.readAudio(chunk)) {
            break;
        }
        const std::uint32_t channels = chunk.format().channels;
        const std::size_t   frames   = chunk.frameCount();
        if (channels == 0 || frames == 0) {
            continue;
        }
        if (chunk.format().format == SampleFormat::DSD) {
            return false;
        }

        samples.resize(float32SampleCount(chunk));
        if (convertToFloat32(chunk, samples) == 0) {
            return false;
        }

        const float* in = samples.data();
        for (std::size_t f = 0; f < frames; ++f, in += channels) {
            float mono = 0.0F;
            for (std::uint32_t c = 0; c < channels; ++c) {
                mono += in[c];
            }
            mono /= static_cast<float>(channels);

            // A decoder that runs past its declared length folds into the last
            // bucket rather than off the end of the array. One that stops short
            // leaves the tail at zero, which is what the seek bar's duration --
            // read from the same declaration -- would show as unreached anyway.
            const std::uint64_t index =
                std::min<std::uint64_t>((frame + f) * out.bucketCount / totalFrames,
                                        out.bucketCount - 1);
            Accumulator& a = sums[index];
            a.peak = std::max(a.peak, std::fabs(mono));
            a.sumSq += static_cast<double>(mono) * static_cast<double>(mono);
            ++a.count;
        }
        frame += frames;

        if (progress) {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastTold >= kProgressInterval) {
                lastTold = now;
                // Up to but not including the bucket being written: that one is
                // still growing and would be reported quieter than it ends up.
                const auto reached = static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(frame * out.bucketCount / totalFrames,
                                            out.bucketCount - 1));
                finalise(sums, reached, out);
                progress(out);
            }
        }
    }

    // Set explicitly rather than from the last frame index: a track that ended
    // a few frames short of its declaration is still complete, and an incomplete
    // summary is never stored.
    finalise(sums, out.bucketCount, out);
    return true;
}

}  // namespace xpcog
