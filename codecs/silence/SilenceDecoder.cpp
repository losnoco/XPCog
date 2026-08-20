// `silence://<seconds>` -- a track that is a measured amount of nothing.
//
// Port of Cog Plugins/SilenceDecoder, which is two halves: a source registered
// for the `silence` scheme that reads zeros, and a decoder selected by the MIME
// type that source reports. Neither reads the other; the source exists so the
// registry has something to open, and the decoder gets the duration off the URL.
//
// **What it is for is not obvious from the code.** Nothing in Cog's interface
// offers to insert a gap. The two places that build one of these URLs are both
// failure paths: `PlaylistEntry.m`'s urlForPath() when an entry has no path at
// all, and `BufferChain.m` when a source will not open. Ten seconds of silence
// instead of a stall -- the playlist keeps moving and the listener hears the
// track is broken rather than watching the player stop.
//
// XPCog does not wire either of those yet, and deliberately: `PlaybackController`
// answers a failed open by moving on and remembering the failure
// (`failedStarts_`), which is a different and mostly better answer than playing
// a gap. This is the piece that makes the other one *possible*, and it is also
// what lets a Cog playlist holding `silence://10` -- which `cogimport` will
// eventually read -- play the same thing here.
//
// Two differences from Cog:
//
//   * **Fractional seconds work.** Cog parses with -intValue, so `silence://2.5`
//     is two seconds there. Nothing Cog writes has a fraction in it, so no
//     existing URL changes meaning; a gap between tracks is very often not a
//     whole number of seconds, and this costs one strtod.
//
//   * **The track ends.** Cog clamps the frame count to what is left and then
//     hands back a zero-frame chunk for ever once that runs out, rather than
//     saying it has finished. Here the read returns false.

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace xpcog {
namespace {

constexpr std::string_view kSchemeList[] = {"silence"};
constexpr std::span<const std::string_view> kSchemes{kSchemeList};

constexpr std::string_view kMimeType = "audio/x-silence";
constexpr std::string_view kMimeTypeList[] = {kMimeType};
constexpr std::span<const std::string_view> kMimeTypes{kMimeTypeList};

/// Cog's, and there is no reason for it to be configurable: silence sounds the
/// same at every rate, and matching the most common device rate means the
/// resampler has nothing to do.
constexpr double        kSampleRate = 44100.0;
constexpr std::uint32_t kChannels   = 2;

/// What Cog plays when the URL says nothing useful -- including `silence://`
/// with no number at all, which is what its own failure paths write when they
/// have no better answer.
constexpr double kDefaultSeconds = 10.0;

/// An hour. Not a limit anybody should meet, but the duration comes off a URL
/// that may have been typed, and `silence://99999999999` should not turn into a
/// frame count that overflows the arithmetic downstream.
constexpr double kMaxSeconds = 3600.0;

/// The number in `silence://<seconds>`.
///
/// Read off the serialized URL rather than out of a path component, because
/// there is no path: the body is `//30`, where a normal URL would have a host.
/// Cog does the same thing with -substringFromIndex:10, counting the characters
/// in "silence://" by hand.
[[nodiscard]] double secondsFrom(const Url& url) {
    const std::string text = url.toString();
    std::string_view  rest{text};

    // "silence:" then, optionally, the "//" that makes it look like an
    // authority. Both forms are accepted -- `silence:10` parses to the same URL
    // and there is no reason to refuse it.
    const std::size_t colon = rest.find(':');
    if (colon == std::string_view::npos) {
        return kDefaultSeconds;
    }
    rest.remove_prefix(colon + 1);
    if (rest.starts_with("//")) {
        rest.remove_prefix(2);
    }
    if (const std::size_t hash = rest.find('#'); hash != std::string_view::npos) {
        rest = rest.substr(0, hash);
    }
    if (rest.empty()) {
        return kDefaultSeconds;
    }

    // strtod wants a terminator, and a string_view into the URL has none.
    const std::string  digits{rest};
    char*              end   = nullptr;
    const double       value = std::strtod(digits.c_str(), &end);

    // Anything unparseable, negative, or zero falls back rather than producing a
    // track of no length -- which is what Cog's -intValue does for the same
    // inputs, and is the more useful answer for a URL that exists because
    // something else already went wrong.
    if (end == digits.c_str() || !(value > 0.0)) {
        return kDefaultSeconds;
    }
    return std::min(value, kMaxSeconds);
}

/// A source that is all zeros, so the registry has something to open.
///
/// The decoder never reads it -- it synthesises from the URL -- but the registry
/// opens a source before it chooses a decoder, and the MIME type this reports is
/// what chooses one. Reads succeed for ever, which is what makes it safe for
/// anything that probes a few bytes before deciding.
class SilenceSource final : public ISource {
public:
    bool open(const Url& url) override {
        url_ = url;
        return true;
    }

    [[nodiscard]] bool seekable() const override { return true; }

    bool seek(std::int64_t /*offset*/, int /*whence*/) override { return true; }

    [[nodiscard]] std::int64_t tell() const override { return 0; }

    std::int64_t read(void* buffer, std::int64_t bytes) override {
        if (bytes < 0) {
            return -1;
        }
        if (buffer != nullptr && bytes > 0) {
            std::memset(buffer, 0, static_cast<std::size_t>(bytes));
        }
        return bytes;
    }

    void close() override {}

    [[nodiscard]] const Url& url() const override { return url_; }

    [[nodiscard]] std::string mimeType() const override {
        return std::string{kMimeType};
    }

private:
    Url url_;
};

class SilenceDecoder final : public IDecoder {
public:
    void setSettings(const Settings* settings) override { settings_ = settings; }

    bool open(ISource* source) override {
        if (source == nullptr) {
            return false;
        }
        totalFrames_ = static_cast<std::int64_t>(secondsFrom(source->url()) * kSampleRate);
        framePos_    = 0;

        format_.sampleRate    = kSampleRate;
        format_.channels      = kChannels;
        format_.channelConfig = 0x3;  // FL | FR
        format_.format        = SampleFormat::F32;
        format_.bitsPerSample = 32;
        return totalFrames_ > 0;
    }

    [[nodiscard]] TrackProperties properties() const override {
        TrackProperties props;
        props.format      = format_;
        props.totalFrames = totalFrames_;
        props.seekable    = true;
        props.lossless    = false;
        props.codec       = "Silence";
        props.encoding    = "synthesized";
        return props;
    }

    [[nodiscard]] MetadataMap metadata() const override { return {}; }

    bool readAudio(AudioChunk& out) override {
        // Repeat-one on a silent track means silence for ever, which is what
        // Cog does and is not as odd as it looks: it is how you hold a chain
        // open while something else is sorted out.
        const bool endless = loopForever(settings_);
        if (!endless && framePos_ >= totalFrames_) {
            return false;
        }

        const auto frames = static_cast<std::size_t>(
            endless ? kFramesPerRead
                    : std::min<std::int64_t>(kFramesPerRead, totalFrames_ - framePos_));

        out.clear();
        out.setFormat(format_);
        out.lossless        = false;
        out.streamTimestamp = static_cast<double>(framePos_) / kSampleRate;
        out.streamTimeRatio = 1.0;

        std::byte* dst = out.allocFrames(frames);
        std::memset(dst, 0, frames * kChannels * sizeof(float));
        out.setFrameCount(frames);

        framePos_ += static_cast<std::int64_t>(frames);
        return true;
    }

    std::int64_t seek(std::int64_t frame) override {
        framePos_ = std::clamp<std::int64_t>(frame, 0, totalFrames_);
        return framePos_;
    }

    void close() override { framePos_ = 0; }

private:
    static constexpr std::int64_t kFramesPerRead = 1024;

    const Settings* settings_ = nullptr;
    AudioFormat     format_{};
    std::int64_t    totalFrames_ = 0;
    std::int64_t    framePos_    = 0;
};

}  // namespace
}  // namespace xpcog

void xpcog_register_silence(xpcog::PluginRegistry& r) {
    r.addSource({
        .name     = "SilenceSource",
        .priority = xpcog::kDefaultPriority,
        .schemes  = xpcog::kSchemes,
        .create   = []() -> xpcog::SourcePtr {
            return std::make_unique<xpcog::SilenceSource>();
        },
        .available = nullptr,
    });

    // No extensions at all: a silence track is addressed by scheme, and the
    // decoder is found through the MIME type its source reports. That is the
    // only path in the registry where the MIME lookup is not a fallback.
    r.addDecoder({
        .name       = "SilenceDecoder",
        .priority   = xpcog::kDefaultPriority,
        .extensions = {},
        .mimeTypes  = xpcog::kMimeTypes,
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::SilenceDecoder>();
        },
        .available = nullptr,
    });
}
