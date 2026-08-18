#include "IcyDemux.hpp"

#include "common/TextEncoding.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace xpcog::codecs {
namespace {

/// Cog gives up on unterminated ICY headers after ten packets
/// (HTTPSource.m:540). Packet size is a property of the network rather than of
/// the stream, so the same guard is expressed in bytes here: a server that has
/// not finished its header block within this much is not going to.
constexpr std::size_t kMaxHeaderBytes = 8192;

/// SHOUTcast metadata is a length byte counting sixteen-byte units, so a block
/// is at most 255 * 16 = 4080. Cog carries a 4096-byte buffer and a "metadata
/// size is too large" branch guarding against overrunning it, which therefore
/// cannot be reached. Nothing here needs the cap; the arithmetic is recorded
/// rather than the branch reproduced.
constexpr std::size_t kMetaUnit = 16;

[[nodiscard]] bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

[[nodiscard]] std::string_view trim(std::string_view text) {
    const auto space = [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    };
    while (!text.empty() && space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] bool startsWithIcyPrefix(std::string_view key) {
    return key.size() >= 4 && iequals(key.substr(0, 4), "icy-");
}

}  // namespace

void IcyDemux::beginResponse() {
    // Per-response only. A redirect's 3xx headers must not survive into the
    // response that actually carries the audio, but icy-* facts and the tags
    // already shown to the user belong to the stream, not to one request.
    headers_.contentType   = {};
    headers_.contentLength = -1;
    headers_.metaint       = 0;
    headersDone_           = false;
    phase_                 = Phase::Audio;
    audioLeft_             = 0;
    metaLeft_              = 0;
    metaBlock_.clear();
}

void IcyDemux::feedHeaderLine(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.remove_suffix(1);
    }

    if (line.starts_with("HTTP/") || line.starts_with("ICY ")) {
        // A status line. With redirects followed by the transport this runs once
        // per hop, and only the last hop's headers describe the audio.
        beginResponse();
        return;
    }

    if (line.empty()) {
        headersDone_ = true;
        return;
    }

    const auto colon = line.find(':');
    if (colon == std::string_view::npos) {
        return;
    }
    applyHeader(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
}

void IcyDemux::applyHeader(std::string_view key, std::string_view value) {
    if (iequals(key, "Content-Type")) {
        // Servers send "audio/mpeg; charset=UTF-8"; the registry matches decoders
        // on the bare type, lowercased.
        std::string type = toUtf8(std::string{value});
        if (const auto semi = type.find(';'); semi != std::string::npos) {
            type.erase(semi);
        }
        type = std::string{trim(type)};
        std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        headers_.contentType = std::move(type);
        return;
    }

    if (iequals(key, "Content-Length")) {
        try {
            headers_.contentLength = std::stoll(std::string{value});
        } catch (const std::exception&) {
            headers_.contentLength = -1;
        }
        return;
    }

    if (!startsWithIcyPrefix(key)) {
        return;
    }

    // Any icy-* header means a stream rather than a file: it has no length, and
    // it does not end -- a clean close is a disconnection to reconnect from.
    headers_.continuous    = true;
    headers_.contentLength = -1;

    if (iequals(key, "icy-metaint")) {
        try {
            headers_.metaint = std::max(0, std::stoi(std::string{value}));
        } catch (const std::exception&) {
            headers_.metaint = 0;
        }
        audioLeft_ = headers_.metaint;
    } else if (iequals(key, "icy-name")) {
        headers_.name    = toUtf8(std::string{value});
        title_           = headers_.name;
        metadataChanged_ = true;
    } else if (iequals(key, "icy-genre")) {
        headers_.genre   = toUtf8(std::string{value});
        metadataChanged_ = true;
    } else if (iequals(key, "icy-url")) {
        headers_.url     = toUtf8(std::string{value});
        metadataChanged_ = true;
    }
}

bool IcyDemux::consumeInBodyHeaders() {
    if (!sawIcyStatus_ && !inBodyHeaders_) {
        static constexpr std::string_view kIcy = "ICY 200 OK";
        const std::size_t decidable = std::min(headerTail_.size(), kIcy.size());
        if (headerTail_.compare(0, decidable, kIcy.data(), decidable) != 0) {
            // Not a SHOUTcast status line, so the body is audio from byte zero.
            headersDone_ = true;
            return true;
        }
        if (headerTail_.size() < kIcy.size()) {
            return false;  // still undecided; wait for more bytes
        }

        headerTail_.erase(0, kIcy.size());
        sawIcyStatus_          = true;
        inBodyHeaders_         = true;
        headers_.continuous    = true;
        headers_.contentLength = -1;

        // The status line's own terminator, so the loop below sees header lines.
        if (headerTail_.starts_with("\r\n")) {
            headerTail_.erase(0, 2);
        } else if (headerTail_.starts_with("\n")) {
            headerTail_.erase(0, 1);
        }
    }

    for (;;) {
        const auto eol = headerTail_.find('\n');
        if (eol == std::string::npos) {
            if (headerTail_.size() > kMaxHeaderBytes) {
                // Discard rather than fall through to the audio path: what is in
                // the buffer is header text. Cog stops parsing but never rewinds,
                // so it hands the decoder whatever followed.
                headerTail_.clear();
                headers_.metaint = 0;
                headersDone_     = true;
                return true;
            }
            return false;
        }

        std::string_view line{headerTail_.data(), eol};
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        const bool blank = line.empty();
        if (!blank) {
            if (const auto colon = line.find(':'); colon != std::string_view::npos) {
                applyHeader(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
            }
        }
        headerTail_.erase(0, eol + 1);

        if (blank) {
            headersDone_ = true;
            return true;
        }
    }
}

bool IcyDemux::feedBody(std::span<const std::byte> data, const AudioSink& sink) {
    if (!headersDone_) {
        headerTail_.append(reinterpret_cast<const char*>(data.data()), data.size());
        if (!consumeInBodyHeaders()) {
            return true;  // buffered; nothing to hand on yet
        }

        // Whatever followed the header block in the same packet is body.
        const std::string leftover = std::exchange(headerTail_, {});
        return feedStream(
            {reinterpret_cast<const std::byte*>(leftover.data()), leftover.size()},
            sink);
    }

    return feedStream(data, sink);
}

bool IcyDemux::feedStream(std::span<const std::byte> data, const AudioSink& sink) {
    std::size_t offset = 0;

    while (offset < data.size()) {
        const std::byte*  p     = data.data() + offset;
        const std::size_t avail = data.size() - offset;

        switch (phase_) {
        case Phase::Audio: {
            std::size_t want = avail;
            if (headers_.metaint > 0) {
                want = std::min<std::size_t>(want,
                                             static_cast<std::size_t>(audioLeft_));
            }
            if (want > 0) {
                const std::size_t took = sink(p, want);
                offset += took;
                if (headers_.metaint > 0) {
                    audioLeft_ -= static_cast<std::int64_t>(took);
                }
                if (took < want) {
                    return false;
                }
            }
            if (headers_.metaint > 0 && audioLeft_ == 0) {
                phase_ = Phase::MetaLength;
            }
            break;
        }

        case Phase::MetaLength: {
            metaLeft_ =
                static_cast<std::size_t>(std::to_integer<unsigned char>(*p)) * kMetaUnit;
            offset += 1;
            metaBlock_.clear();
            if (metaLeft_ == 0) {
                // The common case: nothing has changed, so the server sends a
                // zero length rather than repeating the title.
                phase_     = Phase::Audio;
                audioLeft_ = headers_.metaint;
            } else {
                phase_ = Phase::Meta;
            }
            break;
        }

        case Phase::Meta: {
            const std::size_t take = std::min(avail, metaLeft_);
            metaBlock_.append(reinterpret_cast<const char*>(p), take);
            offset += take;
            metaLeft_ -= take;
            if (metaLeft_ == 0) {
                std::string artist;
                std::string title;
                // Unlike Cog, a block without a StreamTitle does not switch the
                // framing off. Cog sets icy_metaint = 0 when its parser returns
                // -1 (HTTPSource.m:583), and from then on every metadata block
                // reaches the decoder as audio -- a click every metaint bytes for
                // the rest of the stream, caused by a server that sent StreamUrl
                // on its own.
                if (parseStreamTitle(metaBlock_, artist, title) &&
                    (artist != artist_ || title != title_)) {
                    artist_          = artist;
                    title_           = title;
                    metadataChanged_ = true;
                }
                metaBlock_.clear();
                phase_     = Phase::Audio;
                audioLeft_ = headers_.metaint;
            }
            break;
        }
        }
    }

    return true;
}

bool IcyDemux::parseStreamTitle(std::string_view block,
                                std::string&     artist,
                                std::string&     title) {
    static constexpr std::string_view kKey = "StreamTitle='";

    const auto start = block.find(kKey);
    if (start == std::string_view::npos) {
        return false;
    }

    const auto valueStart = start + kKey.size();
    const auto end        = block.find("';", valueStart);
    if (end == std::string_view::npos) {
        return false;
    }

    std::string value =
        toUtf8(std::string{block.substr(valueStart, end - valueStart)});

    // Cog splits on " - " and treats the left half as the artist. Anything
    // without the separator is a title with no artist.
    const auto dash = value.find(" - ");
    if (dash == std::string::npos) {
        artist.clear();
        title = std::move(value);
        return true;
    }

    artist = value.substr(0, dash);
    title  = value.substr(dash + 3);

    // Ported as-is from Cog, where it is commented "Hack for a certain stream":
    // that station wraps the title half in text="...".
    if (title.starts_with("text=\"")) {
        const std::string inner = title.substr(6);
        if (const auto quote = inner.find('"'); quote != std::string::npos) {
            title = inner.substr(0, quote);
        }
    }
    return true;
}

MetadataMap IcyDemux::takeUpdatedMetadata() {
    MetadataMap tags;
    if (!metadataChanged_) {
        return tags;
    }
    metadataChanged_ = false;

    if (!artist_.empty()) {
        tags.set("artist", artist_);
    }
    if (!title_.empty()) {
        tags.set("title", title_);
    }
    if (!headers_.genre.empty()) {
        tags.set("genre", headers_.genre);
    }
    // Cog puts icy-url -- the station's home page -- in the album field
    // (HTTPSource.m:490). Odd, and kept: it is where a Cog user has been reading
    // it, and there is no better-fitting key in the shared vocabulary.
    if (!headers_.url.empty()) {
        tags.set("album", headers_.url);
    }
    return tags;
}

void IcyDemux::resetTransport() {
    // Keeps headers_ and the tags: a reconnect re-announces its headers and
    // restarts the metaint cycle, but it is the same stream and the same title.
    headersDone_   = false;
    sawIcyStatus_  = false;
    inBodyHeaders_ = false;
    headerTail_.clear();
    headers_.metaint = 0;
    phase_           = Phase::Audio;
    audioLeft_       = 0;
    metaLeft_        = 0;
    metaBlock_.clear();
}

}  // namespace xpcog::codecs
