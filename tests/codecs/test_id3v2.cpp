// The ID3v2 parser, which exists because libavformat's is internal.
//
// Everything awkward about ID3v2 is a size or an encoding, and both fail
// quietly: a size read the wrong way walks into the middle of the next frame and
// yields a tag that is merely wrong rather than absent, and a text encoding read
// the wrong way yields mojibake or an empty string. Neither stops a stream
// playing, so neither is visible without a test that says what the bytes were.

#include "common/Id3v2.hpp"

#include "xpcog/core/MetadataMap.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using xpcog::MetadataMap;
using xpcog::codecs::id3v2TagLength;
using xpcog::codecs::parseId3v2;

namespace {

/// Builds an ID3v2 tag frame by frame. Sizes are the point of the exercise, so
/// they are computed here rather than written out by hand.
class TagBuilder {
public:
    explicit TagBuilder(int major) : major_(major) {}

    /// `payload` is the frame body exactly as it appears on the wire, encoding
    /// byte included.
    TagBuilder& frame(std::string_view identifier, const std::vector<std::uint8_t>& payload) {
        frames_.insert(frames_.end(), identifier.begin(), identifier.end());
        if (major_ == 2) {
            appendBigEndian(frames_, payload.size(), 3);
        } else if (major_ == 3) {
            appendBigEndian(frames_, payload.size(), 4);
        } else {
            appendSyncsafe(frames_, payload.size());
        }
        if (major_ != 2) {
            frames_.insert(frames_.end(), {0x00, 0x00});  // frame flags
        }
        frames_.insert(frames_.end(), payload.begin(), payload.end());
        return *this;
    }

    /// A text frame in the given ID3 encoding, values separated by that
    /// encoding's terminator.
    TagBuilder& text(std::string_view identifier, std::uint8_t encoding,
                     const std::vector<std::string>& values) {
        std::vector<std::uint8_t> payload{encoding};
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i > 0) {
                payload.push_back(0x00);
                if (encoding == 1 || encoding == 2) {
                    payload.push_back(0x00);
                }
            }
            appendEncoded(payload, encoding, values[i], i == 0);
        }
        return frame(identifier, payload);
    }

    TagBuilder& padding(std::size_t bytes) {
        padding_ = bytes;
        return *this;
    }

    TagBuilder& footer() {
        footer_ = true;
        return *this;
    }

    [[nodiscard]] std::vector<std::uint8_t> build() const {
        std::vector<std::uint8_t> out{'I', 'D', '3', static_cast<std::uint8_t>(major_), 0x00};
        out.push_back(footer_ ? 0x10 : 0x00);

        const std::size_t bodySize = frames_.size() + padding_;
        appendSyncsafe(out, bodySize);
        out.insert(out.end(), frames_.begin(), frames_.end());
        out.insert(out.end(), padding_, 0x00);
        if (footer_) {
            out.insert(out.end(), {'3', 'D', 'I', static_cast<std::uint8_t>(major_), 0x00, 0x10});
            appendSyncsafe(out, bodySize);
        }
        return out;
    }

private:
    static void appendBigEndian(std::vector<std::uint8_t>& out, std::size_t value,
                                int width) {
        for (int i = width - 1; i >= 0; --i) {
            out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
        }
    }

    static void appendSyncsafe(std::vector<std::uint8_t>& out, std::size_t value) {
        for (int i = 3; i >= 0; --i) {
            out.push_back(static_cast<std::uint8_t>((value >> (7 * i)) & 0x7F));
        }
    }

    /// `withBom` only matters for encoding 1, where every string carries its own
    /// byte-order mark.
    static void appendEncoded(std::vector<std::uint8_t>& out, std::uint8_t encoding,
                              std::string_view text, bool /*withBom*/) {
        switch (encoding) {
            case 1:
                out.insert(out.end(), {0xFF, 0xFE});  // little-endian BOM
                for (const char c : text) {
                    out.push_back(static_cast<std::uint8_t>(c));
                    out.push_back(0x00);
                }
                break;
            case 2:
                for (const char c : text) {
                    out.push_back(0x00);
                    out.push_back(static_cast<std::uint8_t>(c));
                }
                break;
            default:
                out.insert(out.end(), text.begin(), text.end());
                break;
        }
    }

    int                       major_;
    std::vector<std::uint8_t> frames_;
    std::size_t               padding_ = 0;
    bool                      footer_  = false;
};

std::span<const std::byte> bytesOf(const std::vector<std::uint8_t>& data) {
    return {reinterpret_cast<const std::byte*>(data.data()), data.size()};
}

std::string titleOf(const std::vector<std::uint8_t>& tag) {
    MetadataMap tags;
    if (!parseId3v2(bytesOf(tag), tags)) {
        return "<unparsed>";
    }
    return std::string{tags.first("title")};
}

}  // namespace

TEST_CASE("ID3v2 reads text frames from every version", "[id3v2]") {
    // The size field is encoded differently in each: 24-bit in .2, plain 32-bit
    // in .3, syncsafe in .4. Read the wrong way, parsing walks into the middle of
    // the next frame.
    CHECK(titleOf(TagBuilder{2}.text("TT2", 0, {"Two"}).build()) == "Two");
    CHECK(titleOf(TagBuilder{3}.text("TIT2", 0, {"Three"}).build()) == "Three");
    CHECK(titleOf(TagBuilder{4}.text("TIT2", 3, {"Four"}).build()) == "Four");
}

TEST_CASE("ID3v2 decodes every text encoding", "[id3v2]") {
    CHECK(titleOf(TagBuilder{4}.text("TIT2", 0, {"Latin"}).build()) == "Latin");
    // UTF-16 with a byte-order mark, UTF-16 big-endian without one, and UTF-8.
    CHECK(titleOf(TagBuilder{4}.text("TIT2", 1, {"Marked"}).build()) == "Marked");
    CHECK(titleOf(TagBuilder{4}.text("TIT2", 2, {"BigEndian"}).build()) == "BigEndian");
    CHECK(titleOf(TagBuilder{4}.text("TIT2", 3, {"Eight"}).build()) == "Eight");

    // A UTF-8 payload declared as Latin-1, which taggers write constantly.
    // Decoding it as Latin-1 would give "Ã–" where the station wrote "Ö".
    CHECK(titleOf(TagBuilder{4}.text("TIT2", 0, {"Bj\xC3\xB6rk"}).build()) == "Bj\xC3\xB6rk");
}

TEST_CASE("ID3v2 maps frames to the names FFmpeg uses", "[id3v2]") {
    // Same names, so a tag parsed here merges with one the demuxer harvested
    // rather than sitting beside it under a second key.
    const auto tag = TagBuilder{4}
                         .text("TIT2", 3, {"Title"})
                         .text("TPE1", 3, {"Artist"})
                         .text("TALB", 3, {"Album"})
                         .text("TRCK", 3, {"4/12"})
                         .text("TDRC", 3, {"2026"})
                         .build();

    MetadataMap tags;
    REQUIRE(parseId3v2(bytesOf(tag), tags));
    CHECK(tags.first("title") == "Title");
    CHECK(tags.first("artist") == "Artist");
    CHECK(tags.first("album") == "Album");
    CHECK(tags.first("track") == "4/12");
    CHECK(tags.first("date") == "2026");
}

TEST_CASE("ID3v2 keeps every value of a multi-value frame", "[id3v2]") {
    // ID3v2.4 separates them with the encoding's own terminator, so the split has
    // to happen before decoding rather than after.
    const auto tag = TagBuilder{4}.text("TPE1", 3, {"First", "Second"}).build();

    MetadataMap tags;
    REQUIRE(parseId3v2(bytesOf(tag), tags));
    CHECK(tags.joined("artist") == "First, Second");
}

TEST_CASE("ID3v2 reads TXXX under its own description", "[id3v2]") {
    const auto tag =
        TagBuilder{4}.text("TXXX", 3, {"replaygain_track_gain", "-7.2 dB"}).build();

    MetadataMap tags;
    REQUIRE(parseId3v2(bytesOf(tag), tags));
    CHECK(tags.first("replaygain_track_gain") == "-7.2 dB");
}

TEST_CASE("ID3v2 skips the language code on a comment", "[id3v2]") {
    // COMM is encoding, three bytes of language, a description and then the text.
    // Miss the language and the comment reads "engSomething".
    std::vector<std::uint8_t> payload{0x03, 'e', 'n', 'g'};
    payload.push_back(0x00);  // empty description
    const std::string body = "Recorded live";
    payload.insert(payload.end(), body.begin(), body.end());

    MetadataMap tags;
    REQUIRE(parseId3v2(bytesOf(TagBuilder{4}.frame("COMM", payload).build()), tags));
    CHECK(tags.first("comment") == "Recorded live");
}

TEST_CASE("ID3v2 files a named comment under its name", "[id3v2]") {
    // iTunes puts its gapless information in a COMM frame described as
    // "iTunSMPB". Filed under "comment" it is both unreadable prose and
    // unfindable by the decoder that needs it.
    std::vector<std::uint8_t> payload{0x03, 'e', 'n', 'g'};
    const std::string         description = "iTunSMPB";
    payload.insert(payload.end(), description.begin(), description.end());
    payload.push_back(0x00);
    const std::string value = " 00000000 00000210 00000AC0 0000000000A2F930";
    payload.insert(payload.end(), value.begin(), value.end());

    MetadataMap tags;
    REQUIRE(parseId3v2(bytesOf(TagBuilder{4}.frame("COMM", payload).build()), tags));
    CHECK(tags.first("itunsmpb") == value);
    // And it is not also sitting under the generic name.
    CHECK_FALSE(tags.contains("comment"));
}

TEST_CASE("ID3v2 ignores PRIV frames", "[id3v2]") {
    // Every HLS transport-stream segment carries
    // com.apple.streaming.transportStreamTimestamp in one, and its value moves
    // every segment. Surfacing it would rename the track for the whole broadcast.
    std::vector<std::uint8_t> priv;
    const std::string owner = "com.apple.streaming.transportStreamTimestamp";
    priv.insert(priv.end(), owner.begin(), owner.end());
    priv.push_back(0x00);
    priv.insert(priv.end(), {0, 0, 0, 0, 0, 0, 0, 42});

    MetadataMap tags;
    // Nothing readable in the tag at all, so the parse reports it found nothing.
    CHECK_FALSE(parseId3v2(bytesOf(TagBuilder{4}.frame("PRIV", priv).build()), tags));
    CHECK(tags.empty());

    // And a tag holding both keeps only the half that names something.
    MetadataMap mixed;
    const auto  tag =
        TagBuilder{4}.frame("PRIV", priv).text("TIT2", 3, {"On Air"}).build();
    REQUIRE(parseId3v2(bytesOf(tag), mixed));
    CHECK(mixed.size() == 1);
    CHECK(mixed.first("title") == "On Air");
}

TEST_CASE("ID3v2 stops at padding", "[id3v2]") {
    // The zero bytes after the last frame are not a frame whose identifier
    // happens to be empty.
    const auto tag = TagBuilder{4}.text("TIT2", 3, {"Padded"}).padding(64).build();

    MetadataMap tags;
    REQUIRE(parseId3v2(bytesOf(tag), tags));
    CHECK(tags.size() == 1);
    CHECK(tags.first("title") == "Padded");
}

TEST_CASE("ID3v2 tolerates a .4 frame size written the .3 way", "[id3v2]") {
    // Taggers get this wrong often enough that FFmpeg carries the same fallback.
    // A syncsafe field never has a high bit set, so one that does cannot be
    // syncsafe -- and read as syncsafe it gives a size far too small, ending the
    // parse at the first frame.
    auto tag = TagBuilder{4}.text("TIT2", 3, {"Nonstandard"}).build();

    // Frame size lives at offset 10 + 4; make the value 0x80-or-greater in a way
    // only a plain integer can express.
    const std::size_t payload = std::string_view{"Nonstandard"}.size() + 1;
    REQUIRE(payload < 0x80);
    tag[10 + 4] = 0x00;
    tag[10 + 5] = 0x00;
    tag[10 + 6] = 0x00;
    tag[10 + 7] = static_cast<std::uint8_t>(payload);
    CHECK(titleOf(tag) == "Nonstandard");
}

TEST_CASE("ID3v2 tag length covers the header, body and footer", "[id3v2]") {
    const auto plain = TagBuilder{4}.text("TIT2", 3, {"Sized"}).padding(16).build();
    CHECK(id3v2TagLength(bytesOf(plain)) == plain.size());

    // Only ID3v2.4 defines a footer, and a reader that misses it steps ten bytes
    // short and starts decoding "3DI" as audio.
    const auto withFooter = TagBuilder{4}.text("TIT2", 3, {"Sized"}).footer().build();
    CHECK(id3v2TagLength(bytesOf(withFooter)) == withFooter.size());
}

TEST_CASE("ID3v2 declines what is not a tag", "[id3v2]") {
    MetadataMap tags;

    const std::vector<std::uint8_t> notATag{0xFF, 0xF1, 0x4C, 0x80, 0x00, 0x1F,
                                            0xFC, 0x00, 0x00, 0x00};
    CHECK(id3v2TagLength(bytesOf(notATag)) == 0);
    CHECK_FALSE(parseId3v2(bytesOf(notATag), tags));

    const std::vector<std::uint8_t> tooShort{'I', 'D', '3', 0x04};
    CHECK(id3v2TagLength(bytesOf(tooShort)) == 0);
    CHECK_FALSE(parseId3v2(bytesOf(tooShort), tags));

    // "ID3" followed by a size field that cannot be syncsafe: three bytes of
    // audio that happen to spell the magic.
    std::vector<std::uint8_t> falseMatch{'I', 'D', '3', 0x04, 0x00, 0x00,
                                         0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(id3v2TagLength(bytesOf(falseMatch)) == 0);
}

TEST_CASE("ID3v2 reads what it can of a truncated tag", "[id3v2]") {
    // A tag that arrives in a lost packet still has the title at the front, and
    // refusing the whole thing throws that away for nothing.
    auto tag = TagBuilder{4}.text("TIT2", 3, {"Survivor"}).text("TALB", 3, {"Gone"}).build();
    tag.resize(tag.size() - 6);

    MetadataMap tags;
    REQUIRE(parseId3v2(bytesOf(tag), tags));
    CHECK(tags.first("title") == "Survivor");
}
