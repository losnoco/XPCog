// Cue sheet container and decoder.
// Ports of Cog Plugins/CueSheet/CueSheetContainer.m and CueSheetDecoder.m.
//
// A .cue expands to one URL per track, addressed by fragment: album.cue#1,
// album.cue#2, ... Opening one of those decodes the referenced audio file,
// seeks to the track's INDEX 01, and stops at the next track's start.

#include "common/CueSheet.hpp"

#include "../common/PlaylistText.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"

#include <algorithm>
#include <memory>
#include <string_view>

namespace xpcog {
namespace {

std::vector<Url> expandCue(const Url& url, ISource& source,
                           const PluginRegistry& /*registry*/) {
    const codecs::CueSheet sheet =
        codecs::CueSheet::parse(codecs::readAllText(source), url);

    std::vector<Url> entries;
    entries.reserve(sheet.tracks().size());
    for (const codecs::CueTrack& track : sheet.tracks()) {
        entries.push_back(url.withFragment(track.track));
    }
    return entries;
}

/// The audio files a cue sheet needs alongside it. Cog reports these through
/// +dependencyUrlsForContainerURL: so the sandbox can be granted access to them.
std::vector<Url> cueDependencies(const Url& url, ISource& source,
                                 const PluginRegistry& /*registry*/) {
    const codecs::CueSheet sheet =
        codecs::CueSheet::parse(codecs::readAllText(source), url);

    std::vector<Url> files;
    for (const codecs::CueTrack& track : sheet.tracks()) {
        if (std::find(files.begin(), files.end(), track.url) == files.end()) {
            files.push_back(track.url);
        }
    }
    return files;
}

class CueSheetDecoder final : public IDecoder {
public:
    ~CueSheetDecoder() override { CueSheetDecoder::close(); }

    void setRegistry(const PluginRegistry* registry) override { registry_ = registry; }

    bool open(ISource* source) override {
        close();
        if (source == nullptr || registry_ == nullptr) {
            return false;
        }

        const Url& url = source->url();
        if (url.fragment().empty()) {
            // No fragment: nothing identifies which track to play.
            return false;
        }

        const codecs::CueSheet sheet =
            codecs::CueSheet::parse(codecs::readAllText(*source), url);

        const codecs::CueTrack* track = sheet.findTrack(url.fragment());
        if (track == nullptr) {
            return false;
        }

        // skipCue prevents recursing back into this decoder for the audio file.
        inner_ = registry_->open(track->url, SkipCue::Yes);
        if (!inner_) {
            return false;
        }

        const TrackProperties innerProps = inner_.decoder->properties();
        format_    = innerProps.format;
        lossless_  = innerProps.lossless;
        codecName_ = innerProps.codec;

        const double rate = format_.sampleRate;
        startFrame_       = track->startFrame(rate);

        const std::size_t         index = sheet.indexOf(track);
        const codecs::CueTrack*   next  = sheet.nextInSameFile(index);
        endFrame_ = (next != nullptr) ? next->startFrame(rate) : innerProps.totalFrames;

        if (endFrame_ <= startFrame_) {
            return false;
        }

        tags_       = track->metadata();
        replayGain_ = track->replayGain;

        // Seek eagerly so a failure surfaces at open() rather than mid-playback.
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

        // Trim the final chunk so the track stops exactly at the next INDEX,
        // rather than bleeding into the following track.
        const std::int64_t remaining = endFrame_ - framePos_;
        if (static_cast<std::int64_t>(out.frameCount()) > remaining) {
            out.setFrameCount(static_cast<std::size_t>(remaining));
        }

        // Timestamps are relative to the track, not the underlying file.
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
        // Callers seek within the track; the file needs an absolute position.
        const std::int64_t target =
            std::min(startFrame_ + frame, endFrame_);
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
        props.replayGain  = replayGain_;
        if (inner_.decoder) {
            props.bitrateKbps = inner_.decoder->properties().bitrateKbps;
        }
        return props;
    }

    [[nodiscard]] MetadataMap metadata() const override { return tags_; }

private:
    const PluginRegistry*        registry_ = nullptr;
    PluginRegistry::OpenResult   inner_;

    AudioFormat  format_{};
    std::int64_t startFrame_ = 0;
    std::int64_t endFrame_   = 0;
    std::int64_t framePos_   = 0;
    bool         lossless_   = false;
    std::string  codecName_;

    MetadataMap    tags_;
    ReplayGainInfo replayGain_;
};

constexpr std::string_view kExtensions[] = {"cue"};
constexpr std::string_view kMimeTypes[]  = {"application/x-cue"};

}  // namespace
}  // namespace xpcog

void xpcog_register_cuesheet(xpcog::PluginRegistry& r) {
    r.addContainer({
        .name         = "CueSheetContainer",
        .priority     = xpcog::kDefaultPriority,
        .extensions   = xpcog::kExtensions,
        .mimeTypes    = xpcog::kMimeTypes,
        .expand       = &xpcog::expandCue,
        .dependencies = &xpcog::cueDependencies,
    });

    r.addDecoder({
        // The name matters: SkipCue filters on it by string, exactly as Cog's
        // skipCue: filters out "CueSheetDecoder".
        .name       = "CueSheetDecoder",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = xpcog::kMimeTypes,
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::CueSheetDecoder>();
        },
        .available = nullptr,
    });
}
