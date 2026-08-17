// The unit of audio passing through the chain. Replaces Cog's AudioChunk.
//
// Deliberate deviation from Cog: this type is move-only and read APIs fill a
// caller-supplied chunk, so buffers can be recycled. Cog's -readAudio allocates a
// fresh NSMutableData every 16 KiB, which is a lot of churn on the audio path.
// Storage is a plain vector for now; the API shape is what matters, and a pooled
// allocator can be dropped in later without touching any call site.

#pragma once

#include "xpcog/core/AudioFormat.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace xpcog {

class AudioChunk {
public:
    AudioChunk() = default;

    AudioChunk(const AudioChunk&)            = delete;
    AudioChunk& operator=(const AudioChunk&) = delete;
    AudioChunk(AudioChunk&&) noexcept        = default;
    AudioChunk& operator=(AudioChunk&&) noexcept = default;

    // --- format and flags -------------------------------------------------

    [[nodiscard]] const AudioFormat& format() const noexcept { return format_; }
    void setFormat(const AudioFormat& format) noexcept { format_ = format; }

    /// Source was losslessly encoded. Drives the UI indicator and gates HDCD.
    bool lossless = false;
    /// HDCD control codes were detected while decoding this chunk.
    bool hdcd = false;
    /// DoP payload needs its bit order reversed for this device.
    bool dsdDoPReverseBits = false;
    /// Marks the first chunk after a seek or track change, so downstream nodes
    /// flush filter state instead of smearing across the discontinuity.
    bool resetForward = false;

    /// Presentation time in seconds from the start of the stream.
    double streamTimestamp = 0.0;
    /// Time-stretch ratio applied downstream; 1.0 when unmodified.
    double streamTimeRatio = 1.0;

    // --- storage ----------------------------------------------------------

    /// Resizes to hold `frames` and returns writable storage. Contents are
    /// unspecified; callers are expected to overwrite them.
    [[nodiscard]] std::byte* allocFrames(std::size_t frames);

    /// Copies `frames` worth of interleaved samples from `src`.
    void assign(const void* src, std::size_t frames);

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {data_.data(), data_.size()};
    }
    [[nodiscard]] std::span<std::byte> bytesMut() noexcept {
        return {data_.data(), data_.size()};
    }

    [[nodiscard]] std::size_t frameCount() const noexcept;

    /// Truncation only, matching Cog's -setFrameCount:. Growing is a programming
    /// error; use allocFrames().
    void setFrameCount(std::size_t frames);

    /// Moves the first `frames` frames out of this chunk into `out`, shifting the
    /// remainder down. Cog's -removeSamples:.
    void removeFrames(std::size_t frames, AudioChunk& out);

    [[nodiscard]] bool empty() const noexcept { return data_.empty(); }

    /// Length in seconds at the chunk's own sample rate.
    [[nodiscard]] double duration() const noexcept;
    /// Length in seconds after the time-stretch ratio is applied.
    [[nodiscard]] double durationRatioed() const noexcept;

    /// Drops the samples but keeps the format, flags and (importantly) the
    /// allocated capacity, so the chunk can be refilled without reallocating.
    void clear() noexcept { data_.clear(); }

    /// Explicit deep copy, since copying is otherwise disabled.
    [[nodiscard]] AudioChunk clone() const;

private:
    AudioFormat            format_{};
    std::vector<std::byte> data_;
};

}  // namespace xpcog
