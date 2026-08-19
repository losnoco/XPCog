// See HlsDecoder.cpp. Declared in a header rather than kept anonymous because
// the memory source and the segment manager are held by value here, and both
// need complete types at the point this class is destroyed.

#pragma once

#include "HlsMemorySource.hpp"
#include "HlsPlaylist.hpp"
#include "HlsSegmentManager.hpp"

#include "xpcog/core/Plugin.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace xpcog {

class HlsDecoder final : public IDecoder {
public:
    HlsDecoder() = default;
    ~HlsDecoder() override;

    void setRegistry(const PluginRegistry* registry) override { registry_ = registry; }

    bool open(ISource* source) override;
    [[nodiscard]] TrackProperties properties() const override;
    [[nodiscard]] MetadataMap     metadata() const override;
    bool readAudio(AudioChunk& out) override;
    std::int64_t seek(std::int64_t frame) override;
    void close() override;
    void interrupt() override;

private:
    /// Fetches and parses a manifest at `url`, for following a master playlist
    /// to the rendition it names.
    [[nodiscard]] std::optional<codecs::HlsPlaylist> fetchPlaylist(const Url& url) const;

    /// Asks the registry for the decoder that claims the memory source's current
    /// identity and opens it. Also used after a seek, which reopens from scratch.
    [[nodiscard]] bool openInner();

    const PluginRegistry* registry_ = nullptr;

    std::unique_ptr<codecs::HlsMemorySource>   memory_;
    std::unique_ptr<codecs::HlsSegmentManager> manager_;
    DecoderPtr                                 inner_;

    bool         live_        = false;
    std::int64_t totalFrames_ = 0;
    /// Frames to discard after a seek, which can only land on a segment boundary.
    std::int64_t pendingSkipFrames_ = 0;
};

}  // namespace xpcog
