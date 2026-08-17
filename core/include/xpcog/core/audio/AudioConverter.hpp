// Sample-rate conversion, channel fitting and ReplayGain scaling.
//
// This is the job Cog's ConverterNode does (Audio/Chain/ConverterNode.m): take
// whatever a decoder produced and deliver the fixed format the device is running
// at. Holding the device format fixed is what makes a track change gapless even
// when the next file has a different sample rate -- without it the device has to
// be reconfigured mid-stream, which cannot be seamless.
//
// Resampling is libsoxr and edge extrapolation is LPC, both as in Cog. HDCD is
// decoded here too, because it is stateful across chunks and so cannot live in
// the stateless sample conversion.

#pragma once

#include "xpcog/core/AudioChunk.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog {

class AudioConverter {
public:
    AudioConverter();
    ~AudioConverter();

    AudioConverter(const AudioConverter&)            = delete;
    AudioConverter& operator=(const AudioConverter&) = delete;

    /// The format everything is converted to. Set once from the device.
    /// `quality` takes soxr's names: quick, low, medium, high, best.
    bool setOutputFormat(double sampleRate, std::uint32_t channels,
                         std::string_view quality = "high");

    /// Linear gain applied on the way through, from ReplayGain.
    void setGain(float gain) noexcept { gain_ = gain; }
    [[nodiscard]] float gain() const noexcept { return gain_; }

    /// Converts `in` and appends interleaved float32 to `out`.
    /// Returns false only when the input format cannot be handled at all.
    bool process(const AudioChunk& in, std::vector<float>& out);

    /// Flushes whatever the resampler is still holding. Call at end of stream, or
    /// the tail of the last track is lost.
    void drain(std::vector<float>& out);

    /// Discards resampler, HDCD and extrapolation state. Call after a seek so
    /// history from the old position cannot bleed into the new one.
    void reset();

    /// Decode HDCD control codes in 16-bit input, expanding to the full 20-bit
    /// range the format actually carries.
    void setHdcdEnabled(bool enabled) noexcept { hdcdEnabled_ = enabled; }

    /// True once HDCD codes have actually been seen in this stream. Cog surfaces
    /// the same thing as an indicator in the UI.
    [[nodiscard]] bool hdcdDetected() const noexcept { return hdcdDetected_; }

    [[nodiscard]] double outputSampleRate() const noexcept { return outRate_; }
    [[nodiscard]] std::uint32_t outputChannels() const noexcept { return outChannels_; }

private:
    /// Rebuilds the resampler when the input format changes mid-stream.
    bool configureFor(const AudioFormat& input);

    struct Soxr;
    std::unique_ptr<Soxr> soxr_;

    double        outRate_     = 0.0;
    std::uint32_t outChannels_ = 0;
    std::string   quality_     = "high";

    // The input format the resampler is currently set up for.
    double        inRate_     = 0.0;
    std::uint32_t inChannels_ = 0;

    float gain_ = 1.0F;

    bool hdcdEnabled_  = true;
    bool hdcdDetected_ = false;
    /// Opaque so <hdcd_decode2.h> stays out of this header.
    struct Hdcd;
    std::unique_ptr<Hdcd> hdcd_;
    std::vector<int>      hdcdSamples_;

    /// Rolling tail of the previous block, used to extrapolate backwards into the
    /// next one so the resampler never sees a discontinuity at a block edge.
    std::vector<float> history_;

    std::vector<float> decoded_;   ///< input as float32
    std::vector<float> remapped_;  ///< after channel fitting
    std::vector<float> resampled_;
};

}  // namespace xpcog
