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

    /// Upmixes stereo to FreeSurround's 5.1 on the way through.
    ///
    /// Set *after* setOutputFormat, whose channel count must already be the 6
    /// this produces -- the device is opened once and the upmix is what decides
    /// how wide it is. Everything upstream of the upmixer then runs in stereo:
    /// channel fitting reduces to it, and the resampler is created for two
    /// channels rather than six, which is most of the work saved.
    ///
    /// Enabling this makes the converter's output lag its input by half a block,
    /// which drain() gives back. It is not reported as latency because it is not
    /// visible as latency: the leading half block is discarded, so the stream
    /// stays aligned with the clock rather than starting 46 ms late.
    void setFreeSurround(bool enabled);
    [[nodiscard]] bool freeSurroundEnabled() const noexcept;

    /// Linear gain applied on the way through, from ReplayGain.
    void setGain(float gain) noexcept { gain_ = gain; }
    [[nodiscard]] float gain() const noexcept { return gain_; }

    /// Converts `in` and appends interleaved float32 to `out`.
    /// Returns false only when the input format cannot be handled at all.
    bool process(const AudioChunk& in, std::vector<float>& out);

    /// Flushes whatever the resampler is still holding. Call at end of stream, or
    /// at a track seam, or the tail of the outgoing track is lost.
    ///
    /// The soxr instance does not survive being flushed -- it is a one-shot end
    /// of stream, and feeding a drained instance again is undefined. So this
    /// disposes of it, and the next process() builds a fresh one for whatever
    /// format the incoming track turns out to be.
    void drain(std::vector<float>& out);

    /// Discards resampler, HDCD and extrapolation state. Call after a seek so
    /// history from the old position cannot bleed into the new one.
    void reset();

    /// Decode HDCD control codes in 16-bit input, expanding to the full 20-bit
    /// range the format actually carries.
    void setHdcdEnabled(bool enabled) noexcept { hdcdEnabled_ = enabled; }

    /// Halve DSD on the way to PCM.
    ///
    /// The decimation filter has an overall gain of 2.0, which is deliberate:
    /// DSD's practical ceiling is about 50% modulation, so 2.0 puts *that* at
    /// full scale rather than 6 dB below it. A recording that goes higher
    /// clips, and this is the escape. Cog's `halveDSDVolume`, and off for the
    /// same reason it is off there.
    void setHalveDsd(bool enabled) noexcept { halveDsd_ = enabled; }

    /// True once HDCD codes have actually been seen in this stream. Cog surfaces
    /// the same thing as an indicator in the UI.
    [[nodiscard]] bool hdcdDetected() const noexcept { return hdcdDetected_; }

    [[nodiscard]] double outputSampleRate() const noexcept { return outRate_; }
    [[nodiscard]] std::uint32_t outputChannels() const noexcept { return outChannels_; }

private:
    /// Rebuilds the resampler when the input format changes mid-stream.
    bool configureFor(const AudioFormat& input);

    /// Deletes the soxr instance and forgets the format it was built for, so the
    /// next process() builds a fresh one.
    void closeResampler() noexcept;

    /// Prepends a predicted run-up to the block about to be resampled, in
    /// `padded_`, and arms the trim that takes it back off the output.
    ///
    /// The resampler's filter is centred on the sample it is producing and
    /// reaches some way either side of it, so at the very first block it
    /// convolves the leading samples against an implicit run of zeros -- a step
    /// from silence into the music, which is a click. LPC gives it a plausible
    /// continuation to reach into instead. Cog's ConverterNode does the same,
    /// once per stream.
    void extrapolateLeadIn(std::size_t frames);

    /// The mirror at the far end: predicts past the last real sample from
    /// `history_`, feeds that to the resampler, and returns how many output
    /// frames the prediction is worth so drain() can take them back off.
    /// Zero when there is nothing to predict from.
    std::size_t pushLeadOut(std::vector<float>& out);

    /// Feeds `frames` through soxr, appending every output frame to `out`.
    /// Loops until the input is consumed, whatever the output buffer holds.
    bool resampleInto(const float* input, std::size_t frames, std::vector<float>& out);

    /// Keeps the last `primeLen_` frames of what was fed, for pushLeadOut().
    void rememberTail(const float* input, std::size_t frames);

    /// Drops the front trim armed by extrapolateLeadIn() off `buffer`, carrying
    /// what is left over into the next call when the buffer is shorter.
    void eatLeadIn(std::vector<float>& buffer);

    /// Gain, then out -- through the upmixer if one is running. The tail of
    /// both process() and drain().
    void emitFrames(const float* samples, std::size_t frames, std::vector<float>& out);

    /// Turns one chunk of DSD into float in `decoded_`. False if the filters
    /// could not be built.
    bool decimateDsd(const AudioChunk& in, std::size_t frames);

    /// The channel count everything before the upmixer works in: two when the
    /// upmixer is running, the output count otherwise.
    [[nodiscard]] std::uint32_t chainChannels() const noexcept;

    void configureFreeSurround();
    void pushFreeSurround(const float* stereo, std::size_t frames, std::vector<float>& out);
    void emitFreeSurroundBlock(const float* stereoBlock, std::vector<float>& out);
    void flushFreeSurround(std::vector<float>& out);
    void appendWithGain(const float* samples, std::size_t frames, std::vector<float>& out);

    struct Soxr;
    std::unique_ptr<Soxr> soxr_;

    double        outRate_     = 0.0;
    std::uint32_t outChannels_ = 0;
    std::string   quality_     = "high";

    // The input format the resampler is currently set up for.
    double        inRate_     = 0.0;
    std::uint32_t inChannels_ = 0;

    float gain_ = 1.0F;

    /// One decimation filter per channel, built when DSD first arrives and
    /// reset on seek. Opaque so the vendored header stays out of this one.
    struct DsdFilters;
    std::unique_ptr<DsdFilters> dsd_;
    bool                        halveDsd_ = false;

    bool hdcdEnabled_  = true;
    bool hdcdDetected_ = false;
    /// Opaque so <hdcd_decode2.h> stays out of this header.
    struct Hdcd;
    std::unique_ptr<Hdcd> hdcd_;

    class FreeSurroundStage;
    std::unique_ptr<FreeSurroundStage> fsurround_;
    bool freeSurroundWanted_ = false;
    /// Interleaved stereo waiting for a full block. The decoder takes exactly
    /// one block or nothing, so anything short of that has to wait here.
    std::vector<float> fsPending_;
    std::vector<float> fsGained_;
    /// Output frames accepted but not yet emitted. Drives the flush: what is
    /// owed at end of stream is the partial block plus the half-block of delay.
    std::size_t fsOwed_ = 0;
    /// Leading output frames still to be discarded -- the decoder's half block
    /// of priming, dropped so the stream stays aligned instead of starting late.
    std::size_t fsSkip_ = 0;
    std::vector<int>      hdcdSamples_;

    /// LPC scratch, reallocated in place by the extrapolator. Opaque so
    /// <lpc.h> stays out of this header.
    struct LpcScratch;
    std::unique_ptr<LpcScratch> lpc_;

    /// How many input frames the extrapolator predicts at each edge, and what
    /// that is worth in output frames.
    ///
    /// The pair is exact rather than rounded: it is the input:output rate ratio
    /// reduced by its GCD and scaled back up, so `padOut_` really is what
    /// `padIn_` frames become, with no fraction left over to accumulate into a
    /// drift. Cog's samples_len, and the reason it exists.
    std::size_t padIn_  = 0;
    std::size_t padOut_ = 0;
    /// How many real frames the prediction is derived from -- about a twentieth
    /// of a second, bounded either way.
    std::size_t primeLen_ = 0;
    /// Whether the run-up has been prepended yet. Once per resampler, which is
    /// once per track now that drain() disposes of the instance.
    bool        leadInDone_ = false;
    /// Output frames still owed to the front trim. Usually cleared by the first
    /// block; a track shorter than the padding spreads it over several.
    std::size_t latencyEaten_ = 0;

    /// Rolling tail of the input, as long as `primeLen_`. What the far edge is
    /// predicted from, since by the time the stream ends the blocks it was
    /// carried in are long gone.
    std::vector<float> history_;
    /// The block plus its predicted edge, which is what the resampler is
    /// actually fed at a stream boundary.
    std::vector<float> padded_;

    std::vector<float> decoded_;   ///< input as float32
    std::vector<float> remapped_;  ///< after channel fitting
    std::vector<float> resampled_;
};

}  // namespace xpcog
