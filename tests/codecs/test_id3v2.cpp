// The ID3v2 parser, which exists because libavformat's is internal.
//
// Everything awkward about ID3v2 is a size or an encoding, and both fail
// quietly: a size read the wrong way walks into the middle of the next frame and
// yields a tag that is merely wrong rather than absent, and a text encoding read
// the wrong way yields mojibake or an empty string. Neither stops a stream
// playing, so neither is visible without a test that says what the bytes were.

#include "common/Id3v2.hpp"

#include "xpcog/core/MetadataMap.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

#include "../TestShell.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using xpcog::MetadataMap;
using xpcog::codecs::id3v2TagLength;
using xpcog::codecs::parseId3v2;
using xpcog::codecs::parseId3v2Chapters;

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

    TagBuilder& unsynchronised() {
        unsync_ = true;
        return *this;
    }

    [[nodiscard]] std::vector<std::uint8_t> build() const {
        std::vector<std::uint8_t> body = frames_;
        body.insert(body.end(), padding_, 0x00);
        if (unsync_) {
            // Every 0xFF gains a 0x00 after it, which is what the parser's
            // deUnsynchronise undoes.
            std::vector<std::uint8_t> escaped;
            escaped.reserve(body.size());
            for (const std::uint8_t byte : body) {
                escaped.push_back(byte);
                if (byte == 0xFF) {
                    escaped.push_back(0x00);
                }
            }
            body = std::move(escaped);
        }

        std::vector<std::uint8_t> out{'I', 'D', '3', static_cast<std::uint8_t>(major_), 0x00};
        out.push_back(static_cast<std::uint8_t>((footer_ ? 0x10 : 0x00) | (unsync_ ? 0x80 : 0x00)));

        appendSyncsafe(out, body.size());
        out.insert(out.end(), body.begin(), body.end());
        if (footer_) {
            out.insert(out.end(), {'3', 'D', 'I', static_cast<std::uint8_t>(major_), 0x00, 0x10});
            appendSyncsafe(out, body.size());
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
    bool                      unsync_  = false;
};

std::span<const std::byte> bytesOf(const std::vector<std::uint8_t>& data) {
    return {reinterpret_cast<const std::byte*>(data.data()), data.size()};
}

void appendBigEndian(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 3; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
    }
}

void appendSyncsafe(std::vector<std::uint8_t>& out, std::size_t value) {
    for (int i = 3; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>((value >> (7 * i)) & 0x7F));
    }
}

/// A text payload in the given ID3 encoding, as a CHAP's embedded TIT2 holds
/// it: encoding byte then the words, no terminator needed for a single value.
std::vector<std::uint8_t> encodedText(std::uint8_t encoding, std::string_view text) {
    std::vector<std::uint8_t> out{encoding};
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
    return out;
}

/// A CHAP payload: the element ID, start and end in milliseconds, the two
/// byte offsets nobody reads, and an embedded TIT2 carrying the title.
std::vector<std::uint8_t> chapPayload(std::string_view id, std::uint32_t startMs,
                                      std::uint32_t endMs, std::string_view title,
                                      std::uint8_t encoding, int major) {
    std::vector<std::uint8_t> out{id.begin(), id.end()};
    out.push_back(0x00);
    appendBigEndian(out, startMs);
    appendBigEndian(out, endMs);
    appendBigEndian(out, 0xFFFFFFFFu);
    appendBigEndian(out, 0xFFFFFFFFu);

    const std::vector<std::uint8_t> text = encodedText(encoding, title);
    out.insert(out.end(), {'T', 'I', 'T', '2'});
    if (major == 3) {
        appendBigEndian(out, static_cast<std::uint32_t>(text.size()));
    } else {
        appendSyncsafe(out, text.size());
    }
    out.insert(out.end(), {0x00, 0x00});  // frame flags
    out.insert(out.end(), text.begin(), text.end());
    return out;
}

/// A CTOC payload: element ID, flags (0x02 top-level, 0x01 ordered), entry
/// count, and the child element IDs in the order the table lists them.
std::vector<std::uint8_t> ctocPayload(std::string_view id, std::uint8_t flags,
                                      const std::vector<std::string>& children) {
    std::vector<std::uint8_t> out{id.begin(), id.end()};
    out.push_back(0x00);
    out.push_back(flags);
    out.push_back(static_cast<std::uint8_t>(children.size()));
    for (const std::string& child : children) {
        out.insert(out.end(), child.begin(), child.end());
        out.push_back(0x00);
    }
    return out;
}

xpcog::ChapterList chaptersOf(const std::vector<std::uint8_t>& tag) {
    xpcog::ChapterList chapters;
    REQUIRE(xpcog::codecs::parseId3v2Chapters(bytesOf(tag), chapters));
    return chapters;
}

/// A second of silent 16-bit stereo 44.1 kHz WAV, the encoder's raw material.
void writeSilentWav(const std::filesystem::path& wav) {
    std::ofstream out{wav, std::ios::binary};
    const std::vector<std::uint8_t> silence(44100 * 4, 0);
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(silence.size());
    const auto write32 = [&out](std::uint32_t v) {
        const char b[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
                           static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF)};
        out.write(b, 4);
    };
    const auto write16 = [&out](std::uint16_t v) {
        const char b[2] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
        out.write(b, 2);
    };
    out.write("RIFF", 4);
    write32(36 + dataBytes);
    out.write("WAVEfmt ", 8);
    write32(16);
    write16(1);
    write16(2);
    write32(44100);
    write32(44100 * 4);
    write16(4);
    write16(16);
    out.write("data", 4);
    write32(dataBytes);
    out.write(reinterpret_cast<const char*>(silence.data()), dataBytes);
}

std::vector<std::uint8_t> fileBytes(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    return std::vector<std::uint8_t>{std::istreambuf_iterator<char>{in},
                                     std::istreambuf_iterator<char>{}};
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

// --- chapters: the sibling parser -------------------------------------------

TEST_CASE("ID3v2 chapters follow the top-level ordered CTOC", "[id3v2][chapters]") {
    // The table's order deliberately differs from start order: it is the
    // order a player lists, and the reader must keep it.
    const auto tag =
        TagBuilder{4}
            .frame("CHAP", chapPayload("c2", 5000, 9000, "Second", 3, 4))
            .frame("CHAP", chapPayload("c1", 0, 5000, "First", 3, 4))
            .frame("CHAP", chapPayload("c3", 9000, 12000, "Third", 3, 4))
            .frame("CTOC", ctocPayload("toc", 0x03, {"c3", "c1", "c2"}))
            .build();

    const xpcog::ChapterList chapters = chaptersOf(tag);
    REQUIRE(chapters.size() == 3);
    CHECK(chapters[0].title == "Third");
    CHECK(chapters[1].title == "First");
    CHECK(chapters[2].title == "Second");
    CHECK(chapters[0].startMs == 9000);
    // The element IDs ride along opaque, for the writer to keep.
    CHECK(chapters[1].id == "c1");
}

TEST_CASE("ID3v2 chapters fall back to start order without an ordered top-level CTOC",
          "[id3v2][chapters]") {
    const auto chapFrames = [](TagBuilder& builder) {
        builder.frame("CHAP", chapPayload("c2", 5000, 9000, "Second", 3, 4))
            .frame("CHAP", chapPayload("c1", 0, 5000, "First", 3, 4));
    };

    // Top-level but unordered: the table's order is not a listing order.
    TagBuilder unordered{4};
    chapFrames(unordered);
    unordered.frame("CTOC", ctocPayload("toc", 0x02, {"c2", "c1"}));
    const xpcog::ChapterList byStart = chaptersOf(unordered.build());
    REQUIRE(byStart.size() == 2);
    CHECK(byStart[0].title == "First");
    CHECK(byStart[1].title == "Second");

    // Ordered but nested: not the top-level table, so not the listing.
    TagBuilder nested{4};
    chapFrames(nested);
    nested.frame("CTOC", ctocPayload("toc", 0x01, {"c2", "c1"}));
    const xpcog::ChapterList nestedList = chaptersOf(nested.build());
    REQUIRE(nestedList.size() == 2);
    CHECK(nestedList[0].title == "First");

    // No table at all.
    TagBuilder bare{4};
    chapFrames(bare);
    const xpcog::ChapterList bareList = chaptersOf(bare.build());
    REQUIRE(bareList.size() == 2);
    CHECK(bareList[0].title == "First");
}

TEST_CASE("ID3v2 chapters skip missing CTOC children and keep the first duplicate",
          "[id3v2][chapters]") {
    const auto tag =
        TagBuilder{4}
            .frame("CHAP", chapPayload("c1", 0, 5000, "First", 3, 4))
            .frame("CHAP", chapPayload("c1", 0, 5000, "Duplicate", 3, 4))
            .frame("CHAP", chapPayload("c2", 5000, 9000, "Second", 3, 4))
            .frame("CTOC", ctocPayload("toc", 0x03, {"c1", "missing", "c2"}))
            .build();

    const xpcog::ChapterList chapters = chaptersOf(tag);
    REQUIRE(chapters.size() == 2);
    CHECK(chapters[0].title == "First");
    CHECK(chapters[1].title == "Second");
}

TEST_CASE("ID3v2 chapter titles decode every encoding", "[id3v2][chapters]") {
    const auto titleIn = [](std::uint8_t encoding, std::string_view text) {
        const auto tag =
            TagBuilder{4}.frame("CHAP", chapPayload("c", 0, 1000, text, encoding, 4)).build();
        const xpcog::ChapterList chapters = chaptersOf(tag);
        REQUIRE(chapters.size() == 1);
        return chapters.front().title;
    };
    CHECK(titleIn(0, "Latin") == "Latin");
    CHECK(titleIn(1, "Marked") == "Marked");
    CHECK(titleIn(2, "BigEndian") == "BigEndian");
    CHECK(titleIn(3, "Eight") == "Eight");
    // UTF-8 declared as Latin-1, which taggers write constantly.
    CHECK(titleIn(0, "Bj\xC3\xB6rk") == "Bj\xC3\xB6rk");
}

TEST_CASE("ID3v2 chapters keep what they read of a malformed tag", "[id3v2][chapters]") {
    // A CHAP cut off mid-times: the frame is dropped, the good one before it
    // stays, and the walk does not run into the next frame's bytes.
    std::vector<std::uint8_t> truncated{'c', 'x', 0x00, 0x00, 0x01};
    const auto tag = TagBuilder{4}
                         .frame("CHAP", chapPayload("ok", 0, 1000, "Good", 3, 4))
                         .frame("CHAP", truncated)
                         .build();
    const xpcog::ChapterList chapters = chaptersOf(tag);
    REQUIRE(chapters.size() == 1);
    CHECK(chapters.front().title == "Good");

    // A CTOC whose child list runs off the end keeps the children it read.
    std::vector<std::uint8_t> shortToc{'t', 'o', 'c', 0x00, 0x03, 0x02, 'c', '1', 0x00, 'c'};
    const auto cutToc = TagBuilder{4}
                            .frame("CHAP", chapPayload("c1", 0, 1000, "One", 3, 4))
                            .frame("CHAP", chapPayload("c2", 1000, 2000, "Two", 3, 4))
                            .frame("CTOC", shortToc)
                            .build();
    const xpcog::ChapterList cut = chaptersOf(cutToc);
    REQUIRE(cut.size() == 1);
    CHECK(cut.front().title == "One");
}

TEST_CASE("ID3v2 chapters read an unsynchronised tag", "[id3v2][chapters]") {
    // Times full of 0xFF bytes, which unsynchronisation escapes; reading them
    // back wrong would move every chapter.
    const auto tag = TagBuilder{4}
                         .frame("CHAP", chapPayload("c1", 0x00FF00FFu, 0xFF00FF00u, "Escaped", 3, 4))
                         .unsynchronised()
                         .build();
    const xpcog::ChapterList chapters = chaptersOf(tag);
    REQUIRE(chapters.size() == 1);
    CHECK(chapters.front().startMs == 0x00FF00FFu);
    CHECK(chapters.front().endMs == 0xFF00FF00u);
    CHECK(chapters.front().title == "Escaped");
}

TEST_CASE("ID3v2 chapters decline what is not a tag, and yield nothing for v2.2",
          "[id3v2][chapters]") {
    xpcog::ChapterList chapters;
    chapters.push_back(xpcog::Chapter{});  // replaced, not appended to

    const std::vector<std::uint8_t> notATag{0xFF, 0xF1, 0x4C, 0x80, 0x00, 0x1F,
                                            0xFC, 0x00, 0x00, 0x00};
    CHECK_FALSE(parseId3v2Chapters(bytesOf(notATag), chapters));
    CHECK(chapters.empty());

    // CHAP and CTOC are four-character frames; a .2 tag cannot hold them.
    const auto v22 = TagBuilder{2}.text("TT2", 0, {"Title"}).build();
    CHECK(parseId3v2Chapters(bytesOf(v22), chapters));
    CHECK(chapters.empty());
}

TEST_CASE("ID3v2 chapters read what TagLib wrote", "[id3v2][chapters]") {
    namespace fs = std::filesystem;
    if (!xpcog::test::haveTool("ffmpeg")) {
        SKIP("ffmpeg is not installed");
    }
    static xpcog::PluginRegistry registry;
    static const bool            once = [] {
        xpcog::registerAllCodecs(registry);
        return true;
    }();
    (void)once;

    const fs::path dir = fs::temp_directory_path() / "xpcog-id3v2-chapter-tests";
    std::error_code ignored;
    fs::create_directories(dir, ignored);
    const fs::path wav = dir / "source.wav";
    writeSilentWav(wav);

    xpcog::ChapterList written;
    for (const auto& [start, title] : std::vector<std::pair<std::uint32_t, std::string>>{
             {0, "One"}, {300, "Two"}, {600, "Three"}}) {
        xpcog::Chapter chapter;
        chapter.startMs = start;
        chapter.title   = title;
        written.push_back(std::move(chapter));
    }

    // Both block versions: .4 is the default, .3 what the setting asks for.
    for (const int version : {4, 3}) {
        const fs::path mp3 = dir / ("taglib-v" + std::to_string(version) + ".mp3");
        fs::remove(mp3, ignored);
        const std::string command = "ffmpeg -loglevel error -y -i \"" + wav.string() +
                                    "\" -codec:a libmp3lame -b:a 32k \"" + mp3.string() + "\"" +
                                    xpcog::test::kSilenceStderr;
        REQUIRE(std::system(command.c_str()) == 0);

        xpcog::MetadataWriteOptions options;
        options.id3v2Version = version;
        REQUIRE(registry.writeMetadata(xpcog::Url::fromLocalPath(mp3), {}, written, options));

        const xpcog::ChapterList read = chaptersOf(fileBytes(mp3));
        REQUIRE(read.size() == 3);
        for (std::size_t i = 0; i < 3; ++i) {
            CHECK(read[i].startMs == written[i].startMs);
            CHECK(read[i].title == written[i].title);
            CHECK_FALSE(read[i].id.empty());
        }
    }
}

TEST_CASE("ID3v2 chapters read what ffmpeg wrote", "[id3v2][chapters]") {
    namespace fs = std::filesystem;
    if (!xpcog::test::haveTool("ffmpeg")) {
        SKIP("ffmpeg is not installed");
    }
    const fs::path dir = fs::temp_directory_path() / "xpcog-id3v2-chapter-tests";
    std::error_code ignored;
    fs::create_directories(dir, ignored);
    const fs::path wav = dir / "source.wav";
    writeSilentWav(wav);
    const fs::path plain = dir / "ffmpeg-plain.mp3";
    fs::remove(plain, ignored);
    const std::string encode = "ffmpeg -loglevel error -y -i \"" + wav.string() +
                               "\" -codec:a libmp3lame -b:a 32k \"" + plain.string() + "\"" +
                               xpcog::test::kSilenceStderr;
    REQUIRE(std::system(encode.c_str()) == 0);

    const fs::path meta = dir / "chapters.ffmeta";
    {
        std::ofstream out{meta};
        out << ";FFMETADATA1\n";
        for (const auto& [start, end, title] :
             std::vector<std::tuple<int, int, std::string>>{
                 {0, 45000, "Chapter One"}, {45000, 90000, "Chapter Two"},
                 {90000, 135000, "Chapter Three"}, {135000, 180000, "Chapter Four"}}) {
            out << "[CHAPTER]\nTIMEBASE=1/1000\nSTART=" << start << "\nEND=" << end
                << "\ntitle=" << title << "\n";
        }
    }

    // ffmpeg writes .4 by default and .3 on request; both must read back.
    for (const int version : {4, 3}) {
        const fs::path mp3 = dir / ("ffmpeg-v" + std::to_string(version) + ".mp3");
        fs::remove(mp3, ignored);
        const std::string command = "ffmpeg -loglevel error -y -f ffmetadata -i \"" +
                                    meta.string() + "\" -i \"" + plain.string() +
                                    "\" -c copy -map_metadata 1 -map_chapters 0 -id3v2_version " +
                                    std::to_string(version) + " \"" + mp3.string() + "\"" +
                                    xpcog::test::kSilenceStderr;
        REQUIRE(std::system(command.c_str()) == 0);

        const xpcog::ChapterList read = chaptersOf(fileBytes(mp3));
        REQUIRE(read.size() == 4);
        CHECK(read[0].startMs == 0);
        CHECK(read[1].startMs == 45000);
        CHECK(read[2].startMs == 90000);
        CHECK(read[3].startMs == 135000);
        CHECK(read[0].title == "Chapter One");
        CHECK(read[3].title == "Chapter Four");
    }
}
