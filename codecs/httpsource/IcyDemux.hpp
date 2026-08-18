// The ICY (SHOUTcast) protocol layer: everything Icecast-style servers add to a
// plain HTTP body, separated from the transport that fetches it.
//
// Port of the header and metadata handling in Cog Plugins/HTTPSource/HTTPSource.m
// (handle_icy_headers, http_content_header_handler_int, _handle_icy_metadata,
// http_parse_shoutcast_meta). Kept free of sockets, threads and buffers so the
// framing can be tested by feeding it bytes -- which is what it is: a state
// machine over a byte stream, and the part of an HTTP source most likely to be
// subtly wrong in a way that only shows up as a click every sixteen seconds.
//
// Two things ICY does that HTTP does not:
//
//   1. A SHOUTcast server may answer "ICY 200 OK" instead of an HTTP status
//      line. That is not HTTP, so an HTTP client sees a bodyless response or an
//      HTTP/0.9 one and the "headers" arrive as the first bytes of the body.
//      feedBody() detects and consumes them.
//   2. With icy-metaint set, the body is not audio -- it is `metaint` bytes of
//      audio, a length byte, that many sixteen-byte units of metadata, and
//      repeat. Failing to de-interleave that feeds the metadata to the decoder.

#pragma once

#include "xpcog/core/MetadataMap.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace xpcog::codecs {

class IcyDemux {
public:
    /// Where de-interleaved audio goes. Returns how many bytes it accepted; a
    /// short return means the consumer is shutting down and the demuxer stops.
    /// It may block -- the caller is a network thread and back-pressure is the
    /// point.
    using AudioSink = std::function<std::size_t(const std::byte*, std::size_t)>;

    /// What the response headers said. Fields not sent keep their defaults.
    struct Headers {
        std::string  contentType;
        std::int64_t contentLength = -1;  ///< -1 when absent or an ICY stream
        int          metaint       = 0;   ///< 0 when the stream carries no metadata
        /// Any icy-* header. Such a stream never ends on its own, so a clean
        /// close is a disconnection to reconnect from rather than an EOF.
        bool continuous = false;
        std::string name;   ///< icy-name
        std::string genre;  ///< icy-genre
        std::string url;    ///< icy-url
    };

    /// One "Key: value" line from the transport's own header parser. Case
    /// insensitive; a line without a colon is ignored.
    void feedHeaderLine(std::string_view line);

    /// Body bytes. Consumes in-body ICY headers first if the stream opened with
    /// them, then de-interleaves metadata, then passes audio to `sink`.
    ///
    /// Returns false when `sink` refused, meaning the transfer should stop. It is
    /// deliberately not a consumed-byte count: incomplete headers are buffered
    /// internally, so "consumed" and "made progress" come apart, and the only
    /// thing a transport can do with the answer is keep going or stop.
    bool feedBody(std::span<const std::byte> data, const AudioSink& sink);

    [[nodiscard]] const Headers& headers() const noexcept { return headers_; }

    /// True once the response headers are complete -- immediately for a real HTTP
    /// response, or after the in-body ICY block for a SHOUTcast one. open() waits
    /// on this so the MIME type is known before a decoder is chosen.
    [[nodiscard]] bool headersComplete() const noexcept { return headersDone_; }

    /// Tags from icy-* headers and the most recent StreamTitle, or an empty map
    /// when nothing has changed since the last call. Cog splits this into
    /// -hasMetadata (which clears the flag) and -metadata; one call that answers
    /// "what is new" cannot be used in the order that returns stale tags.
    [[nodiscard]] MetadataMap takeUpdatedMetadata();

    /// Forgets the ICY framing and header state but keeps the parsed headers and
    /// tags. Called before a reconnect: the new response repeats its headers and
    /// restarts the metaint cycle, but the stream is the same stream.
    void resetTransport();

    /// Parses a `StreamTitle='...';StreamUrl='...';` block. Exposed for tests.
    /// Returns false when the block contains no StreamTitle.
    [[nodiscard]] static bool parseStreamTitle(std::string_view block,
                                               std::string&     artist,
                                               std::string&     title);

private:
    enum class Phase : std::uint8_t { Audio, MetaLength, Meta };

    /// Works on headerTail_. True once the header block is complete, leaving
    /// whatever followed it in headerTail_ as the first body bytes.
    bool consumeInBodyHeaders();
    /// De-interleaves and forwards `data`, which must be past all headers.
    bool feedStream(std::span<const std::byte> data, const AudioSink& sink);
    void applyHeader(std::string_view key, std::string_view value);
    void beginResponse();

    Headers headers_;
    bool    headersDone_    = false;
    bool    sawIcyStatus_   = false;  ///< the response began "ICY 200 OK"
    bool    inBodyHeaders_  = false;  ///< still consuming the in-body header block
    /// In-body header text not yet parsed, and after the block is complete, the
    /// body bytes that followed it in the same packet.
    std::string headerTail_;

    Phase       phase_     = Phase::Audio;
    std::int64_t audioLeft_ = 0;  ///< audio bytes before the next metadata block
    std::size_t  metaLeft_  = 0;
    std::string  metaBlock_;

    std::string artist_;
    std::string title_;
    bool        metadataChanged_ = false;
};

}  // namespace xpcog::codecs
