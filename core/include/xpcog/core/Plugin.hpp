// The decoder contract, replacing Cog's Objective-C protocols in Audio/Plugin.h.
//
// These are complete types rather than forward declarations because
// std::unique_ptr's default deleter requires a complete type at every point the
// smart pointer is instantiated or destroyed. libc++ tolerates the forward
// declaration in some contexts; libstdc++ and MinGW correctly reject it.

#pragma once

#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/MetadataMap.hpp"
#include "xpcog/core/TrackProperties.hpp"
#include "xpcog/core/Url.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace xpcog {

class PluginRegistry;

/// Byte-oriented input, mirroring Cog's CogSource protocol.
/// Implementations: FileSource (M1a); HTTPSource and ArchiveSource (M6).
class ISource {
public:
    ISource()          = default;
    virtual ~ISource() = default;

    ISource(const ISource&)            = delete;
    ISource& operator=(const ISource&) = delete;

    virtual bool open(const Url& url) = 0;

    [[nodiscard]] virtual bool seekable() const = 0;

    /// `whence` takes the usual SEEK_SET / SEEK_CUR / SEEK_END values.
    virtual bool seek(std::int64_t offset, int whence) = 0;

    [[nodiscard]] virtual std::int64_t tell() const = 0;

    /// Reads up to `bytes`; returns the number actually read, 0 at end of input,
    /// negative on error.
    virtual std::int64_t read(void* buffer, std::int64_t bytes) = 0;

    virtual void close() = 0;

    [[nodiscard]] virtual const Url& url() const = 0;

    /// Empty unless the transport reports one (HTTP Content-Type).
    [[nodiscard]] virtual std::string mimeType() const { return {}; }

    /// Unblocks an in-flight read when playback is stopping. Teardown still
    /// happens in close(), on the reader's own thread.
    virtual void interrupt() {}
};

using SourcePtr = std::unique_ptr<ISource>;

/// Audio decoding, mirroring Cog's CogDecoder protocol.
class IDecoder {
public:
    IDecoder()          = default;
    virtual ~IDecoder() = default;

    IDecoder(const IDecoder&)            = delete;
    IDecoder& operator=(const IDecoder&) = delete;

    /// The decoder borrows `source` and must not outlive it.
    virtual bool open(ISource* source) = 0;

    [[nodiscard]] virtual TrackProperties properties() const = 0;

    /// Tags that can change mid-stream (ICY, chained Ogg). Static tags come from
    /// the metadata readers instead.
    [[nodiscard]] virtual MetadataMap metadata() const { return {}; }

    /// Fills `out` with the next block. Returns false at end of stream.
    /// `out` may carry recycled storage and its previous contents are irrelevant.
    virtual bool readAudio(AudioChunk& out) = 0;

    /// Seeks to `frame`; returns the frame actually reached, or -1 on failure.
    virtual std::int64_t seek(std::int64_t frame) = 0;

    virtual void close() = 0;

    virtual void interrupt() {}

    /// Selects a track within a container (cue sheet, multi-song archive).
    virtual bool setTrack(const Url& /*track*/) { return false; }

    /// True for the silence decoder, which stands in for unplayable input so a
    /// broken file does not stall the playlist. Mirrors Cog's -isSilence.
    [[nodiscard]] virtual bool isSilence() const { return false; }

    /// Called by the registry immediately after construction, before open().
    ///
    /// Decoders that must open a *second* file keep it -- a cue sheet decodes the
    /// audio file its sheet points at, and archive members work the same way.
    /// Cog reaches for the AudioSource/AudioDecoder singletons instead; passing
    /// the registry keeps that dependency explicit and testable.
    /// Every other decoder ignores this.
    virtual void setRegistry(const PluginRegistry* /*registry*/) {}

    /// Replaces the KVO on @"properties"/@"metadata" that Cog's InputNode
    /// registers (Audio/Chain/InputNode.m). Called from the decode thread.
    using ChangeCallback = std::function<void(bool propertiesChanged,
                                              bool metadataChanged)>;
    virtual void setChangeCallback(ChangeCallback callback) {
        onChange_ = std::move(callback);
    }

protected:
    /// Implementations call this when properties or metadata change mid-stream.
    void notifyChanged(bool propertiesChanged, bool metadataChanged) const {
        if (onChange_) {
            onChange_(propertiesChanged, metadataChanged);
        }
    }

private:
    ChangeCallback onChange_;
};

using DecoderPtr = std::unique_ptr<IDecoder>;

}  // namespace xpcog
