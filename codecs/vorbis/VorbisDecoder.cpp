// Port of Cog Plugins/Vorbis/VorbisDecoder.m.

#include "../common/VorbisComments.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"

#include <vorbis/vorbisfile.h>

#include <cstdio>
#include <memory>
#include <string_view>
#include <vector>

namespace xpcog {
namespace {

constexpr int kMaxMappedChannels = 8;
constexpr int kFramesPerRead     = 1024;

/// Vorbis orders channels differently from the interleaved layout the rest of the
/// chain expects, so each count needs its own permutation. Ported verbatim from
/// Cog VorbisDecoder.m:24-33 -- getting this wrong swaps centre and surround on
/// multichannel files, which is easy to miss on a stereo setup.
constexpr int kChannelMap[kMaxMappedChannels][kMaxMappedChannels] = {
    {0},                          // mono
    {0, 1},                       // l, r
    {0, 2, 1},                    // l, c, r          -> l, r, c
    {0, 1, 2, 3},                 // l, r, bl, br
    {0, 2, 1, 3, 4},              // l, c, r, bl, br  -> l, r, c, bl, br
    {0, 2, 1, 5, 3, 4},           // + lfe
    {0, 2, 1, 6, 5, 3, 4},        // 6.1
    {0, 2, 1, 7, 5, 6, 3, 4},     // 7.1
};

class VorbisDecoder final : public IDecoder {
public:
    ~VorbisDecoder() override { VorbisDecoder::close(); }

    bool open(ISource* source) override {
        close();
        source_ = source;
        if (source_ == nullptr) {
            return false;
        }

        ov_callbacks callbacks{};
        callbacks.read_func  = &VorbisDecoder::readCb;
        callbacks.seek_func  = &VorbisDecoder::seekCb;
        callbacks.close_func = nullptr;  // the source outlives us; nothing to do
        callbacks.tell_func  = &VorbisDecoder::tellCb;

        if (ov_open_callbacks(source_, &vorbis_, nullptr, 0, callbacks) != 0) {
            return false;
        }
        opened_ = true;

        const vorbis_info* info = ov_info(&vorbis_, -1);
        if (info == nullptr) {
            return false;
        }

        format_.channels      = static_cast<std::uint32_t>(info->channels);
        format_.sampleRate    = static_cast<double>(info->rate);
        format_.format        = SampleFormat::F32;
        format_.bitsPerSample = 32;
        format_.channelConfig = guessChannelConfig(format_.channels);

        bitrateKbps_ = static_cast<std::int32_t>(info->bitrate_nominal / 1000);
        seekable_    = ov_seekable(&vorbis_) != 0;
        totalFrames_ = static_cast<std::int64_t>(ov_pcm_total(&vorbis_, -1));
        framePos_    = 0;

        readTags();
        return format_.valid();
    }

    bool readAudio(AudioChunk& out) override {
        const double timestamp = (format_.sampleRate > 0.0)
                                     ? static_cast<double>(framePos_) / format_.sampleRate
                                     : 0.0;

        const auto channels = static_cast<int>(format_.channels);
        interleaved_.resize(static_cast<std::size_t>(kFramesPerRead) * channels);

        int total = 0;
        for (;;) {
            float** pcm     = nullptr;
            int     section = currentSection_;
            const long got  = ov_read_float(&vorbis_, &pcm, kFramesPerRead - total,
                                            &section);
            if (got <= 0) {
                break;
            }

            // Chained stream: the format can change between links.
            if (section != currentSection_) {
                currentSection_ = section;
                if (const vorbis_info* info = ov_info(&vorbis_, -1)) {
                    if (static_cast<std::uint32_t>(info->channels) != format_.channels ||
                        static_cast<double>(info->rate) != format_.sampleRate) {
                        format_.channels   = static_cast<std::uint32_t>(info->channels);
                        format_.sampleRate = static_cast<double>(info->rate);
                        format_.channelConfig = guessChannelConfig(format_.channels);
                        notifyChanged(true, false);
                    }
                }
                readTags();
                if (total > 0) {
                    break;
                }
                continue;
            }

            for (int c = 0; c < channels; ++c) {
                const int sourceChannel = (channels <= kMaxMappedChannels)
                                              ? kChannelMap[channels - 1][c]
                                              : c;
                for (long i = 0; i < got; ++i) {
                    interleaved_[static_cast<std::size_t>(total + i) * channels + c] =
                        pcm[sourceChannel][i];
                }
            }

            total += static_cast<int>(got);
            if (total >= kFramesPerRead) {
                break;
            }
        }

        if (total <= 0) {
            return false;
        }

        out.clear();
        out.setFormat(format_);
        out.lossless        = false;
        out.streamTimestamp = timestamp;
        out.streamTimeRatio = 1.0;
        out.assign(interleaved_.data(), static_cast<std::size_t>(total));

        framePos_ += total;
        return true;
    }

    std::int64_t seek(std::int64_t frame) override {
        if (!opened_ || ov_pcm_seek(&vorbis_, frame) != 0) {
            return -1;
        }
        framePos_ = frame;
        return frame;
    }

    void close() override {
        if (opened_) {
            ov_clear(&vorbis_);
            opened_ = false;
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
        props.codec       = "Ogg Vorbis";
        props.encoding    = "lossy";
        props.replayGain  = replayGain_;
        return props;
    }

    [[nodiscard]] MetadataMap metadata() const override { return tags_; }

private:
    static std::size_t readCb(void* buffer, std::size_t size, std::size_t count,
                              void* client) {
        auto*              self  = static_cast<ISource*>(client);
        const std::int64_t bytes = static_cast<std::int64_t>(size * count);
        const std::int64_t got   = self->read(buffer, bytes);
        return (got <= 0) ? 0 : static_cast<std::size_t>(got) / size;
    }

    static int seekCb(void* client, ogg_int64_t offset, int whence) {
        auto* self = static_cast<ISource*>(client);
        if (!self->seekable()) {
            return -1;
        }
        return self->seek(offset, whence) ? 0 : -1;
    }

    static long tellCb(void* client) {
        return static_cast<long>(static_cast<ISource*>(client)->tell());
    }

    void readTags() {
        tags_.clear();
        replayGain_ = {};

        const vorbis_comment* comments = ov_comment(&vorbis_, -1);
        if (comments == nullptr) {
            return;
        }
        for (int i = 0; i < comments->comments; ++i) {
            codecs::applyVorbisComment(
                std::string_view{comments->user_comments[i],
                                 static_cast<std::size_t>(comments->comment_lengths[i])},
                tags_, replayGain_, &format_.channelConfig);
        }
    }

    OggVorbis_File vorbis_{};
    ISource*       source_ = nullptr;
    bool           opened_ = false;

    AudioFormat        format_{};
    std::vector<float> interleaved_;
    std::int64_t       totalFrames_    = 0;
    std::int64_t       framePos_       = 0;
    std::int32_t       bitrateKbps_    = 0;
    int                currentSection_ = 0;
    bool               seekable_       = false;

    MetadataMap    tags_;
    ReplayGainInfo replayGain_;
};

constexpr std::string_view kExtensions[] = {"ogg", "oga"};
constexpr std::string_view kMimeTypes[]  = {"application/ogg", "audio/ogg",
                                            "audio/x-vorbis+ogg"};

}  // namespace
}  // namespace xpcog

void xpcog_register_vorbis(xpcog::PluginRegistry& r) {
    r.addDecoder({
        .name       = "VorbisDecoder",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = xpcog::kMimeTypes,
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::VorbisDecoder>();
        },
        .available = nullptr,
    });
}
