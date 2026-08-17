// Port of Cog Plugins/WavPack/WavPackDecoder.m.
//
// WavPack always hands back int32 samples, one per int32 regardless of the source
// bit depth, so they are shifted down to the container the rest of the chain
// expects. Cog does the same normalisation.

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"

#include <wavpack/wavpack.h>

#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

namespace xpcog {
namespace {

constexpr int kFramesPerRead = 1024;

class WavPackDecoder final : public IDecoder {
public:
    ~WavPackDecoder() override { WavPackDecoder::close(); }

    bool open(ISource* source) override {
        close();
        source_ = source;
        if (source_ == nullptr) {
            return false;
        }

        reader_.read_bytes     = &WavPackDecoder::readBytes;
        reader_.get_pos        = &WavPackDecoder::getPos;
        reader_.set_pos_abs    = &WavPackDecoder::setPosAbs;
        reader_.set_pos_rel    = &WavPackDecoder::setPosRel;
        reader_.push_back_byte = &WavPackDecoder::pushBackByte;
        reader_.get_length     = &WavPackDecoder::getLength;
        reader_.can_seek       = &WavPackDecoder::canSeek;
        reader_.write_bytes    = nullptr;

        char error[80] = {0};
        // No correction file: .wvc would need a second source, which the plugin
        // API has no way to request. Cog has the same limitation.
        context_ = WavpackOpenFileInputEx(&reader_, source_, nullptr, error,
                                          OPEN_NORMALIZE | OPEN_TAGS, 0);
        if (context_ == nullptr) {
            return false;
        }

        format_.channels   = static_cast<std::uint32_t>(WavpackGetNumChannels(context_));
        format_.sampleRate = static_cast<double>(WavpackGetSampleRate(context_));
        bitsPerSample_     = WavpackGetBitsPerSample(context_);
        format_.bitsPerSample = static_cast<std::uint32_t>(bitsPerSample_);

        const int mode = WavpackGetMode(context_);
        // MODE_FLOAT is the documented float test. GetFloatNormExp reports the
        // normalisation exponent and is non-zero for integer files too, so using
        // it here mislabelled every 16-bit file as float32.
        isFloat_ = (mode & MODE_FLOAT) != 0;

        if (isFloat_) {
            format_.format = SampleFormat::F32;
            format_.bitsPerSample = 32;
        } else if (bitsPerSample_ <= 8) {
            format_.format = SampleFormat::S8;
        } else if (bitsPerSample_ <= 16) {
            format_.format = SampleFormat::S16;
        } else if (bitsPerSample_ <= 24) {
            format_.format = SampleFormat::S24;
        } else {
            format_.format = SampleFormat::S32;
        }

        const int mask = WavpackGetChannelMask(context_);
        format_.channelConfig = (mask != 0) ? static_cast<std::uint32_t>(mask)
                                            : guessChannelConfig(format_.channels);

        totalFrames_ = static_cast<std::int64_t>(WavpackGetNumSamples64(context_));
        lossless_    = (mode & MODE_LOSSLESS) != 0;
        bitrateKbps_ =
            static_cast<std::int32_t>(WavpackGetAverageBitrate(context_, 1) / 1000.0);
        framePos_ = 0;

        return format_.valid();
    }

    bool readAudio(AudioChunk& out) override {
        const double timestamp = (format_.sampleRate > 0.0)
                                     ? static_cast<double>(framePos_) / format_.sampleRate
                                     : 0.0;

        const auto channels = static_cast<std::size_t>(format_.channels);
        raw_.resize(static_cast<std::size_t>(kFramesPerRead) * channels);

        const uint32_t got =
            WavpackUnpackSamples(context_, raw_.data(), kFramesPerRead);
        if (got == 0) {
            return false;
        }

        const std::size_t samples = static_cast<std::size_t>(got) * channels;

        out.clear();
        out.setFormat(format_);
        out.lossless        = lossless_;
        out.streamTimestamp = timestamp;
        out.streamTimeRatio = 1.0;

        std::byte* dst = out.allocFrames(got);

        if (isFloat_ || format_.format == SampleFormat::S32) {
            // Already the right width; float samples arrive bit-identical in the
            // int32 buffer, so a straight copy is correct for both.
            std::memcpy(dst, raw_.data(), samples * 4);
        } else if (format_.format == SampleFormat::S24) {
            for (std::size_t i = 0; i < samples; ++i) {
                const std::int32_t v = raw_[i];
                dst[i * 3 + 0] = static_cast<std::byte>(v & 0xFF);
                dst[i * 3 + 1] = static_cast<std::byte>((v >> 8) & 0xFF);
                dst[i * 3 + 2] = static_cast<std::byte>((v >> 16) & 0xFF);
            }
        } else if (format_.format == SampleFormat::S16) {
            for (std::size_t i = 0; i < samples; ++i) {
                const auto v = static_cast<std::int16_t>(raw_[i]);
                std::memcpy(dst + i * 2, &v, 2);
            }
        } else {
            for (std::size_t i = 0; i < samples; ++i) {
                const auto v = static_cast<std::int8_t>(raw_[i]);
                std::memcpy(dst + i, &v, 1);
            }
        }

        framePos_ += got;
        return true;
    }

    std::int64_t seek(std::int64_t frame) override {
        if (context_ == nullptr ||
            !WavpackSeekSample64(context_, static_cast<int64_t>(frame))) {
            return -1;
        }
        framePos_ = frame;
        return frame;
    }

    void close() override {
        if (context_ != nullptr) {
            WavpackCloseFile(context_);
            context_ = nullptr;
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
        props.seekable    = source_ != nullptr && source_->seekable();
        props.lossless    = lossless_;
        props.codec       = "WavPack";
        props.encoding    = lossless_ ? "lossless" : "lossy";
        return props;
    }

private:
    static ISource* src(void* client) { return static_cast<ISource*>(client); }

    static int32_t readBytes(void* client, void* data, int32_t count) {
        return static_cast<int32_t>(src(client)->read(data, count));
    }
    static uint32_t getPos(void* client) {
        return static_cast<uint32_t>(src(client)->tell());
    }
    static int setPosAbs(void* client, uint32_t position) {
        return src(client)->seek(position, SEEK_SET) ? 0 : -1;
    }
    static int setPosRel(void* client, int32_t delta, int mode) {
        return src(client)->seek(delta, mode) ? 0 : -1;
    }
    static int pushBackByte(void* client, int c) {
        // WavPack only ever pushes back the byte it just read, so stepping the
        // source back one is sufficient and avoids an extra buffering layer.
        auto* source = src(client);
        if (!source->seek(-1, SEEK_CUR)) {
            return EOF;
        }
        return c;
    }
    static uint32_t getLength(void* client) {
        auto* source = src(client);
        if (!source->seekable()) {
            return 0;
        }
        const std::int64_t saved = source->tell();
        source->seek(0, SEEK_END);
        const std::int64_t length = source->tell();
        source->seek(saved, SEEK_SET);
        return static_cast<uint32_t>(length);
    }
    static int canSeek(void* client) { return src(client)->seekable() ? 1 : 0; }

    WavpackStreamReader reader_{};
    WavpackContext*     context_ = nullptr;
    ISource*            source_  = nullptr;

    AudioFormat               format_{};
    std::vector<std::int32_t> raw_;
    std::int64_t              totalFrames_   = 0;
    std::int64_t              framePos_      = 0;
    std::int32_t              bitrateKbps_   = 0;
    int                       bitsPerSample_ = 0;
    bool                      lossless_      = true;
    bool                      isFloat_       = false;
};

constexpr std::string_view kExtensions[] = {"wv"};
constexpr std::string_view kMimeTypes[]  = {"audio/x-wavpack", "audio/wavpack"};

}  // namespace
}  // namespace xpcog

void xpcog_register_wavpack(xpcog::PluginRegistry& r) {
    r.addDecoder({
        .name       = "WavPackDecoder",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = xpcog::kMimeTypes,
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::WavPackDecoder>();
        },
        .available = nullptr,
    });
}
