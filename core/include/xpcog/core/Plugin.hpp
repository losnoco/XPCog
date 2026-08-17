// The decoder contract, replacing Cog's Objective-C protocols in Audio/Plugin.h.
//
// These are complete types rather than forward declarations because
// std::unique_ptr's default deleter requires a complete type at every point the
// smart pointer is instantiated or destroyed. libc++ tolerates the forward
// declaration in some contexts; libstdc++ and MinGW correctly reject it.
//
// The method sets are filled out in M1a alongside Url, AudioChunk,
// TrackProperties and MetadataMap. What is declared here is only what those
// value types are not needed for.

#pragma once

#include <cstdint>

namespace xpcog {

/// Byte-oriented input, mirroring Cog's CogSource protocol.
/// Implementations: FileSource (M1a), HTTPSource and ArchiveSource (M6).
class ISource {
public:
    ISource()          = default;
    virtual ~ISource() = default;

    ISource(const ISource&)            = delete;
    ISource& operator=(const ISource&) = delete;

    [[nodiscard]] virtual bool seekable() const = 0;

    /// `whence` takes the usual SEEK_SET / SEEK_CUR / SEEK_END values.
    virtual bool seek(std::int64_t offset, int whence) = 0;

    [[nodiscard]] virtual std::int64_t tell() const = 0;

    /// Reads up to `bytes`; returns the number actually read, 0 at end of input.
    virtual std::int64_t read(void* buffer, std::int64_t bytes) = 0;

    virtual void close() = 0;

    /// Unblocks an in-flight read when playback is stopping. Teardown still
    /// happens in close(), on the reader's own thread. Optional in Cog; a
    /// no-op default here.
    virtual void interrupt() {}

    // M1a adds: open(const Url&), url(), mimeType().
};

/// Audio decoding, mirroring Cog's CogDecoder protocol.
class IDecoder {
public:
    IDecoder()          = default;
    virtual ~IDecoder() = default;

    IDecoder(const IDecoder&)            = delete;
    IDecoder& operator=(const IDecoder&) = delete;

    /// Seeks to `frame`; returns the frame actually reached.
    virtual std::int64_t seek(std::int64_t frame) = 0;

    virtual void close() = 0;

    /// Unblocks an in-flight readAudio() when playback is stopping.
    virtual void interrupt() {}

    /// True for the silence decoder, which stands in for unplayable input so a
    /// broken file does not stall the playlist. Mirrors Cog's -isSilence.
    [[nodiscard]] virtual bool isSilence() const { return false; }

    // M1a adds: open(ISource*), readAudio(AudioChunk&), properties(),
    //           metadata(), setChangeCallback().
};

}  // namespace xpcog
