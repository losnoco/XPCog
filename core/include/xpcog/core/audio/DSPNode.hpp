// One stage of the DSP chain. Port of Cog Audio/Chain/DSPNode.h, restructured.
//
// Cog's DSPNode is a *threaded* node: each one owns an NSThread, a ChunkList
// buffer sized by a latency argument, a write semaphore, a read semaphore and a
// recursive lock, and the chain is a pipeline of producer/consumer handoffs.
// That shape follows from Cog's engine, where every stage between the decoder
// and the output is such a node.
//
// XPCog's engine is one feeder thread that pulls from the decoder, converts, and
// writes into a lock-free ring the output drains (see AudioEngine). Porting
// Cog's threading on top of that would add a thread, a buffer and two
// semaphores per stage to a pipeline that already has exactly the concurrency it
// needs, and would put a lock between the decoder and the ring for no benefit.
// So a DSPNode here is a synchronous transform, called on the feeder thread
// between conversion and the ring, and the chain is a plain sequence.
//
// What that buys, beyond the obvious: the chain is deterministic. A stage can be
// unit-tested by calling process() on a buffer, with no thread to synchronise
// and no wall-clock behaviour to flake on, and the offline output already makes
// the whole engine reproducible.
//
// The contract:
//
//   * process() must not allocate. Anything a stage needs is sized in
//     prepare(), which is the only place a format change is observed.
//   * process() works in place on interleaved float, because that is what the
//     converter already produces and the ring already consumes. Cog's nodes each
//     allocate a fresh ChunkList buffer per call; here the feeder's buffer is
//     reused for the whole chain.
//   * reset() drops filter state. This is not optional bookkeeping -- a biquad
//     carrying two samples of history across a seek smears the old position into
//     the new one, which is audible as a click at exactly the moment the user is
//     listening for the seek to land.
//   * active() reports whether the stage currently changes the signal at all,
//     so a chain can skip it. A stage configured to do nothing must be
//     bit-transparent, and the cheapest way to guarantee that is not to run it.

#pragma once

#include <cstddef>

namespace xpcog {

struct AudioFormat;

class DSPNode {
public:
    virtual ~DSPNode() = default;

    DSPNode()                          = default;
    DSPNode(const DSPNode&)            = delete;
    DSPNode& operator=(const DSPNode&) = delete;

    /// Sizes state for `format` and drops anything held for the previous one.
    /// Called before the first process() and again whenever the format changes.
    virtual void prepare(const AudioFormat& format) = 0;

    /// Transforms `frames` frames of interleaved float in place. `samples` holds
    /// `frames * channels` values, with channels as given to prepare(). Must not
    /// allocate, and must be safe to call with frames == 0.
    virtual void process(float* samples, std::size_t frames) = 0;

    /// Drops filter state without changing configuration. Called on a seam the
    /// signal does not flow across -- a seek, a stop, a flush.
    virtual void reset() = 0;

    /// False when this stage would leave the signal untouched, so callers may
    /// skip it entirely.
    [[nodiscard]] virtual bool active() const = 0;

    /// Frames of delay this stage adds, for a chain that has to report latency.
    /// Zero for anything without a lookahead, which is every M4 stage bar the
    /// time-stretchers.
    [[nodiscard]] virtual std::size_t latencyFrames() const { return 0; }
};

}  // namespace xpcog
