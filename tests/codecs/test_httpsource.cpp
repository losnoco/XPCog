// The two halves of the HTTP source that do not need a socket: the ring the
// network thread fills and the decoder drains, and the SHOUTcast framing that
// sits between them.
//
// Both are where the bugs live. A ring is arithmetic that is wrong only at the
// wrap, and ICY framing is wrong only every metaint bytes -- neither shows up as
// a failure to play, only as a click or a seek that lands in the wrong place, so
// neither is caught by trying it.

#include "httpsource/IcyDemux.hpp"
#include "httpsource/StreamBuffer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using xpcog::codecs::IcyDemux;
using xpcog::codecs::StreamBuffer;

namespace {

std::vector<std::byte> bytesOf(std::string_view text) {
    std::vector<std::byte> out(text.size());
    std::memcpy(out.data(), text.data(), text.size());
    return out;
}

/// A run of distinct bytes, so a misplaced copy shows up as a wrong value rather
/// than as the right value in the wrong place.
std::vector<std::byte> ramp(std::size_t count, unsigned char first = 0) {
    std::vector<std::byte> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = static_cast<std::byte>((first + i) & 0xFF);
    }
    return out;
}

/// Collects everything the demuxer calls audio.
struct Collector {
    std::string        audio;
    std::size_t        refuseAfter = static_cast<std::size_t>(-1);

    IcyDemux::AudioSink sink() {
        return [this](const std::byte* p, std::size_t n) -> std::size_t {
            const std::size_t room =
                (refuseAfter > audio.size()) ? refuseAfter - audio.size() : 0;
            const std::size_t take = std::min(n, room);
            audio.append(reinterpret_cast<const char*>(p), take);
            return take;
        };
    }
};

/// A SHOUTcast metadata block: the length byte followed by `text` padded out to
/// a whole number of sixteen-byte units, which is the only shape the protocol
/// can express.
std::string metaBlock(std::string_view text) {
    const std::size_t units = (text.size() + 15) / 16;
    std::string       out(1, static_cast<char>(units));
    out += text;
    out.resize(1 + units * 16, ' ');
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// StreamBuffer
// ---------------------------------------------------------------------------

TEST_CASE("the ring hands back exactly what was written, across the wrap") {
    StreamBuffer buffer{64};

    // Half the ring is reserved for history, so a write can never exceed 32.
    REQUIRE(buffer.writable() == 32);

    std::string       seen;
    unsigned char     next = 0;
    std::vector<char> out(16);

    // Six rounds of 24 bytes wraps the 64-byte ring twice at a stride that is
    // not a divisor of it, so every copy is split.
    for (int round = 0; round < 6; ++round) {
        const auto chunk = ramp(24, next);
        next += 24;
        REQUIRE(buffer.write(chunk.data(), chunk.size()) == 24);

        std::size_t got = 0;
        while ((got = buffer.read(out.data(), out.size())) > 0) {
            seen.append(out.data(), got);
        }
    }

    REQUIRE(seen.size() == 144);
    for (std::size_t i = 0; i < seen.size(); ++i) {
        REQUIRE(static_cast<unsigned char>(seen[i]) == (i & 0xFF));
    }
}

TEST_CASE("the producer stops at half the ring so history survives") {
    StreamBuffer buffer{64};

    const auto chunk = ramp(64);
    REQUIRE(buffer.write(chunk.data(), chunk.size()) == 32);
    REQUIRE(buffer.writable() == 0);
    REQUIRE(buffer.write(chunk.data(), 1) == 0);
}

TEST_CASE("a backward seek reads history still in the ring") {
    StreamBuffer buffer{64};

    const auto chunk = ramp(32);
    REQUIRE(buffer.write(chunk.data(), chunk.size()) == 32);

    std::vector<char> out(32);
    REQUIRE(buffer.read(out.data(), 32) == 32);
    REQUIRE(buffer.tell() == 32);
    REQUIRE(buffer.buffered() == 0);

    // Those bytes were consumed but never overwritten, so going back is free.
    REQUIRE(buffer.seekWithinWindow(8));
    REQUIRE(buffer.tell() == 8);
    REQUIRE(buffer.buffered() == 24);

    REQUIRE(buffer.read(out.data(), 4) == 4);
    REQUIRE(static_cast<unsigned char>(out[0]) == 8);
    REQUIRE(static_cast<unsigned char>(out[3]) == 11);
}

TEST_CASE("a forward seek within the window waits rather than reconnecting") {
    StreamBuffer buffer{64};

    // Nothing buffered yet, but the target is close enough to wait for.
    REQUIRE(buffer.seekWithinWindow(20));
    REQUIRE(buffer.tell() == 20);
    REQUIRE(buffer.pendingSkip() == 20);
    REQUIRE(buffer.readable() == 0);  // the skip has to be consumed first

    const auto chunk = ramp(32);
    REQUIRE(buffer.write(chunk.data(), chunk.size()) == 32);

    REQUIRE(buffer.applyPendingSkip());  // 32 buffered covers a 20-byte skip
    REQUIRE(buffer.pendingSkip() == 0);
    REQUIRE(buffer.tell() == 20);

    std::vector<char> out(4);
    REQUIRE(buffer.read(out.data(), 4) == 4);
    REQUIRE(static_cast<unsigned char>(out[0]) == 20);
}

TEST_CASE("a skip larger than what has arrived is consumed in instalments") {
    StreamBuffer buffer{64};

    REQUIRE(buffer.seekWithinWindow(40));
    REQUIRE(buffer.pendingSkip() == 40);

    const auto first = ramp(16);
    REQUIRE(buffer.write(first.data(), first.size()) == 16);
    REQUIRE(!buffer.applyPendingSkip());
    REQUIRE(buffer.pendingSkip() == 24);
    REQUIRE(buffer.readable() == 0);

    const auto second = ramp(32, 16);
    REQUIRE(buffer.write(second.data(), second.size()) == 32);
    REQUIRE(buffer.applyPendingSkip());
    REQUIRE(buffer.tell() == 40);

    std::vector<char> out(1);
    REQUIRE(buffer.read(out.data(), 1) == 1);
    REQUIRE(static_cast<unsigned char>(out[0]) == 40);
}

TEST_CASE("a seek too far in either direction asks for a reconnect") {
    StreamBuffer buffer{64};

    const auto chunk = ramp(32);
    REQUIRE(buffer.write(chunk.data(), chunk.size()) == 32);
    std::vector<char> out(32);
    REQUIRE(buffer.read(out.data(), 32) == 32);

    REQUIRE(!buffer.seekWithinWindow(1000));  // past a whole buffer ahead
    REQUIRE(buffer.tell() == 32);             // and the cursor did not move
}

TEST_CASE("history from before a reconnect is not offered as the new stream") {
    // Cog's backward-seek test is pure ring geometry, so after a restart -- which
    // moves the cursor while leaving the previous request's bytes in place -- it
    // says those stale bytes are reachable.
    StreamBuffer buffer{64};

    const auto chunk = ramp(32);
    REQUIRE(buffer.write(chunk.data(), chunk.size()) == 32);
    std::vector<char> out(32);
    REQUIRE(buffer.read(out.data(), 32) == 32);

    buffer.reset(4096);
    REQUIRE(buffer.tell() == 4096);
    REQUIRE(buffer.oldestValid() == 4096);

    // Geometrically this is a 16-byte step back into a ring that holds bytes.
    // None of them describe offset 4080.
    REQUIRE(!buffer.seekWithinWindow(4080));
    REQUIRE(buffer.tell() == 4096);
}

TEST_CASE("the resume offset skips what is already buffered") {
    StreamBuffer buffer{64};

    const auto chunk = ramp(24);
    REQUIRE(buffer.write(chunk.data(), chunk.size()) == 24);
    std::vector<char> out(8);
    REQUIRE(buffer.read(out.data(), 8) == 8);

    // A Range request must not re-fetch the 16 bytes still in the ring.
    REQUIRE(buffer.pos() == 8);
    REQUIRE(buffer.buffered() == 16);
    REQUIRE(buffer.resumeOffset() == 24);
}

// ---------------------------------------------------------------------------
// IcyDemux -- headers
// ---------------------------------------------------------------------------

TEST_CASE("transport headers give the MIME type without their parameters") {
    IcyDemux demux;
    demux.feedHeaderLine("HTTP/1.1 200 OK\r\n");
    demux.feedHeaderLine("Content-Type: Audio/MPEG; charset=UTF-8\r\n");
    demux.feedHeaderLine("Content-Length: 12345\r\n");
    demux.feedHeaderLine("\r\n");

    REQUIRE(demux.headersComplete());
    REQUIRE(demux.headers().contentType == "audio/mpeg");
    REQUIRE(demux.headers().contentLength == 12345);
    REQUIRE(!demux.headers().continuous);
}

TEST_CASE("a redirect's headers do not describe the audio") {
    IcyDemux demux;
    demux.feedHeaderLine("HTTP/1.1 302 Found\r\n");
    demux.feedHeaderLine("Content-Type: text/html\r\n");
    demux.feedHeaderLine("Content-Length: 210\r\n");
    demux.feedHeaderLine("\r\n");

    demux.feedHeaderLine("HTTP/1.1 200 OK\r\n");
    REQUIRE(!demux.headersComplete());  // the hop reopened the header block
    demux.feedHeaderLine("Content-Type: audio/ogg\r\n");
    demux.feedHeaderLine("\r\n");

    REQUIRE(demux.headers().contentType == "audio/ogg");
    REQUIRE(demux.headers().contentLength == -1);
}

TEST_CASE("an icy header marks the stream continuous and lengthless") {
    IcyDemux demux;
    demux.feedHeaderLine("HTTP/1.0 200 OK\r\n");
    demux.feedHeaderLine("Content-Length: 999\r\n");
    demux.feedHeaderLine("icy-name: Test Radio\r\n");
    demux.feedHeaderLine("icy-metaint: 16\r\n");
    demux.feedHeaderLine("\r\n");

    REQUIRE(demux.headers().continuous);
    REQUIRE(demux.headers().contentLength == -1);
    REQUIRE(demux.headers().metaint == 16);
    REQUIRE(demux.headers().name == "Test Radio");
}

TEST_CASE("a SHOUTcast reply carries its headers in the body") {
    // libcurl reports "ICY 200 OK" as an HTTP/0.9 response: no headers at all,
    // body from the first byte. The header block therefore arrives here.
    IcyDemux demux;
    Collector collector;

    const auto response = bytesOf(
        "ICY 200 OK\r\n"
        "icy-name: Body Radio\r\n"
        "icy-metaint: 8\r\n"
        "content-type: audio/aacp\r\n"
        "\r\n"
        "12345678");

    REQUIRE(demux.feedBody(response, collector.sink()));
    REQUIRE(demux.headersComplete());
    REQUIRE(demux.headers().name == "Body Radio");
    REQUIRE(demux.headers().metaint == 8);
    REQUIRE(demux.headers().contentType == "audio/aacp");
    REQUIRE(demux.headers().continuous);
    REQUIRE(collector.audio == "12345678");
}

TEST_CASE("in-body headers split across packets still parse") {
    IcyDemux  demux;
    Collector collector;

    // The split lands inside the status line, which is the case a parser that
    // decides on the first packet gets wrong.
    REQUIRE(demux.feedBody(bytesOf("ICY 2"), collector.sink()));
    REQUIRE(!demux.headersComplete());
    REQUIRE(demux.feedBody(bytesOf("00 OK\r\nicy-met"), collector.sink()));
    REQUIRE(!demux.headersComplete());
    REQUIRE(demux.feedBody(bytesOf("aint: 4\r\n\r\nabcd"), collector.sink()));

    REQUIRE(demux.headersComplete());
    REQUIRE(demux.headers().metaint == 4);
    REQUIRE(collector.audio == "abcd");
}

TEST_CASE("a body that is not SHOUTcast is audio from the first byte") {
    IcyDemux  demux;
    Collector collector;

    // Built with an explicit length: audio contains NULs, and a literal would
    // stop at the first one and stop testing what this is about.
    const std::string page("OggS\0\2", 6);
    REQUIRE(demux.feedBody(bytesOf(page), collector.sink()));
    REQUIRE(demux.headersComplete());
    REQUIRE(collector.audio == page);
}

// ---------------------------------------------------------------------------
// IcyDemux -- metadata framing
// ---------------------------------------------------------------------------

TEST_CASE("metadata blocks are lifted out of the audio") {
    IcyDemux demux;
    demux.feedHeaderLine("HTTP/1.0 200 OK\r\n");
    demux.feedHeaderLine("icy-metaint: 8\r\n");
    demux.feedHeaderLine("\r\n");

    Collector collector;

    std::string stream = "AAAAAAAA";
    stream += metaBlock("StreamTitle='A - B';");
    stream += "BBBBBBBB";

    REQUIRE(demux.feedBody(bytesOf(stream), collector.sink()));
    REQUIRE(collector.audio == "AAAAAAAABBBBBBBB");

    const auto tags = demux.takeUpdatedMetadata();
    REQUIRE(tags.first("artist") == "A");
    REQUIRE(tags.first("title") == "B");
}

TEST_CASE("a zero-length metadata block just resumes the audio") {
    IcyDemux demux;
    demux.feedHeaderLine("HTTP/1.0 200 OK\r\n");
    demux.feedHeaderLine("icy-metaint: 4\r\n");
    demux.feedHeaderLine("\r\n");

    Collector   collector;
    std::string stream = "aaaa";
    stream.push_back('\0');
    stream += "bbbb";
    stream.push_back('\0');
    stream += "cccc";

    REQUIRE(demux.feedBody(bytesOf(stream), collector.sink()));
    REQUIRE(collector.audio == "aaaabbbbcccc");
}

TEST_CASE("framing survives arbitrary packet boundaries") {
    // The network decides where packets split, and it will eventually split
    // inside the length byte and inside a metadata block.
    const int metaint = 5;

    const std::string block = metaBlock("StreamTitle='X';");

    std::string stream;
    std::string expected;
    for (int i = 0; i < 4; ++i) {
        const std::string audio(static_cast<std::size_t>(metaint),
                                static_cast<char>('a' + i));
        stream += audio;
        expected += audio;
        stream += block;
    }

    for (std::size_t step : {std::size_t{1}, std::size_t{3}, std::size_t{7},
                             std::size_t{16}, std::size_t{100}}) {
        IcyDemux demux;
        demux.feedHeaderLine("HTTP/1.0 200 OK\r\n");
        demux.feedHeaderLine("icy-metaint: 5\r\n");
        demux.feedHeaderLine("\r\n");

        Collector collector;
        for (std::size_t at = 0; at < stream.size(); at += step) {
            const auto slice = stream.substr(at, step);
            REQUIRE(demux.feedBody(bytesOf(slice), collector.sink()));
        }
        REQUIRE(collector.audio == expected);
    }
}

TEST_CASE("a metadata block without a StreamTitle does not break the framing") {
    // Cog switches icy_metaint off when its parser finds no StreamTitle, so every
    // later metadata block reaches the decoder as audio.
    IcyDemux demux;
    demux.feedHeaderLine("HTTP/1.0 200 OK\r\n");
    demux.feedHeaderLine("icy-metaint: 4\r\n");
    demux.feedHeaderLine("\r\n");

    Collector collector;

    std::string stream = "aaaa";
    stream += metaBlock("StreamUrl='http://example.invalid/';");
    stream += "bbbb";
    stream += metaBlock("StreamTitle='Later';");
    stream += "cccc";

    REQUIRE(demux.feedBody(bytesOf(stream), collector.sink()));
    REQUIRE(collector.audio == "aaaabbbbcccc");
    REQUIRE(demux.takeUpdatedMetadata().first("title") == "Later");
}

TEST_CASE("a refused sink stops the transfer") {
    IcyDemux demux;
    demux.feedHeaderLine("HTTP/1.0 200 OK\r\n");
    demux.feedHeaderLine("\r\n");

    Collector collector;
    collector.refuseAfter = 4;

    REQUIRE(!demux.feedBody(bytesOf("aaaabbbb"), collector.sink()));
    REQUIRE(collector.audio == "aaaa");
}

TEST_CASE("a reconnect keeps the tags and restarts the framing") {
    IcyDemux demux;
    demux.feedHeaderLine("HTTP/1.0 200 OK\r\n");
    demux.feedHeaderLine("icy-name: Kept\r\n");
    demux.feedHeaderLine("icy-metaint: 4\r\n");
    demux.feedHeaderLine("\r\n");

    Collector   collector;
    std::string stream = "aaaa";
    stream += metaBlock("StreamTitle='Before';");
    REQUIRE(demux.feedBody(bytesOf(stream), collector.sink()));
    REQUIRE(demux.takeUpdatedMetadata().first("title") == "Before");

    demux.resetTransport();
    REQUIRE(!demux.headersComplete());
    REQUIRE(demux.headers().name == "Kept");

    // The new response announces its own metaint and starts the cycle over.
    demux.feedHeaderLine("HTTP/1.0 200 OK\r\n");
    demux.feedHeaderLine("icy-metaint: 4\r\n");
    demux.feedHeaderLine("\r\n");
    REQUIRE(demux.feedBody(bytesOf("bbbb"), collector.sink()));
    REQUIRE(collector.audio == "aaaabbbb");
}

// ---------------------------------------------------------------------------
// IcyDemux -- StreamTitle
// ---------------------------------------------------------------------------

TEST_CASE("StreamTitle splits on the first dash and tolerates the rest") {
    std::string artist;
    std::string title;

    REQUIRE(IcyDemux::parseStreamTitle("StreamTitle='Artist - Song';", artist, title));
    REQUIRE(artist == "Artist");
    REQUIRE(title == "Song");

    // No separator: a title with no artist, not a mis-split.
    REQUIRE(IcyDemux::parseStreamTitle("StreamTitle='Just A Title';", artist, title));
    REQUIRE(artist.empty());
    REQUIRE(title == "Just A Title");

    // Later fields must not be swallowed by the value.
    REQUIRE(IcyDemux::parseStreamTitle(
        "StreamTitle='A - B';StreamUrl='http://example.invalid/';", artist, title));
    REQUIRE(artist == "A");
    REQUIRE(title == "B");

    // Cog's "hack for a certain stream".
    REQUIRE(IcyDemux::parseStreamTitle("StreamTitle='A - text=\"B\"';", artist, title));
    REQUIRE(artist == "A");
    REQUIRE(title == "B");

    REQUIRE(!IcyDemux::parseStreamTitle("StreamUrl='http://example.invalid/';",
                                        artist, title));
    REQUIRE(!IcyDemux::parseStreamTitle("StreamTitle='unterminated", artist, title));
}

TEST_CASE("stream titles that are not UTF-8 survive as Latin-1") {
    std::string artist;
    std::string title;

    // 0xE9 is 'e' with an acute in Latin-1 and invalid on its own in UTF-8.
    const std::string block = std::string("StreamTitle='Caf\xE9';");
    REQUIRE(IcyDemux::parseStreamTitle(block, artist, title));
    REQUIRE(title == "Caf\xC3\xA9");
}

TEST_CASE("only changed metadata is reported") {
    IcyDemux demux;
    demux.feedHeaderLine("HTTP/1.0 200 OK\r\n");
    demux.feedHeaderLine("icy-metaint: 4\r\n");
    demux.feedHeaderLine("icy-genre: Jazz\r\n");
    demux.feedHeaderLine("\r\n");

    REQUIRE(demux.takeUpdatedMetadata().first("genre") == "Jazz");
    REQUIRE(demux.takeUpdatedMetadata().empty());

    Collector         collector;
    const std::string same = metaBlock("StreamTitle='A - B';");

    REQUIRE(demux.feedBody(bytesOf("aaaa" + same), collector.sink()));
    REQUIRE(demux.takeUpdatedMetadata().first("title") == "B");

    // The same title again is not news.
    const std::string repeat = "bbbb" + same;
    REQUIRE(demux.feedBody(bytesOf(repeat), collector.sink()));
    REQUIRE(demux.takeUpdatedMetadata().empty());
}
