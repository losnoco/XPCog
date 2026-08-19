// HTTP Live Streaming. Port of Cog Plugins/HLS/HLSDecoder.m.
//
// HLS is not an audio format -- it is a manifest naming a sequence of ordinary
// media files. So this decoder does no decoding: it parses the manifest, picks a
// rendition, keeps a fetcher a few segments ahead, and hands the concatenated
// bytes to whichever decoder claims the segment format.
//
// Two departures from Cog, both consequences of the registry:
//
//   * Cog reaches for FFMPEGDecoder by name (NSClassFromString) because its
//     extension lookup routes nothing to MPEG-TS. Here the memory source is
//     given the identity of what was actually fetched -- a filename and a MIME
//     type -- and the registry chooses, so an MP3 rendition reaches minimp3 and a
//     future dedicated decoder is picked up with no change here.
//   * Cog refuses anything that is not http(s). The segments are fetched through
//     the registry like any other URL, so the scheme is not this decoder's
//     business; a file:// manifest works, which is also what makes an offline
//     test of the whole path possible.

#include "HlsDecoder.hpp"

#include "../common/Id3v2.hpp"
#include "../common/PlaylistText.hpp"

#include "xpcog/core/PluginRegistry.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xpcog {
namespace {

using codecs::HlsPlaylist;
using codecs::HlsSegment;
using codecs::HlsVariant;

/// What the first segment turned out to be. `filename` is what the memory source
/// is named so the registry's extension lookup finds the right decoder;
/// `mimeType` is the fallback for the same lookup when it does not.
struct SegmentFormat {
    std::string_view filename = "stream";
    std::string_view mimeType;
};

/// Skips any ID3v2 tags, which an HLS "packed audio" segment carries in front of
/// the audio to convey its timestamp -- so the bytes that identify the format
/// are not at offset zero for exactly the renditions most likely to be raw AAC.
[[nodiscard]] std::span<const std::byte> skipId3(std::span<const std::byte> data) {
    while (const std::size_t length = codecs::id3v2TagLength(data)) {
        if (length >= data.size()) {
            return {};
        }
        data = data.subspan(length);
    }
    return data;
}

[[nodiscard]] bool startsWith(std::span<const std::byte> data, std::size_t offset,
                              std::string_view tag) {
    if (data.size() < offset + tag.size()) {
        return false;
    }
    return std::memcmp(data.data() + offset, tag.data(), tag.size()) == 0;
}

/// Identifies a segment from its own bytes.
///
/// Sniffing rather than trusting Content-Type, which Cog uses: an origin serving
/// HLS very often labels every segment `application/octet-stream`, and Cog's
/// fallback for that is `audio/mpeg` -- which names the one thing an MPEG-TS
/// segment is not. The bytes are unambiguous and already in hand.
[[nodiscard]] SegmentFormat sniffSegment(std::span<const std::byte> raw) {
    // MPEG-TS: 188-byte packets, each beginning 0x47. Two in a row rules out a
    // coincidence, and the sync byte is checked before the ID3 skip because a
    // transport stream can legitimately contain those three bytes early on.
    if (raw.size() > 188 && std::to_integer<int>(raw[0]) == 0x47 &&
        std::to_integer<int>(raw[188]) == 0x47) {
        return {"stream.ts", "video/mp2t"};
    }

    const std::span<const std::byte> data = skipId3(raw);
    if (data.size() < 4) {
        return {};
    }

    // ISO base media: the second box word names the type. `styp` is what a
    // fragmented-MP4 HLS segment leads with; `ftyp` is the initialisation
    // section an EXT-X-MAP points at.
    if (startsWith(data, 4, "ftyp") || startsWith(data, 4, "styp") ||
        startsWith(data, 4, "moof") || startsWith(data, 4, "sidx")) {
        return {"stream.m4a", "audio/mp4"};
    }

    if (startsWith(data, 0, "\x0B\x77")) {
        return {"stream.ac3", "audio/ac3"};
    }

    const int first  = std::to_integer<int>(data[0]);
    const int second = std::to_integer<int>(data[1]);
    if (first == 0xFF && (second & 0xE0) == 0xE0) {
        // Both ADTS and an MPEG audio frame start with the same sync bits. The
        // layer field separates them: ADTS always reports layer 00, which no
        // MPEG-1/2 audio frame ever does.
        return ((second & 0x06) == 0) ? SegmentFormat{"stream.aac", "audio/aac"}
                                      : SegmentFormat{"stream.mp3", "audio/mpeg"};
    }

    return {};
}

/// Cog's -fakeFilenameForMimeType:, used only when the bytes said nothing.
[[nodiscard]] SegmentFormat formatFromMimeType(std::string_view mime) {
    std::string lowered{mime};
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // Parameters are not part of the type: "audio/aac; charset=binary" is aac.
    if (const std::size_t semicolon = lowered.find(';'); semicolon != std::string::npos) {
        lowered.erase(semicolon);
    }

    if (lowered.starts_with("audio/aac") || lowered.starts_with("audio/aacp")) {
        return {"stream.aac", "audio/aac"};
    }
    if (lowered.starts_with("audio/mpeg") || lowered.starts_with("audio/mp3")) {
        return {"stream.mp3", "audio/mpeg"};
    }
    if (lowered.starts_with("audio/mp4") || lowered.starts_with("audio/m4a") ||
        lowered.starts_with("video/mp4")) {
        return {"stream.m4a", "audio/mp4"};
    }
    if (lowered.starts_with("video/mp2t") || lowered.starts_with("audio/mp2t")) {
        return {"stream.ts", "video/mp2t"};
    }
    // Nothing recognised. An extensionless name leaves the registry to match on
    // MIME type, and crucially does not leave the manifest's own .m3u8 in place,
    // which would select this decoder again.
    return {};
}

constexpr std::string_view kExtensions[] = {"m3u8", "m3u"};
constexpr std::string_view kMimeTypes[]  = {"application/vnd.apple.mpegurl",
                                            "application/x-mpegurl", "audio/mpegurl",
                                            "audio/x-mpegurl"};

}  // namespace

HlsDecoder::~HlsDecoder() { HlsDecoder::close(); }

bool HlsDecoder::open(ISource* source) {
    close();
    if (source == nullptr || registry_ == nullptr) {
        return false;
    }

    // Fragment stripped: on this URL it would be read as a subsong index by
    // whichever decoder ends up opening the segments.
    const Url manifestUrl = source->url().withoutFragment();

    std::optional<HlsPlaylist> playlist =
        codecs::parseHlsPlaylist(codecs::readAllText(*source), manifestUrl);
    if (!playlist) {
        return false;
    }

    if (playlist->isMaster) {
        const HlsVariant* best = playlist->bestVariant();
        if (best == nullptr) {
            return false;
        }
        playlist = fetchPlaylist(best->url);
        // A master naming another master is malformed; not followed, so a
        // mutually-referencing pair cannot loop.
        if (!playlist || playlist->isMaster) {
            return false;
        }
    }

    if (playlist->segments.empty()) {
        return false;
    }
    // Cog refuses these too. Parsing EXT-X-KEY rather than ignoring it is what
    // makes this a specific refusal instead of a corrupt-stream failure inside
    // whichever decoder was handed ciphertext.
    if (playlist->segments.front().encrypted) {
        return false;
    }

    live_ = playlist->isLive;

    memory_ = std::make_unique<codecs::HlsMemorySource>(manifestUrl,
                                                        "application/octet-stream");
    manager_ = std::make_unique<codecs::HlsSegmentManager>(std::move(*playlist),
                                                           *registry_, *memory_);

    // The first segment is fetched synchronously: the inner decoder cannot probe
    // an empty stream, and until its bytes are in hand there is nothing to say
    // what that decoder should be.
    std::optional<std::vector<std::byte>> first = manager_->download(0);
    if (!first) {
        close();
        return false;
    }

    SegmentFormat format = sniffSegment(*first);
    if (format.mimeType.empty()) {
        format = formatFromMimeType(manager_->lastMimeType());
    }
    if (const auto identity = codecs::resolveHlsUri(format.filename, manifestUrl)) {
        memory_->setIdentity(*identity, std::string{format.mimeType});
    } else {
        memory_->setIdentity(manifestUrl, std::string{format.mimeType});
    }

    memory_->append(std::move(*first));
    manager_->start(1);

    if (!openInner()) {
        // The fetcher is running by now, so a failure here has a thread to stop.
        close();
        return false;
    }

    // Only a finite playlist has a length. A live one reports zero frames, which
    // is how TrackProperties says "unknown".
    if (!live_) {
        const double rate = inner_->properties().format.sampleRate;
        if (const double duration = manager_->totalDuration(); duration > 0.0 && rate > 0.0) {
            totalFrames_ = static_cast<std::int64_t>(duration * rate);
        }
    }

    return true;
}

std::optional<HlsPlaylist> HlsDecoder::fetchPlaylist(const Url& url) const {
    SourcePtr source = registry_->makeSource(url);
    if (!source || !source->open(url)) {
        return std::nullopt;
    }
    const std::string text = codecs::readAllText(*source);
    source->close();
    return codecs::parseHlsPlaylist(text, url);
}

bool HlsDecoder::openInner() {
    inner_ = registry_->makeDecoder(*memory_);
    if (!inner_ || !inner_->open(memory_.get())) {
        inner_.reset();
        return false;
    }
    // Chained-stream tag and format changes come from the decoder underneath;
    // this replaces the KVO forwarding Cog installs on the same two keys.
    inner_->setChangeCallback([this](bool properties, bool metadata) {
        notifyChanged(properties, metadata);
    });
    return true;
}

TrackProperties HlsDecoder::properties() const {
    TrackProperties properties =
        inner_ ? inner_->properties() : TrackProperties{};
    properties.totalFrames = live_ ? 0 : totalFrames_;
    // Seeking a live stream is meaningless: the window it would seek within is
    // not what is playing, and the segments before it are already gone.
    properties.seekable = !live_ && totalFrames_ > 0;
    return properties;
}

MetadataMap HlsDecoder::metadata() const {
    return inner_ ? inner_->metadata() : MetadataMap{};
}

bool HlsDecoder::readAudio(AudioChunk& out) {
    if (!inner_) {
        return false;
    }

    while (inner_->readAudio(out)) {
        if (pendingSkipFrames_ <= 0) {
            return true;
        }

        // A seek lands on a segment boundary; the frames between there and where
        // the user asked for are discarded here rather than inside the decoder,
        // which was reopened and knows nothing about the seek.
        const auto frames = static_cast<std::int64_t>(out.frameCount());
        if (frames <= pendingSkipFrames_) {
            pendingSkipFrames_ -= frames;
            continue;
        }

        AudioChunk discarded;
        out.removeFrames(static_cast<std::size_t>(pendingSkipFrames_), discarded);
        pendingSkipFrames_ = 0;
        return true;
    }

    return false;
}

std::int64_t HlsDecoder::seek(std::int64_t frame) {
    if (live_ || !inner_ || !manager_ || !memory_) {
        return -1;
    }
    frame = std::max<std::int64_t>(frame, 0);

    const double rate = inner_->properties().format.sampleRate;
    if (rate <= 0.0) {
        return -1;
    }

    const std::vector<HlsSegment> segments   = manager_->segments();
    const double                  targetTime = static_cast<double>(frame) / rate;

    // The segment holding the target, and where that segment starts. Durations
    // are what the manifest declares, so this is approximate by construction --
    // the remainder is trimmed from the decoded audio below.
    std::size_t index            = 0;
    double      elapsed          = 0.0;
    double      segmentStartTime = 0.0;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        index            = i;
        segmentStartTime = elapsed;
        if (targetTime < elapsed + segments[i].duration) {
            break;
        }
        elapsed += segments[i].duration;
    }

    // Fetched before anything is torn down: a failure here leaves playback
    // running rather than dropping the user into a half-closed decoder.
    std::optional<std::vector<std::byte>> data = manager_->download(index);
    if (!data) {
        return -1;
    }

    manager_->stop();
    memory_->reset();
    inner_->close();
    inner_.reset();

    memory_->append(std::move(*data));
    manager_->start(index + 1);

    if (!openInner()) {
        close();
        return -1;
    }

    const auto segmentStartFrame = static_cast<std::int64_t>(segmentStartTime * rate);
    pendingSkipFrames_           = std::max<std::int64_t>(frame - segmentStartFrame, 0);
    return frame;
}

void HlsDecoder::close() {
    // Fetcher first: it is the only other thread, and it writes into the memory
    // source that the decoder below is reading from.
    if (manager_) {
        manager_->stop();
    }
    if (inner_) {
        inner_->close();
        inner_.reset();
    }
    if (memory_) {
        memory_->close();
        memory_.reset();
    }
    manager_.reset();

    live_              = false;
    totalFrames_       = 0;
    pendingSkipFrames_ = 0;
}

void HlsDecoder::interrupt() {
    // Unblocks without tearing down: close() still runs on the reader's thread.
    if (manager_) {
        manager_->interrupt();
    }
    if (memory_) {
        memory_->interrupt();
    }
    if (inner_) {
        inner_->interrupt();
    }
}

}  // namespace xpcog

void xpcog_register_hls(xpcog::PluginRegistry& r) {
    r.addDecoder({
        // Well above every other decoder, so an .m3u8 reaches this before
        // anything that claims the extension for ordinary playlists. Cog uses 16
        // for the same reason.
        .name       = "HlsDecoder",
        .priority   = 16.0F,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = xpcog::kMimeTypes,
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::HlsDecoder>();
        },
        .available = nullptr,
    });
}
