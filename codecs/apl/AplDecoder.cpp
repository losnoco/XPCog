// Monkey's Audio Link files. Port of Cog Plugins/APL/, both APLFile.m and
// APLDecoder.m.
//
// An `.apl` is not audio. It is a few lines of text naming one big `.ape` -- a
// whole CD ripped to a single file -- and a range of samples within it, so a
// set of them turns that one file into an album of tracks. Monkey's Audio's own
// ripper writes them, which is why they travel in company with `.ape` and why
// a library that ignores them shows a fifty-minute track where twelve should be.
//
// So this is the same shape as a cue sheet track and shares its whole
// implementation idea (see ../cuesheet/CueSheetPlugin.cpp): open the file the
// link names through the registry, seek to the start, and stop at the end. What
// differs is only how the range is discovered -- one file per link here, rather
// than one sheet listing many.
//
// Nothing about it is specific to Monkey's Audio. The image is opened through
// the registry like any other file, so an `.apl` pointing at something else
// works if that something else decodes and seeks.

#include "common/PlaylistText.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace xpcog {
namespace {

constexpr std::string_view kHeader = "[monkey's audio image link file]";

[[nodiscard]] std::string lowerAscii(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

[[nodiscard]] std::string_view trim(std::string_view text) {
    const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!text.empty() && space(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && space(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

/// What one `.apl` says: which file, and which samples of it.
struct AplLink {
    Url          image;
    std::int64_t startBlock = 0;
    std::int64_t endBlock   = 0;  ///< 0 means "to the end of the image"
    bool         valid      = false;
};

/// The path an `Image File` line names, resolved against the link's own
/// location. Cog accepts an absolute path or a URL as well, and so does this.
[[nodiscard]] Url resolveImage(std::string_view value, const Url& linkUrl) {
    if (value.find("://") != std::string_view::npos) {
        if (auto parsed = Url::parse(value)) {
            return *parsed;
        }
    }

    // Backslashes only ever appear in a relative path here -- the files are
    // written on Windows -- and every platform this runs on takes a forward
    // slash, so they are normalised rather than special-cased per platform.
    std::string path{value};
    std::replace(path.begin(), path.end(), '\\', '/');

    const std::filesystem::path candidate{path};
    if (candidate.is_absolute()) {
        return Url::fromLocalPath(candidate);
    }

    const auto base = linkUrl.localPath();
    if (!base) {
        return {};
    }
    return Url::fromLocalPath(base->parent_path() / candidate);
}

/// Reads the header and the `field=value` lines under it.
///
/// The block stops at the first line beginning with '-', which is how the file
/// separates its fields from the APE tag copy underneath -- Cog matches the
/// exact banner *and* falls back to any leading '-', and the second test is the
/// one that matters, since the banner has been spelled more than one way.
[[nodiscard]] AplLink parseApl(std::string_view text, const Url& linkUrl) {
    AplLink link;

    std::size_t at = 0;
    const auto nextLine = [&]() -> std::string_view {
        if (at >= text.size()) {
            return {};
        }
        const std::size_t end  = text.find('\n', at);
        const auto        line = text.substr(at, end == std::string_view::npos
                                                     ? std::string_view::npos
                                                     : end - at);
        at = (end == std::string_view::npos) ? text.size() : end + 1;
        return line;
    };

    if (lowerAscii(trim(nextLine())) != kHeader) {
        return link;
    }

    bool haveImage = false;
    while (at < text.size()) {
        const std::string_view line = trim(nextLine());
        if (line.empty()) {
            continue;
        }
        if (line.front() == '-') {
            break;
        }

        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const std::string      field = lowerAscii(trim(line.substr(0, equals)));
        const std::string_view value = trim(line.substr(equals + 1));

        if (field == "image file") {
            link.image = resolveImage(value, linkUrl);
            haveImage  = !link.image.toString().empty();
        } else if (field == "start block") {
            // 64-bit, where Cog parses these with -[NSString intValue] and
            // carries a comment saying it "bugs with files over 2GB". A block is
            // a sample, so 32 bits runs out around thirteen hours of CD audio --
            // which no single image reaches, but the same field is written for
            // high-rate images where it does.
            link.startBlock = std::strtoll(std::string{value}.c_str(), nullptr, 10);
        } else if (field == "finish block") {
            link.endBlock = std::strtoll(std::string{value}.c_str(), nullptr, 10);
        }
    }

    link.valid = haveImage;
    return link;
}

class AplDecoder final : public IDecoder {
public:
    ~AplDecoder() override { AplDecoder::close(); }

    void setRegistry(const PluginRegistry* registry) override { registry_ = registry; }

    bool open(ISource* source) override {
        close();
        if (source == nullptr || registry_ == nullptr) {
            return false;
        }

        const AplLink link = parseApl(codecs::readAllText(*source), source->url());
        if (!link.valid) {
            return false;
        }

        // SkipCue so an image that also has a cue sheet beside it is opened as
        // audio rather than as a sheet, which is the same reason the cue decoder
        // passes it when opening its own image.
        inner_ = registry_->open(link.image, SkipCue::Yes);
        if (!inner_) {
            return false;
        }

        const TrackProperties innerProps = inner_.decoder->properties();
        format_    = innerProps.format;
        lossless_  = innerProps.lossless;
        codecName_ = innerProps.codec;

        startFrame_ = std::max<std::int64_t>(0, link.startBlock);
        // A finish block at or before the start means the link covers the rest
        // of the image, which is what Cog does with the same test.
        endFrame_ = (link.endBlock > link.startBlock) ? link.endBlock
                                                      : innerProps.totalFrames;
        endFrame_ = std::min(endFrame_, innerProps.totalFrames);
        if (endFrame_ <= startFrame_) {
            return false;
        }

        // Eagerly, so a link naming a range the image does not have fails at
        // open() rather than at the moment it is played.
        if (inner_.decoder->seek(startFrame_) < 0) {
            return false;
        }
        framePos_ = startFrame_;
        return format_.valid();
    }

    bool readAudio(AudioChunk& out) override {
        if (!inner_ || framePos_ >= endFrame_) {
            return false;
        }
        if (!inner_.decoder->readAudio(out)) {
            return false;
        }

        const std::int64_t remaining = endFrame_ - framePos_;
        if (static_cast<std::int64_t>(out.frameCount()) > remaining) {
            out.setFrameCount(static_cast<std::size_t>(remaining));
        }

        out.streamTimestamp = (format_.sampleRate > 0.0)
                                  ? static_cast<double>(framePos_ - startFrame_) /
                                        format_.sampleRate
                                  : 0.0;

        framePos_ += static_cast<std::int64_t>(out.frameCount());
        return out.frameCount() > 0;
    }

    std::int64_t seek(std::int64_t frame) override {
        if (!inner_) {
            return -1;
        }
        const std::int64_t target = std::min(startFrame_ + frame, endFrame_);
        if (inner_.decoder->seek(target) < 0) {
            return -1;
        }
        framePos_ = target;
        return frame;
    }

    void close() override {
        // The inner decoder borrows the inner source, so order matters.
        inner_.decoder.reset();
        inner_.source.reset();
    }

    void interrupt() override {
        if (inner_.decoder) {
            inner_.decoder->interrupt();
        }
    }

    [[nodiscard]] TrackProperties properties() const override {
        TrackProperties props;
        props.format      = format_;
        props.totalFrames = endFrame_ - startFrame_;
        props.seekable    = true;
        props.lossless    = lossless_;
        props.codec       = codecName_;
        props.encoding    = lossless_ ? "lossless" : "lossy";
        if (inner_.decoder) {
            const TrackProperties innerProps = inner_.decoder->properties();
            props.bitrateKbps = innerProps.bitrateKbps;
            props.replayGain  = innerProps.replayGain;
        }
        return props;
    }

private:
    const PluginRegistry*      registry_ = nullptr;
    PluginRegistry::OpenResult inner_;

    AudioFormat  format_{};
    std::string  codecName_;
    std::int64_t startFrame_ = 0;
    std::int64_t endFrame_   = 0;
    std::int64_t framePos_   = 0;
    bool         lossless_   = true;
};

constexpr std::string_view kExtensions[] = {"apl"};
constexpr std::string_view kMimeTypes[]  = {"application/x-apl"};

}  // namespace
}  // namespace xpcog

void xpcog_register_apl(xpcog::PluginRegistry& r) {
    r.addDecoder({
        .name       = "AplDecoder",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = xpcog::kMimeTypes,
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::AplDecoder>();
        },
        .available = nullptr,
    });
}
