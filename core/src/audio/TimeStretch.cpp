#include "xpcog/core/audio/TimeStretch.hpp"

#include <rubberband/rubberband-c.h>

// Before the Signalsmith header, out of the sorted order the includes below
// keep: its fft.h calls std::memcpy without including <cstring>, which MSVC's
// standard headers happen to leak and libstdc++'s do not -- so alphabetical
// order here compiled on Windows and broke every Linux build in CI.
#include <cstring>
#include <signalsmith-stretch/signalsmith-stretch.h>
#include <soxr.h>

#include <algorithm>
#include <cmath>

namespace xpcog {
namespace {

/// Cog's slider range, enforced here as well as in the UI because the values
/// arrive from a settings file anybody can edit. A ratio of 0 would divide by
/// itself somewhere below; a ratio of 50 is a typo, not a request.
constexpr double kMinRatio = 0.2;
constexpr double kMaxRatio = 5.0;

/// The block cap every engine is fed under -- Cog's, from both of its nodes.
constexpr std::size_t kBlockFrames = 4096;

[[nodiscard]] double clampRatio(double value) noexcept {
    if (!(value > 0.0)) {  // catches NaN as well as zero and negatives
        return 1.0;
    }
    return std::clamp(value, kMinRatio, kMaxRatio);
}

void deinterleave(const float* interleaved, std::size_t frames, std::size_t channels,
                  float* const* planar) {
    for (std::size_t ch = 0; ch < channels; ++ch) {
        const float* in  = interleaved + ch;
        float*       out = planar[ch];
        for (std::size_t i = 0; i < frames; ++i) {
            out[i] = in[i * channels];
        }
    }
}

void interleaveAppend(const float* const* planar, std::size_t frames,
                      std::size_t channels, std::vector<float>& out) {
    const std::size_t base = out.size();
    out.resize(base + (frames * channels));
    for (std::size_t ch = 0; ch < channels; ++ch) {
        const float* in  = planar[ch];
        float*       dst = out.data() + base + ch;
        for (std::size_t i = 0; i < frames; ++i) {
            dst[i * channels] = in[i];
        }
    }
}

/// Planar scratch: one flat allocation, one pointer per channel. Sized once
/// per engine build, so process() itself never allocates for input.
struct PlanarScratch {
    std::vector<float>  samples;
    std::vector<float*> pointers;

    void size(std::size_t channels, std::size_t frames) {
        samples.assign(channels * frames, 0.0F);
        pointers.resize(channels);
        for (std::size_t ch = 0; ch < channels; ++ch) {
            pointers[ch] = samples.data() + (ch * frames);
        }
    }
};

/// Cog's getRubberbandOptions, string for string. The vocabulary defaults are
/// all the zero-valued option in their group, so an unknown string simply
/// falls through to Rubber Band's own default -- the same forgiveness the
/// choice rows in the preferences apply to a stale settings file.
[[nodiscard]] RubberBandOptions rubberbandOptionsFor(const StretchOptions& opt) {
    RubberBandOptions options = RubberBandOptionProcessRealTime;

    const bool finer = opt.engine == StretchEngine::RubberbandFiner;
    if (finer) {
        options |= RubberBandOptionEngineFiner;
    } else {
        options |= RubberBandOptionEngineFaster;
    }

    if (!finer) {
        if (opt.transients == "mixed") {
            options |= RubberBandOptionTransientsMixed;
        } else if (opt.transients == "smooth") {
            options |= RubberBandOptionTransientsSmooth;
        }
        if (opt.detector == "percussive") {
            options |= RubberBandOptionDetectorPercussive;
        } else if (opt.detector == "soft") {
            options |= RubberBandOptionDetectorSoft;
        }
        if (opt.phase == "independent") {
            options |= RubberBandOptionPhaseIndependent;
        }
        if (opt.smoothing == "on") {
            options |= RubberBandOptionSmoothingOn;
        }
    }

    // R3 has no long window; Cog maps a stored "long" back to standard there.
    if (opt.window == "short") {
        options |= RubberBandOptionWindowShort;
    } else if (opt.window == "long" && !finer) {
        options |= RubberBandOptionWindowLong;
    }

    if (opt.formant == "preserved") {
        options |= RubberBandOptionFormantPreserved;
    }
    if (opt.pitchMode == "highquality") {
        options |= RubberBandOptionPitchHighQuality;
    } else if (opt.pitchMode == "highconsistency") {
        options |= RubberBandOptionPitchHighConsistency;
    }
    if (opt.channels == "together") {
        options |= RubberBandOptionChannelsTogether;
    }

    return options;
}

}  // namespace

StretchEngine StretchOptions::engineFromString(std::string_view value) noexcept {
    if (value == "varispeed") {
        return StretchEngine::Varispeed;
    }
    if (value == "signalsmith") {
        return StretchEngine::Signalsmith;
    }
    if (value == "faster") {
        return StretchEngine::RubberbandFaster;
    }
    if (value == "finer") {
        return StretchEngine::RubberbandFiner;
    }
    return StretchEngine::Disabled;
}

// ---------------------------------------------------------------------------
// The engine seam
// ---------------------------------------------------------------------------

struct TimeStretch::Engine {
    virtual ~Engine() = default;

    /// Builds for `format` under `options`. False leaves the stage bypassing,
    /// which is the honest fallback for an engine that would not construct.
    virtual bool init(const AudioFormat& format, const StretchOptions& options) = 0;

    /// Feeds `frames` (at most kBlockFrames) interleaved frames and appends
    /// whatever output is ready. Returns the frames appended.
    virtual std::size_t feed(const float* interleaved, std::size_t frames,
                             std::vector<float>& out) = 0;

    /// Appends the end-of-stream tail, untrimmed -- the caller owns the
    /// count-in trim because the counters live there. Returns frames appended.
    virtual std::size_t flushTail(std::vector<float>& out) = 0;

    /// Absorbs an option change live. False means the change is one the engine
    /// cannot apply to a running instance and the caller must rebuild.
    virtual bool update(const StretchOptions& options) = 0;
};

namespace {

// ---------------------------------------------------------------------------
// Rubber Band, R2 and R3 -- port of DSPRubberbandNode.m
// ---------------------------------------------------------------------------

class RubberbandEngine final : public TimeStretch::Engine {
public:
    ~RubberbandEngine() override {
        if (state_ != nullptr) {
            rubberband_delete(state_);
        }
    }

    bool init(const AudioFormat& format, const StretchOptions& options) override {
        channels_    = format.channels;
        lastOptions_ = rubberbandOptionsFor(options);
        finer_       = options.engine == StretchEngine::RubberbandFiner;
        tempo_       = clampRatio(options.tempo);
        pitch_       = clampRatio(options.pitch);

        state_ = rubberband_new(static_cast<unsigned int>(format.sampleRate),
                                channels_, lastOptions_, 1.0 / tempo_, pitch_);
        if (state_ == nullptr) {
            return false;
        }

        blockFrames_ = std::min<std::size_t>(rubberband_get_process_size_limit(state_),
                                             kBlockFrames);
        rubberband_set_max_process_size(state_,
                                        static_cast<unsigned int>(blockFrames_));
        toDrop_ = rubberband_get_start_delay(state_);

        in_.size(channels_, blockFrames_);
        retrieved_.size(channels_, blockFrames_);

        // The preferred start pad, fed as silence so the engine's window is
        // full of signal by the time the first real block arrives; the matching
        // start delay is dropped from the output above. Cog's fullInit.
        std::size_t toPad = rubberband_get_preferred_start_pad(state_);
        while (toPad > 0) {
            const std::size_t step = std::min(toPad, blockFrames_);
            rubberband_process(state_, in_.pointers.data(),
                               static_cast<unsigned int>(step), 0);
            toPad -= step;
        }
        return true;
    }

    std::size_t feed(const float* interleaved, std::size_t frames,
                     std::vector<float>& out) override {
        std::size_t appended = 0;
        std::size_t offset   = 0;
        while (offset < frames) {
            const std::size_t step = std::min(frames - offset, blockFrames_);
            deinterleave(interleaved + (offset * channels_), step, channels_,
                         in_.pointers.data());
            rubberband_process(state_, in_.pointers.data(),
                               static_cast<unsigned int>(step), 0);
            offset += step;
            appended += retrieveAvailable(out);
        }
        return appended;
    }

    std::size_t flushTail(std::vector<float>& out) override {
        // Zero frames with `final` set: the documented way to say the stream
        // is over without holding a block back to say it with.
        rubberband_process(state_, in_.pointers.data(), 0, 1);
        return retrieveAvailable(out);
    }

    bool update(const StretchOptions& options) override {
        const RubberBandOptions wanted  = rubberbandOptionsFor(options);
        const RubberBandOptions changed = wanted ^ lastOptions_;

        if (changed != 0) {
            // Cog's mustRestart set: the engine choice, the window, smoothing,
            // channel coupling, and under R3 the pitch mode are all baked in at
            // construction.
            RubberBandOptions mustRestart =
                RubberBandOptionEngineFiner | RubberBandOptionWindowShort |
                RubberBandOptionWindowLong | RubberBandOptionSmoothingOn |
                RubberBandOptionChannelsTogether;
            if (finer_) {
                mustRestart |= RubberBandOptionPitchHighQuality |
                               RubberBandOptionPitchHighConsistency;
            }
            if ((changed & mustRestart) != 0) {
                return false;
            }

            constexpr RubberBandOptions kTransients =
                RubberBandOptionTransientsMixed | RubberBandOptionTransientsSmooth;
            constexpr RubberBandOptions kDetector =
                RubberBandOptionDetectorPercussive | RubberBandOptionDetectorSoft;
            constexpr RubberBandOptions kPhase   = RubberBandOptionPhaseIndependent;
            constexpr RubberBandOptions kFormant = RubberBandOptionFormantPreserved;
            constexpr RubberBandOptions kPitch   = RubberBandOptionPitchHighQuality |
                                                 RubberBandOptionPitchHighConsistency;

            if ((changed & kTransients) != 0) {
                rubberband_set_transients_option(state_, wanted & kTransients);
            }
            if (!finer_) {
                if ((changed & kDetector) != 0) {
                    rubberband_set_detector_option(state_, wanted & kDetector);
                }
                if ((changed & kPhase) != 0) {
                    rubberband_set_phase_option(state_, wanted & kPhase);
                }
                if ((changed & kPitch) != 0) {
                    rubberband_set_pitch_option(state_, wanted & kPitch);
                }
            }
            if ((changed & kFormant) != 0) {
                rubberband_set_formant_option(state_, wanted & kFormant);
            }
            lastOptions_ = wanted;
        }

        const double tempo = clampRatio(options.tempo);
        const double pitch = clampRatio(options.pitch);
        if (std::fabs(tempo - tempo_) > 1e-5 || std::fabs(pitch - pitch_) > 1e-5) {
            tempo_ = tempo;
            pitch_ = pitch;
            rubberband_set_time_ratio(state_, 1.0 / tempo_);
            rubberband_set_pitch_scale(state_, pitch_);
        }
        return true;
    }

private:
    /// Drains everything the engine has ready, dropping the start delay first.
    std::size_t retrieveAvailable(std::vector<float>& out) {
        std::size_t appended = 0;
        int         available = 0;
        while ((available = rubberband_available(state_)) > 0) {
            std::size_t chunk = std::min<std::size_t>(
                static_cast<std::size_t>(available), blockFrames_);
            if (toDrop_ > 0) {
                const std::size_t drop = std::min(chunk, toDrop_);
                rubberband_retrieve(state_, retrieved_.pointers.data(),
                                    static_cast<unsigned int>(drop));
                toDrop_ -= drop;
                continue;
            }
            chunk = rubberband_retrieve(state_, retrieved_.pointers.data(),
                                        static_cast<unsigned int>(chunk));
            if (chunk == 0) {
                break;
            }
            interleaveAppend(retrieved_.pointers.data(), chunk, channels_, out);
            appended += chunk;
        }
        return appended;
    }

    RubberBandState   state_ = nullptr;
    RubberBandOptions lastOptions_ = 0;
    bool              finer_ = false;
    std::uint32_t     channels_ = 0;
    std::size_t       blockFrames_ = kBlockFrames;
    std::size_t       toDrop_ = 0;
    double            tempo_ = 1.0;
    double            pitch_ = 1.0;
    PlanarScratch     in_;
    PlanarScratch     retrieved_;
};

// ---------------------------------------------------------------------------
// Signalsmith Stretch -- port of DSPSignalsmithStretchNode.mm
// ---------------------------------------------------------------------------

class SignalsmithEngine final : public TimeStretch::Engine {
public:
    bool init(const AudioFormat& format, const StretchOptions& options) override {
        channels_ = format.channels;
        rate_     = format.sampleRate;
        tempo_    = clampRatio(options.tempo);
        pitch_    = clampRatio(options.pitch);

        stretch_.presetDefault(static_cast<int>(channels_),
                               static_cast<float>(rate_));
        // The 8 kHz tonality limit is Cog's, from the library's own guidance:
        // above it the shifter stops trying to keep harmonics harmonic, which
        // is what speech and most instruments want.
        stretch_.setTransposeFactor(static_cast<float>(pitch_),
                                    static_cast<float>(8000.0 / rate_));

        in_.size(channels_, kBlockFrames);
        // Output per block is frames / tempo, plus the flush tail; sized for
        // the slowest tempo so process() never reallocates.
        const auto outCap = static_cast<std::size_t>(
            std::ceil(static_cast<double>(kBlockFrames) / kMinRatio)) +
            static_cast<std::size_t>(stretch_.outputLatency()) + 64;
        outScratch_.size(channels_, outCap);
        outCap_ = outCap;
        primed_ = false;
        return true;
    }

    std::size_t feed(const float* interleaved, std::size_t frames,
                     std::vector<float>& out) override {
        std::size_t appended = 0;
        std::size_t offset   = 0;
        while (offset < frames) {
            const std::size_t step = std::min(frames - offset, kBlockFrames);
            deinterleave(interleaved + (offset * channels_), step, channels_,
                         in_.pointers.data());
            offset += step;

            if (!primed_) {
                // Cog primes with the stream's own opening rather than with
                // silence: outputSeek treats the first read as surplus history,
                // so the engine starts mid-signal instead of fading in from an
                // implicit zero. These frames produce no output; the count-in
                // trim at the end settles the difference.
                stretch_.outputSeek(in_.pointers.data(), static_cast<int>(step));
                primed_ = true;
                continue;
            }

            const auto produce = static_cast<std::size_t>(
                std::floor((static_cast<double>(step) / tempo_) + 0.5));
            if (produce == 0 || produce > outCap_) {
                continue;
            }
            stretch_.process(in_.pointers.data(), static_cast<int>(step),
                             outScratch_.pointers.data(), static_cast<int>(produce));
            interleaveAppend(outScratch_.pointers.data(), produce, channels_, out);
            appended += produce;
        }
        return appended;
    }

    std::size_t flushTail(std::vector<float>& out) override {
        // The engine's own latency, expressed in output frames: what it is
        // still holding, plus the input-side window converted through the
        // tempo. Cog's toFlush, term for term.
        auto toFlush = static_cast<std::size_t>(stretch_.outputLatency()) +
                       static_cast<std::size_t>(std::floor(
                           (stretch_.inputLatency() / tempo_) + 0.5));
        toFlush = std::min(toFlush, outCap_);
        if (toFlush == 0) {
            return 0;
        }
        stretch_.flush(outScratch_.pointers.data(), static_cast<int>(toFlush));
        interleaveAppend(outScratch_.pointers.data(), toFlush, channels_, out);
        return toFlush;
    }

    bool update(const StretchOptions& options) override {
        const double tempo = clampRatio(options.tempo);
        const double pitch = clampRatio(options.pitch);
        tempo_             = tempo;  // takes effect through the next block's ratio
        if (std::fabs(pitch - pitch_) > 1e-5) {
            pitch_ = pitch;
            stretch_.setTransposeFactor(static_cast<float>(pitch_),
                                        static_cast<float>(8000.0 / rate_));
        }
        return true;
    }

private:
    signalsmith::stretch::SignalsmithStretch<float> stretch_;

    std::uint32_t channels_ = 0;
    double        rate_     = 0.0;
    double        tempo_    = 1.0;
    double        pitch_    = 1.0;
    bool          primed_   = false;
    std::size_t   outCap_   = 0;
    PlanarScratch in_;
    PlanarScratch outScratch_;
};

// ---------------------------------------------------------------------------
// Varispeed -- a soxr variable-rate resampler. No Cog counterpart.
// ---------------------------------------------------------------------------

class VarispeedEngine final : public TimeStretch::Engine {
public:
    ~VarispeedEngine() override {
        if (soxr_ != nullptr) {
            soxr_delete(soxr_);
        }
    }

    bool init(const AudioFormat& format, const StretchOptions& options) override {
        channels_ = format.channels;
        speed_    = clampRatio(options.tempo);

        // Under SOXR_VR the two rates given at creation only bound the ratio;
        // the ratio itself is set below and re-set live. The bound is the
        // slider's own ceiling.
        const soxr_io_spec_t      io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
        const soxr_quality_spec_t quality = soxr_quality_spec(SOXR_HQ, SOXR_VR);

        soxr_error_t error = nullptr;
        soxr_ = soxr_create(kMaxRatio, 1.0, channels_, &error, &io, &quality, nullptr);
        if (error != nullptr || soxr_ == nullptr) {
            if (soxr_ != nullptr) {
                soxr_delete(soxr_);
                soxr_ = nullptr;
            }
            return false;
        }
        // No slew on the first set: there is nothing playing to glide from.
        soxr_set_io_ratio(soxr_, speed_, 0);
        return true;
    }

    std::size_t feed(const float* interleaved, std::size_t frames,
                     std::vector<float>& out) override {
        return push(interleaved, frames, out);
    }

    std::size_t flushTail(std::vector<float>& out) override {
        // A null input tells soxr the stream is over; it then returns the
        // filter tail a chunk at a time until it has nothing left.
        std::size_t appended = 0;
        for (;;) {
            const std::size_t got = push(nullptr, 0, out);
            if (got == 0) {
                break;
            }
            appended += got;
        }
        return appended;
    }

    bool update(const StretchOptions& options) override {
        const double speed = clampRatio(options.tempo);
        if (std::fabs(speed - speed_) > 1e-5) {
            speed_ = speed;
            // The slew is what makes dragging the slider sound like a hand on
            // the platter rather than a gear change: soxr glides the ratio over
            // that many output samples instead of stepping it.
            soxr_set_io_ratio(soxr_, speed_, kSlewFrames);
        }
        return true;
    }

private:
    std::size_t push(const float* interleaved, std::size_t frames,
                     std::vector<float>& out) {
        // Worst case output for this input at the slowest speed, plus margin
        // for what the filter was already holding.
        const std::size_t cap =
            static_cast<std::size_t>(
                std::ceil(static_cast<double>(std::max<std::size_t>(frames, 1)) /
                          kMinRatio)) + 256;
        scratch_.resize(cap * channels_);

        std::size_t appended = 0;
        std::size_t offset   = 0;
        do {
            std::size_t        idone = 0;
            std::size_t        odone = 0;
            const soxr_error_t error = soxr_process(
                soxr_, interleaved == nullptr ? nullptr
                                              : interleaved + (offset * channels_),
                frames - offset, interleaved == nullptr ? nullptr : &idone,
                scratch_.data(), cap, &odone);
            if (error != nullptr) {
                break;
            }
            offset += idone;
            if (odone > 0) {
                out.insert(out.end(), scratch_.data(),
                           scratch_.data() + (odone * channels_));
                appended += odone;
            }
            if (interleaved == nullptr) {
                // Flush mode: one pass per call; the caller loops on the count.
                break;
            }
            if (idone == 0 && odone == 0) {
                break;  // wedged rather than progressing; do not spin
            }
        } while (offset < frames);
        return appended;
    }

    static constexpr std::size_t kSlewFrames = 4096;

    soxr_t             soxr_ = nullptr;
    std::uint32_t      channels_ = 0;
    double             speed_ = 1.0;
    std::vector<float> scratch_;
};

}  // namespace

// ---------------------------------------------------------------------------
// The stage
// ---------------------------------------------------------------------------

TimeStretch::TimeStretch()  = default;
TimeStretch::~TimeStretch() = default;

void TimeStretch::prepare(const AudioFormat& format) {
    format_ = format;
    reset();
}

void TimeStretch::setOptions(const StretchOptions& options) {
    const bool engineChanged = options.engine != options_.engine;
    if (engine_ != nullptr) {
        if (engineChanged || !engine_->update(options)) {
            rebuildWanted_ = true;
        }
    }
    options_ = options;
}

bool TimeStretch::active() const noexcept {
    return options_.engine != StretchEngine::Disabled && format_.channels > 0;
}

std::unique_ptr<TimeStretch::Engine> TimeStretch::makeEngine() const {
    std::unique_ptr<Engine> engine;
    switch (options_.engine) {
        case StretchEngine::Disabled: return nullptr;
        case StretchEngine::Varispeed: engine = std::make_unique<VarispeedEngine>(); break;
        case StretchEngine::Signalsmith:
            engine = std::make_unique<SignalsmithEngine>();
            break;
        case StretchEngine::RubberbandFaster:
        case StretchEngine::RubberbandFiner:
            engine = std::make_unique<RubberbandEngine>();
            break;
    }
    if (engine != nullptr && !engine->init(format_, options_)) {
        engine.reset();
    }
    return engine;
}

void TimeStretch::process(const float* samples, std::size_t frames,
                          std::vector<float>& out) {
    if (frames == 0) {
        return;
    }
    if (!active()) {
        out.insert(out.end(), samples, samples + (frames * format_.channels));
        return;
    }

    if (rebuildWanted_) {
        // The old engine's buffered latency goes with it, exactly as Cog's
        // fullShutdown drops it: the moment of an engine change is not one the
        // listener expects to be seamless.
        engine_.reset();
        rebuildWanted_ = false;
    }
    if (engine_ == nullptr) {
        engine_ = makeEngine();
        if (engine_ == nullptr) {
            // The engine would not build. Passing through is the difference
            // between "the option did nothing" and silence.
            out.insert(out.end(), samples, samples + (frames * format_.channels));
            consumed_ += frames;
            produced_ += frames;
            countIn_ += static_cast<double>(frames);
            return;
        }
    }

    pushBlock(samples, frames, out);
}

void TimeStretch::pushBlock(const float* samples, std::size_t frames,
                            std::vector<float>& out) {
    const double tempo = clampRatio(options_.tempo);
    consumed_ += frames;
    countIn_ += static_cast<double>(frames) / tempo;
    produced_ += engine_->feed(samples, frames, out);
}

void TimeStretch::drain(std::vector<float>& out) {
    if (engine_ == nullptr) {
        return;
    }

    // The tail, then the trim. A real-time stretcher does not count its own
    // flush in its ratio, so left alone every stream would end with a latency's
    // worth of overhang; the ideal length is what the input was worth, and
    // anything past it is cut. Cog's countIn/ideal logic, in both of its nodes.
    const std::size_t before = out.size();
    engine_->flushTail(out);
    const std::size_t tail = (out.size() - before) / format_.channels;

    const auto ideal = static_cast<std::uint64_t>(std::floor(countIn_ + 0.5));
    std::size_t keep = tail;
    if (produced_ + tail > ideal) {
        keep = (ideal > produced_) ? static_cast<std::size_t>(ideal - produced_) : 0;
        out.resize(before + (keep * format_.channels));
    }
    produced_ += keep;

    // A flushed engine is spent; the next block builds a fresh one.
    engine_.reset();
}

void TimeStretch::reset() {
    engine_.reset();
    rebuildWanted_ = false;
    consumed_      = 0;
    produced_      = 0;
    countIn_       = 0.0;
}

}  // namespace xpcog
