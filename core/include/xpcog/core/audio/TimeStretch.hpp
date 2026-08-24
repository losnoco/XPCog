// The pitch and tempo stage. Port of Cog Audio/Chain/DSP/DSPRubberbandNode.m
// and DSPSignalsmithStretchNode.mm, folded into one stage, plus a varispeed
// engine Cog does not have.
//
// This is deliberately *not* a DSPNode. That contract is in place with a fixed
// frame count, and a time-stretcher's whole job is to return a different number
// of frames than it was given -- the same reason FreeSurround lives in the
// converter rather than the chain. It cannot go in the converter either,
// though: the converter feeds the deep three-second ring, and a tempo slider
// whose effect is three seconds away reads as broken. So this is a third shape,
// sitting where Cog's stretch nodes sit -- between the deep buffer and the
// short chain, called by the DSP thread with append-to-vector semantics -- and
// AudioEngine::dspLoop() is its one caller.
//
// The engines:
//
//   * `faster` / `finer` -- Rubber Band's R2 and R3, through the C API, with
//     the same option vocabulary Cog's Rubber Band pane writes.
//   * `signalsmith` -- Signalsmith Stretch, as Cog runs it: presetDefault, a
//     tonality limit of 8 kHz, and the output-seek priming on the first block.
//   * `varispeed` -- a soxr variable-rate resampler, which is the turntable
//     answer: pitch and tempo are one knob, locked together by physics rather
//     than by the UI, in exchange for costing a fraction of either stretcher.
//     Cog has no equivalent; its speed lock runs both stretch ratios at the
//     same value, which this does far cheaper when that is all that is wanted.
//
// What is common to all three, ported from Cog rather than rediscovered:
//
//   * Real-time mode, fed block by block, with options applied live where the
//     engine allows and by a rebuild where it does not (Cog's mustRestart
//     mask).
//   * The count-in/count-out trim. A stretcher's flush does not account for
//     its own latency, so at end of stream the output is cut to
//     round(input / tempo) total frames -- without it every track grows a
//     stretcher-latency tail of garbage. Cog carries the identical `countIn`
//     and `ideal` logic in both nodes.
//   * Counters for the position clock. Consumed and produced frame totals are
//     what AudioEngine's stretch map is built from, so the seek bar and the
//     gapless seam announcements stay in track time while the device plays
//     stretched time.

#pragma once

#include "xpcog/core/AudioFormat.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog {

enum class StretchEngine : std::uint8_t {
    Disabled,
    Varispeed,          ///< soxr variable-rate: one knob, pitch follows tempo
    Signalsmith,        ///< Signalsmith Stretch
    RubberbandFaster,   ///< Rubber Band R2
    RubberbandFiner,    ///< Rubber Band R3
};

/// Everything the stage is configured by, as one value so a settings read is a
/// single assignment. The strings are Cog's option vocabulary verbatim --
/// "crisp", "compound", "laminar" and so on -- because they come straight from
/// settings keys shared with Cog's plist; parsing them belongs to this stage,
/// not to whoever read the file.
struct StretchOptions {
    StretchEngine engine = StretchEngine::Disabled;

    /// Speed ratio, 1.0 unstretched, clamped to [0.2, 5.0] as Cog's sliders
    /// are. Under varispeed this is the one knob.
    double tempo = 1.0;
    /// Frequency ratio, 1.0 unshifted, same range. Ignored by varispeed, whose
    /// pitch *is* its tempo.
    double pitch = 1.0;

    // Rubber Band's option groups. Ignored by the other engines.
    std::string transients = "crisp";     ///< crisp | mixed | smooth (R2)
    std::string detector   = "compound";  ///< compound | percussive | soft (R2)
    std::string phase      = "laminar";   ///< laminar | independent (R2)
    std::string window     = "standard";  ///< standard | short | long (long is R2-only)
    std::string smoothing  = "off";       ///< off | on (R2)
    std::string formant    = "shifted";   ///< shifted | preserved
    std::string pitchMode  = "highspeed"; ///< highspeed | highquality | highconsistency
    std::string channels   = "apart";     ///< apart | together

    /// The `rubberbandEngine` settings value: disabled, varispeed, signalsmith,
    /// faster, finer. Unknown strings -- a newer build's engine in an older
    /// build's settings -- read as disabled, which is the value's own default.
    [[nodiscard]] static StretchEngine engineFromString(std::string_view value) noexcept;
};

class TimeStretch {
public:
    /// One virtual seam, three implementations, all private to the .cpp.
    /// Public only so those implementations can derive from it; nothing else
    /// can reach one -- the factory and the pointer are both private.
    struct Engine;

    TimeStretch();
    ~TimeStretch();

    TimeStretch(const TimeStretch&)            = delete;
    TimeStretch& operator=(const TimeStretch&) = delete;

    /// Sizes for `format` and drops any engine built for the previous one.
    /// Call before the first process() and again on a device format change.
    void prepare(const AudioFormat& format);

    /// Applies a fresh settings read. Live where the engine can -- pitch and
    /// tempo always are -- and by scheduling a rebuild where it cannot, which
    /// is Cog's mustRestart set: engine choice, window, smoothing, R3's pitch
    /// mode, channel coupling. The rebuild happens inside the next process()
    /// rather than here, so a slider moved while paused costs nothing until
    /// audio flows again.
    void setOptions(const StretchOptions& options);

    /// Feeds `frames` interleaved frames and appends whatever the engine has
    /// ready to `out` -- possibly nothing, possibly several blocks' worth.
    /// With the stage inactive it appends the input verbatim.
    void process(const float* samples, std::size_t frames, std::vector<float>& out);

    /// Flushes the engine's tail at end of stream, trimmed so the stream's
    /// total output is round(total input / tempo). The engine does not survive
    /// its own flush; the next process() builds a fresh one.
    void drain(std::vector<float>& out);

    /// Drops the engine and every counter. Call on a seam the signal does not
    /// flow across -- a seek, a stop.
    void reset();

    /// Whether the stage transforms at all. False for Disabled, and the DSP
    /// loop then bypasses it entirely; an enabled engine at 1.0/1.0 still runs,
    /// as it does in Cog, so nudging a slider off unity is seamless.
    [[nodiscard]] bool active() const noexcept;

    /// Interleaved frames taken in and handed out since the last reset().
    /// The stretch map's two axes; see AudioEngine.
    [[nodiscard]] std::uint64_t framesConsumed() const noexcept { return consumed_; }
    [[nodiscard]] std::uint64_t framesProduced() const noexcept { return produced_; }

private:
    /// Builds the engine the options ask for. Null for Disabled, or when the
    /// build failed -- both bypass.
    [[nodiscard]] std::unique_ptr<Engine> makeEngine() const;

    /// Feeds one bounded sub-block to the engine and collects its output.
    void pushBlock(const float* samples, std::size_t frames, std::vector<float>& out);

    StretchOptions options_;
    AudioFormat    format_{};

    std::unique_ptr<Engine> engine_;
    /// Raised by setOptions() when the running engine cannot absorb the change
    /// live; consumed by process(), which rebuilds. Never read off-thread: both
    /// halves run on the DSP thread.
    bool rebuildWanted_ = false;

    std::uint64_t consumed_ = 0;
    std::uint64_t produced_ = 0;
    /// Output frames the input consumed so far is worth: sum of block/tempo at
    /// the tempo each block was fed under. What drain() trims the total to,
    /// double so a long album at 0.9x does not accumulate a rounding drift.
    double countIn_ = 0.0;
};

}  // namespace xpcog
