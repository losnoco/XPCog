// DSD to float, one byte in, one sample out.
//
// The eight-to-one decimation that turns a one-bit stream into something the
// rest of the chain can measure, wrapped so it can run in more than one place:
// AudioConverter needs it on the way to the device, and the waveform analyser
// needs it to have anything to draw. The filter itself is vendor/dsd2pcm; this
// keeps one instance of it per channel and walks an interleaved chunk.
//
// The rate does not change here. A chunk's frame count for DSD is already in
// bytes per channel -- that is how the decoders declare it, so that duration
// arithmetic stays ordinary -- and each byte becomes exactly one float, so
// 705,600 Hz of DSD128 leaves as 705,600 Hz of PCM.
//
// The filter's gain is 2.0, deliberately: see AudioConverter::setHalveDsd.

#pragma once

#include "xpcog/core/AudioChunk.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace xpcog {

class DsdDecimator {
public:
    DsdDecimator();
    ~DsdDecimator();

    DsdDecimator(const DsdDecimator&)            = delete;
    DsdDecimator& operator=(const DsdDecimator&) = delete;

    /// Turns a DSD chunk into interleaved float32 in `out`, one frame per input
    /// frame, resizing `out` to fit. False if the chunk is not DSD, has no
    /// channels, or a filter could not be allocated.
    bool process(const AudioChunk& in, std::vector<float>& out);

    /// Forgets the 64 taps of history. After a seek those are the wrong taps;
    /// reset rather than rebuilt, because rebuilding recomputes the lookup
    /// tables and the far side of a seek is the same stream.
    void reset() noexcept;

private:
    struct Filters;  // opaque so the vendored header stays out of this one
    std::unique_ptr<Filters> filters_;
};

}  // namespace xpcog
