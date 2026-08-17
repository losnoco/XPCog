// MP3 via libmpg123.
//
// Cog uses minimp3 here (Plugins/minimp3). mpg123 is used instead because it is a
// proper vcpkg dependency on all three platforms, handles Xing/VBRI/LAME headers
// and gapless padding itself, and is the more conservative choice for a format
// with as much malformed material in the wild as MP3.

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"

#include <mpg123.h>

#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace xpcog {
namespace {

/// mpg123 needs a process-wide init exactly once. Later versions make
/// mpg123_init() a no-op, but calling it once is correct for every version.
void ensureMpg123Init() {
    static std::once_flag once;
    std::call_once(once, [] { mpg123_init(); });
}

class Mpg123Decoder final : public IDecoder {
public:
    ~Mpg123Decoder() override { Mpg123Decoder::close(); }

    bool open(ISource* source) override {
        close();
        ensureMpg123Init();

        source_ = source;
        if (source_ == nullptr) {
            return false;
        }

        int error = MPG123_OK;
        handle_   = mpg123_new(nullptr, &error);
        if (handle_ == nullptr) {
            return false;
        }

        // Quiet, and let mpg123 apply Xing/LAME gapless trimming.
        mpg123_param(handle_, MPG123_ADD_FLAGS, MPG123_QUIET | MPG123_GAPLESS, 0.0);

        // Advertise float32 at every rate libmpg123 supports, before opening.
        // Doing this after open (or leaving it at none) makes format negotiation
        // fail outright with "Unable to set up output format".
        mpg123_format_none(handle_);
        {
            const long* rates = nullptr;
            std::size_t count = 0;
            mpg123_rates(&rates, &count);
            for (std::size_t i = 0; i < count; ++i) {
                mpg123_format(handle_, rates[i], MPG123_MONO | MPG123_STEREO,
                              MPG123_ENC_FLOAT_32);
            }
        }

        if (mpg123_replace_reader_handle(handle_, &Mpg123Decoder::readCb,
                                         &Mpg123Decoder::seekCb, nullptr) != MPG123_OK) {
            return false;
        }
        if (mpg123_open_handle(handle_, source_) != MPG123_OK) {
            return false;
        }
        opened_ = true;

        long         rate     = 0;
        int          channels = 0;
        int          encoding = 0;
        if (mpg123_getformat(handle_, &rate, &channels, &encoding) != MPG123_OK) {
            return false;
        }

        if (encoding != MPG123_ENC_FLOAT_32) {
            return false;
        }

        format_.sampleRate    = static_cast<double>(rate);
        format_.channels      = static_cast<std::uint32_t>(channels);
        format_.format        = SampleFormat::F32;
        format_.bitsPerSample = 32;
        format_.channelConfig = guessChannelConfig(format_.channels);

        if (source_->seekable()) {
            mpg123_scan(handle_);
            const off_t length = mpg123_length(handle_);
            totalFrames_ = (length > 0) ? static_cast<std::int64_t>(length) : 0;
        }

        mpg123_frameinfo info{};
        if (mpg123_info(handle_, &info) == MPG123_OK) {
            bitrateKbps_ = info.bitrate;
        }

        framePos_ = 0;
        return format_.valid();
    }

    bool readAudio(AudioChunk& out) override {
        const double timestamp = (format_.sampleRate > 0.0)
                                     ? static_cast<double>(framePos_) / format_.sampleRate
                                     : 0.0;

        buffer_.resize(4096 * format_.channels);

        std::size_t produced = 0;
        const int   status   = mpg123_read(handle_,
                                           reinterpret_cast<unsigned char*>(buffer_.data()),
                                           buffer_.size() * sizeof(float), &produced);

        if (status == MPG123_NEW_FORMAT) {
            long rate = 0; int channels = 0; int encoding = 0;
            if (mpg123_getformat(handle_, &rate, &channels, &encoding) == MPG123_OK) {
                format_.sampleRate    = static_cast<double>(rate);
                format_.channels      = static_cast<std::uint32_t>(channels);
                format_.channelConfig = guessChannelConfig(format_.channels);
                notifyChanged(true, false);
            }
        } else if (status != MPG123_OK && status != MPG123_DONE) {
            return false;
        }

        if (produced == 0) {
            return false;
        }

        const std::size_t frames = produced / sizeof(float) / format_.channels;
        if (frames == 0) {
            return false;
        }

        out.clear();
        out.setFormat(format_);
        out.lossless        = false;
        out.streamTimestamp = timestamp;
        out.streamTimeRatio = 1.0;
        out.assign(buffer_.data(), frames);

        framePos_ += static_cast<std::int64_t>(frames);
        return true;
    }

    std::int64_t seek(std::int64_t frame) override {
        if (!opened_) {
            return -1;
        }
        const off_t got = mpg123_seek(handle_, static_cast<off_t>(frame), SEEK_SET);
        if (got < 0) {
            return -1;
        }
        framePos_ = static_cast<std::int64_t>(got);
        return framePos_;
    }

    void close() override {
        if (handle_ != nullptr) {
            if (opened_) {
                mpg123_close(handle_);
                opened_ = false;
            }
            mpg123_delete(handle_);
            handle_ = nullptr;
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
        props.lossless    = false;
        props.codec       = "MP3";
        props.encoding    = "lossy";
        return props;
    }

private:
    static ssize_t readCb(void* client, void* buffer, size_t bytes) {
        auto*              self = static_cast<ISource*>(client);
        const std::int64_t got  = self->read(buffer, static_cast<std::int64_t>(bytes));
        return (got < 0) ? -1 : static_cast<ssize_t>(got);
    }

    static off_t seekCb(void* client, off_t offset, int whence) {
        auto* self = static_cast<ISource*>(client);
        if (!self->seekable() || !self->seek(offset, whence)) {
            return -1;
        }
        return static_cast<off_t>(self->tell());
    }

    mpg123_handle* handle_ = nullptr;
    ISource*       source_ = nullptr;
    bool           opened_ = false;

    AudioFormat        format_{};
    std::vector<float> buffer_;
    std::int64_t       totalFrames_ = 0;
    std::int64_t       framePos_    = 0;
    int                bitrateKbps_ = 0;
};

constexpr std::string_view kExtensions[] = {"mp3", "mp2", "mpa", "m2a"};
constexpr std::string_view kMimeTypes[]  = {"audio/mpeg", "audio/mp3", "audio/x-mp3"};

}  // namespace
}  // namespace xpcog

void xpcog_register_mpg123(xpcog::PluginRegistry& r) {
    r.addDecoder({
        .name       = "Mpg123Decoder",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = xpcog::kMimeTypes,
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::Mpg123Decoder>();
        },
        .available = nullptr,
    });
}
