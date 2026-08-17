// Port of Cog Plugins/Opus/Opus/OpusDecoder.m.
//
// Opus always decodes at 48 kHz regardless of the original sample rate; that is a
// property of the format, not a resampling choice made here.

#include "../common/VorbisComments.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"

#include <opus/opusfile.h>

#include <cstdio>
#include <memory>
#include <string_view>
#include <vector>

namespace xpcog {
namespace {

constexpr double kOpusSampleRate = 48000.0;
constexpr int    kFramesPerRead  = 1024;

/// Opus uses the Vorbis channel order, so the same permutation applies.
/// Cog OpusDecoder.m carries an identical table.
constexpr int kMaxMappedChannels = 8;
constexpr int kChannelMap[kMaxMappedChannels][kMaxMappedChannels] = {
    {0},
    {0, 1},
    {0, 2, 1},
    {0, 1, 2, 3},
    {0, 2, 1, 3, 4},
    {0, 2, 1, 5, 3, 4},
    {0, 2, 1, 6, 5, 3, 4},
    {0, 2, 1, 7, 5, 6, 3, 4},
};

class OpusFileDecoder final : public IDecoder {
public:
    ~OpusFileDecoder() override { OpusFileDecoder::close(); }

    bool open(ISource* source) override {
        close();
        source_ = source;
        if (source_ == nullptr) {
            return false;
        }

        OpusFileCallbacks callbacks{};
        callbacks.read  = &OpusFileDecoder::readCb;
        callbacks.seek  = &OpusFileDecoder::seekCb;
        callbacks.tell  = &OpusFileDecoder::tellCb;
        callbacks.close = nullptr;

        int error = 0;
        opus_     = op_open_callbacks(source_, &callbacks, nullptr, 0, &error);
        if (opus_ == nullptr || error != 0) {
            return false;
        }

        const OpusHead* head = op_head(opus_, -1);
        if (head == nullptr) {
            return false;
        }

        format_.channels      = static_cast<std::uint32_t>(head->channel_count);
        format_.sampleRate    = kOpusSampleRate;
        format_.format        = SampleFormat::F32;
        format_.bitsPerSample = 32;
        format_.channelConfig = guessChannelConfig(format_.channels);

        totalFrames_ = op_pcm_total(opus_, -1);
        seekable_    = op_seekable(opus_) != 0;
        bitrateKbps_ = static_cast<std::int32_t>(op_bitrate(opus_, -1) / 1000);
        framePos_    = 0;

        readTags();
        return format_.valid();
    }

    bool readAudio(AudioChunk& out) override {
        const double timestamp = static_cast<double>(framePos_) / kOpusSampleRate;
        const auto   channels  = static_cast<int>(format_.channels);

        planar_.resize(static_cast<std::size_t>(kFramesPerRead) * channels);
        interleaved_.resize(planar_.size());

        // op_read_float returns already-interleaved samples in Vorbis channel
        // order, so only the permutation is needed, not a transpose.
        const int got = op_read_float(opus_, planar_.data(),
                                      static_cast<int>(planar_.size()), nullptr);
        if (got <= 0) {
            return false;
        }

        if (channels <= kMaxMappedChannels) {
            for (int c = 0; c < channels; ++c) {
                const int sourceChannel = kChannelMap[channels - 1][c];
                for (int i = 0; i < got; ++i) {
                    interleaved_[static_cast<std::size_t>(i) * channels + c] =
                        planar_[static_cast<std::size_t>(i) * channels + sourceChannel];
                }
            }
        } else {
            std::copy_n(planar_.begin(), static_cast<std::size_t>(got) * channels,
                        interleaved_.begin());
        }

        out.clear();
        out.setFormat(format_);
        out.lossless        = false;
        out.streamTimestamp = timestamp;
        out.streamTimeRatio = 1.0;
        out.assign(interleaved_.data(), static_cast<std::size_t>(got));

        framePos_ += got;
        return true;
    }

    std::int64_t seek(std::int64_t frame) override {
        if (opus_ == nullptr || op_pcm_seek(opus_, frame) != 0) {
            return -1;
        }
        framePos_ = frame;
        return frame;
    }

    void close() override {
        if (opus_ != nullptr) {
            op_free(opus_);
            opus_ = nullptr;
        }
        source_ = nullptr;
    }

    void interrupt() override {
        if (source_ != nullptr) {
            source_->interrupt();
        }
    }

    [[nodiscard]] TrackProperties properties() const override {
        TrackProperties props;
        props.format      = format_;
        props.totalFrames = totalFrames_;
        props.bitrateKbps = bitrateKbps_;
        props.seekable    = seekable_ && source_ != nullptr && source_->seekable();
        props.lossless    = false;
        props.codec       = "Opus";
        props.encoding    = "lossy";
        props.replayGain  = replayGain_;
        return props;
    }

    [[nodiscard]] MetadataMap metadata() const override { return tags_; }

private:
    static int readCb(void* client, unsigned char* buffer, int bytes) {
        auto*              self = static_cast<ISource*>(client);
        const std::int64_t got  = self->read(buffer, bytes);
        return (got < 0) ? -1 : static_cast<int>(got);
    }

    static int seekCb(void* client, opus_int64 offset, int whence) {
        auto* self = static_cast<ISource*>(client);
        if (!self->seekable()) {
            return -1;
        }
        return self->seek(offset, whence) ? 0 : -1;
    }

    static opus_int64 tellCb(void* client) {
        return static_cast<ISource*>(client)->tell();
    }

    void readTags() {
        tags_.clear();
        replayGain_ = {};

        const OpusTags* tags = op_tags(opus_, -1);
        if (tags == nullptr) {
            return;
        }
        for (int i = 0; i < tags->comments; ++i) {
            codecs::applyVorbisComment(
                std::string_view{tags->user_comments[i],
                                 static_cast<std::size_t>(tags->comment_lengths[i])},
                tags_, replayGain_, &format_.channelConfig);
        }
    }

    OggOpusFile* opus_  = nullptr;
    ISource*     source_ = nullptr;

    AudioFormat        format_{};
    std::vector<float> planar_;
    std::vector<float> interleaved_;
    std::int64_t       totalFrames_ = 0;
    std::int64_t       framePos_    = 0;
    std::int32_t       bitrateKbps_ = 0;
    bool               seekable_    = false;

    MetadataMap    tags_;
    ReplayGainInfo replayGain_;
};

constexpr std::string_view kExtensions[] = {"opus"};
constexpr std::string_view kMimeTypes[]  = {"audio/opus", "audio/x-opus+ogg"};

}  // namespace
}  // namespace xpcog

void xpcog_register_opus(xpcog::PluginRegistry& r) {
    r.addDecoder({
        // Above default so that .ogg files carrying Opus are tried here before
        // the Vorbis decoder, which would reject them.
        .name       = "OpusDecoder",
        .priority   = 1.5F,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = xpcog::kMimeTypes,
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::OpusFileDecoder>();
        },
        .available = nullptr,
    });
}
