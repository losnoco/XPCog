// Port of Cog Audio/CogPluginMulti.m (CogDecoderMulti).
//
// When more than one decoder claims an extension or MIME type, each is tried in
// descending priority order until one opens successfully. Everything after open()
// forwards to whichever won.

#include "xpcog/core/PluginRegistry.hpp"

#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

namespace xpcog {
namespace {

class MultiDecoder final : public IDecoder {
public:
    explicit MultiDecoder(std::vector<const DecoderDescriptor*> candidates)
        : candidates_(std::move(candidates)) {}

    bool open(ISource* source) override {
        for (const DecoderDescriptor* candidate : candidates_) {
            DecoderPtr decoder = candidate->create();
            if (!decoder) {
                continue;
            }

            decoder->setChangeCallback(onChange_);
            decoder->setRegistry(registry_);
            if (decoder->open(source)) {
                active_ = std::move(decoder);
                return true;
            }

            // Rewind for the next candidate. Cog notes the HTTP source supports
            // only limited rewinding, so a failure here is not fatal on its own.
            source->seek(0, SEEK_SET);
        }
        return false;
    }

    [[nodiscard]] TrackProperties properties() const override {
        return active_ ? active_->properties() : TrackProperties{};
    }

    [[nodiscard]] MetadataMap metadata() const override {
        return active_ ? active_->metadata() : MetadataMap{};
    }

    bool readAudio(AudioChunk& out) override {
        return active_ && active_->readAudio(out);
    }

    std::int64_t seek(std::int64_t frame) override {
        return active_ ? active_->seek(frame) : -1;
    }

    void close() override {
        if (active_) {
            active_->close();
        }
    }

    void interrupt() override {
        if (active_) {
            active_->interrupt();
        }
    }

    bool setTrack(const Url& track) override {
        return active_ && active_->setTrack(track);
    }

    [[nodiscard]] bool isSilence() const override {
        return active_ && active_->isSilence();
    }

    void setChangeCallback(ChangeCallback callback) override {
        onChange_ = std::move(callback);
        if (active_) {
            active_->setChangeCallback(onChange_);
        }
    }

    void setRegistry(const PluginRegistry* registry) override {
        registry_ = registry;
        if (active_) {
            active_->setRegistry(registry);
        }
    }

private:
    std::vector<const DecoderDescriptor*> candidates_;
    DecoderPtr                            active_;
    ChangeCallback                        onChange_;
    const PluginRegistry*                 registry_ = nullptr;
};

}  // namespace

DecoderPtr makeMultiDecoder(std::vector<const DecoderDescriptor*> candidates) {
    return std::make_unique<MultiDecoder>(std::move(candidates));
}

}  // namespace xpcog
