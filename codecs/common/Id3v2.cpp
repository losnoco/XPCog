#include "Id3v2.hpp"

#include "common/TextEncoding.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog::codecs {
namespace {

constexpr std::size_t kHeaderSize = 10;

[[nodiscard]] std::uint8_t byteAt(std::span<const std::byte> data, std::size_t at) {
    return std::to_integer<std::uint8_t>(data[at]);
}

/// Seven bits per byte, high bit always clear -- which is what stops a size
/// field ever looking like an MPEG sync word.
[[nodiscard]] std::uint32_t syncsafe(std::span<const std::byte> data, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 7) | (byteAt(data, at + i) & 0x7FU);
    }
    return value;
}

[[nodiscard]] std::uint32_t bigEndian32(std::span<const std::byte> data, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8) | byteAt(data, at + i);
    }
    return value;
}

[[nodiscard]] std::uint32_t bigEndian24(std::span<const std::byte> data, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        value = (value << 8) | byteAt(data, at + i);
    }
    return value;
}

[[nodiscard]] bool looksLikeHeader(std::span<const std::byte> data) {
    return data.size() >= kHeaderSize && byteAt(data, 0) == 'I' &&
           byteAt(data, 1) == 'D' && byteAt(data, 2) == '3' &&
           byteAt(data, 3) != 0xFF && byteAt(data, 4) != 0xFF &&
           // A syncsafe size with a high bit set is not a size at all, which is
           // the cheapest way to reject three coincidental bytes of audio.
           (byteAt(data, 6) & 0x80U) == 0 && (byteAt(data, 7) & 0x80U) == 0 &&
           (byteAt(data, 8) & 0x80U) == 0 && (byteAt(data, 9) & 0x80U) == 0;
}

void appendCodepoint(std::string& out, std::uint32_t code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

[[nodiscard]] std::string utf16ToUtf8(std::span<const std::byte> data, bool bigEndianDefault) {
    bool        big  = bigEndianDefault;
    std::size_t at   = 0;

    if (data.size() >= 2) {
        const std::uint16_t mark =
            static_cast<std::uint16_t>((byteAt(data, 0) << 8) | byteAt(data, 1));
        if (mark == 0xFEFF) {
            big = true;
            at  = 2;
        } else if (mark == 0xFFFE) {
            big = false;
            at  = 2;
        }
    }

    std::string out;
    while (at + 1 < data.size()) {
        const std::uint16_t unit =
            big ? static_cast<std::uint16_t>((byteAt(data, at) << 8) | byteAt(data, at + 1))
                : static_cast<std::uint16_t>((byteAt(data, at + 1) << 8) | byteAt(data, at));
        at += 2;

        if (unit >= 0xD800 && unit < 0xDC00 && at + 1 < data.size()) {
            const std::uint16_t low =
                big ? static_cast<std::uint16_t>((byteAt(data, at) << 8) | byteAt(data, at + 1))
                    : static_cast<std::uint16_t>((byteAt(data, at + 1) << 8) | byteAt(data, at));
            if (low >= 0xDC00 && low < 0xE000) {
                at += 2;
                appendCodepoint(out, 0x10000U + ((static_cast<std::uint32_t>(unit - 0xD800) << 10) |
                                                 static_cast<std::uint32_t>(low - 0xDC00)));
                continue;
            }
        }
        appendCodepoint(out, unit);
    }
    return out;
}

/// A frame's text payload, split into the values it holds. ID3v2.4 allows a text
/// frame to carry several, separated by a terminator in the frame's own
/// encoding -- so the split has to happen before decoding, not after.
[[nodiscard]] std::vector<std::string> decodeTextList(std::uint8_t encoding,
                                                      std::span<const std::byte> data) {
    std::vector<std::string> values;

    const auto flush = [&](std::span<const std::byte> unit) {
        switch (encoding) {
            case 0: {
                std::string raw(reinterpret_cast<const char*>(unit.data()), unit.size());
                // Latin-1 by the standard, but taggers write UTF-8 here constantly
                // and Latin-1 decoding of UTF-8 is mojibake rather than an error.
                values.push_back(toUtf8(std::move(raw)));
                break;
            }
            case 1: values.push_back(utf16ToUtf8(unit, false)); break;
            case 2: values.push_back(utf16ToUtf8(unit, true)); break;
            default: {
                std::string raw(reinterpret_cast<const char*>(unit.data()), unit.size());
                values.push_back(toUtf8(std::move(raw)));
                break;
            }
        }
    };

    const std::size_t width = (encoding == 1 || encoding == 2) ? 2U : 1U;
    std::size_t       start = 0;
    for (std::size_t at = 0; at + width <= data.size(); at += width) {
        const bool terminator =
            (width == 1) ? byteAt(data, at) == 0
                         : (byteAt(data, at) == 0 && byteAt(data, at + 1) == 0);
        if (!terminator) {
            continue;
        }
        flush(data.subspan(start, at - start));
        start = at + width;
    }
    if (start < data.size()) {
        flush(data.subspan(start));
    }

    // Empty units are kept: COMM and TXXX put a description before the value,
    // and an empty description is the ordinary case -- dropping it here would
    // shift the value into the description's place and lose it.
    return values;
}

/// FFmpeg's own frame-to-key tables (libavformat/id3v2.c), so a tag parsed here
/// lands under the same names as one the demuxer read.
struct FrameName {
    std::string_view frame;
    std::string_view key;
};

constexpr FrameName kFrameNames[] = {
    // ID3v2.3 and .4
    {"TALB", "album"},   {"TCOM", "composer"},   {"TCON", "genre"},
    {"TCOP", "copyright"}, {"TENC", "encoded_by"}, {"TIT2", "title"},
    {"TLAN", "language"}, {"TPE1", "artist"},     {"TPE2", "album_artist"},
    {"TPE3", "performer"}, {"TPOS", "disc"},      {"TPUB", "publisher"},
    {"TRCK", "track"},   {"TSSE", "encoder"},    {"USLT", "lyrics"},
    {"TCMP", "compilation"}, {"TDRC", "date"},   {"TDRL", "date"},
    {"TDEN", "creation_time"}, {"TSOA", "album-sort"}, {"TSOP", "artist-sort"},
    {"TSOT", "title-sort"}, {"TIT1", "grouping"}, {"TYER", "date"},
    {"TDAT", "date"},    {"COMM", "comment"},
    // ID3v2.2, whose frame identifiers are three characters
    {"TAL", "album"},    {"TCO", "genre"},       {"TCP", "compilation"},
    {"TT2", "title"},    {"TEN", "encoded_by"},  {"TP1", "artist"},
    {"TP2", "album_artist"}, {"TP3", "performer"}, {"TRK", "track"},
    {"TCM", "composer"}, {"TYE", "date"},        {"TPA", "disc"},
    {"COM", "comment"},  {"ULT", "lyrics"},
};

[[nodiscard]] std::string_view keyForFrame(std::string_view frame) {
    for (const FrameName& name : kFrameNames) {
        if (name.frame == frame) {
            return name.key;
        }
    }
    return {};
}

[[nodiscard]] bool isFrameIdentifier(std::string_view frame) {
    return std::all_of(frame.begin(), frame.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    });
}

[[nodiscard]] std::string lowercased(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

/// Undoes the 0xFF 0x00 escaping the unsynchronisation flag introduces, which
/// exists so a tag can never contain something a decoder mistakes for a frame
/// sync. Rare in practice; ignoring the flag corrupts every string after the
/// first escaped byte, so it is cheaper to handle than to detect.
[[nodiscard]] std::vector<std::byte> deUnsynchronise(std::span<const std::byte> data) {
    std::vector<std::byte> out;
    out.reserve(data.size());
    for (std::size_t at = 0; at < data.size(); ++at) {
        out.push_back(data[at]);
        if (byteAt(data, at) == 0xFF && at + 1 < data.size() && byteAt(data, at + 1) == 0) {
            ++at;
        }
    }
    return out;
}

}  // namespace

std::size_t id3v2TagLength(std::span<const std::byte> data) {
    if (!looksLikeHeader(data)) {
        return 0;
    }
    const std::uint8_t  major = byteAt(data, 3);
    const std::uint8_t  flags = byteAt(data, 5);
    const std::uint32_t size  = syncsafe(data, 6);

    // Only ID3v2.4 defines a footer, and it is another header's worth of bytes.
    const std::size_t footer = (major >= 4 && (flags & 0x10U) != 0) ? kHeaderSize : 0;
    return kHeaderSize + size + footer;
}

bool parseId3v2(std::span<const std::byte> data, MetadataMap& out) {
    if (!looksLikeHeader(data)) {
        return false;
    }

    const std::uint8_t  major = byteAt(data, 3);
    const std::uint8_t  flags = byteAt(data, 5);
    const std::uint32_t size  = syncsafe(data, 6);
    if (major < 2 || major > 4 || size == 0) {
        return false;
    }

    // A truncated tag is still worth reading: a station's title is at the front,
    // and refusing the whole thing loses it to a lost packet at the back.
    const std::size_t available =
        std::min<std::size_t>(size, data.size() - kHeaderSize);
    std::span<const std::byte> body = data.subspan(kHeaderSize, available);

    std::vector<std::byte> unsynchronised;
    if ((flags & 0x80U) != 0) {
        unsynchronised = deUnsynchronise(body);
        body           = unsynchronised;
    }

    // ID3v2.2 uses bit 6 for compression, which nothing writes and this does not
    // read. In .3 and .4 it announces an extended header to be skipped.
    if ((flags & 0x40U) != 0) {
        if (major == 2) {
            return false;
        }
        if (body.size() < 4) {
            return false;
        }
        const std::size_t extended =
            (major >= 4) ? syncsafe(body, 0) : bigEndian32(body, 0) + 4;
        if (extended >= body.size()) {
            return false;
        }
        body = body.subspan(extended);
    }

    const std::size_t identifierSize = (major == 2) ? 3 : 4;
    const std::size_t frameHeader    = (major == 2) ? 6 : 10;

    bool        found = false;
    std::size_t at    = 0;
    while (at + frameHeader <= body.size()) {
        const std::string_view identifier{
            reinterpret_cast<const char*>(body.data() + at), identifierSize};
        // Padding: the rest of the tag is zeroes.
        if (identifier[0] == '\0' || !isFrameIdentifier(identifier)) {
            break;
        }

        std::size_t frameSize = 0;
        if (major == 2) {
            frameSize = bigEndian24(body, at + 3);
        } else if (major == 3) {
            frameSize = bigEndian32(body, at + 4);
        } else {
            // ID3v2.4 sizes are syncsafe, but taggers get this wrong often
            // enough that FFmpeg carries a fallback too. A high bit set says the
            // field cannot be syncsafe, so read it as a plain integer.
            const bool plain = (byteAt(body, at + 4) & 0x80U) != 0 ||
                               (byteAt(body, at + 5) & 0x80U) != 0 ||
                               (byteAt(body, at + 6) & 0x80U) != 0 ||
                               (byteAt(body, at + 7) & 0x80U) != 0;
            frameSize = plain ? bigEndian32(body, at + 4) : syncsafe(body, at + 4);
        }

        at += frameHeader;
        if (frameSize == 0 || frameSize > body.size() - at) {
            break;
        }

        const std::span<const std::byte> payload = body.subspan(at, frameSize);
        at += frameSize;

        const bool isText    = identifier[0] == 'T';
        const bool isComment = identifier == "COMM" || identifier == "COM";
        const bool isLyrics  = identifier == "USLT" || identifier == "ULT";

        // Everything else is dropped, PRIV most deliberately: every HLS
        // transport-stream segment carries
        // com.apple.streaming.transportStreamTimestamp in one, so a reader that
        // kept it would see the tags change on every segment forever and rename
        // the track each time. APIC goes too -- cover art belongs to the file,
        // and FFmpeg already surfaces an embedded one as its own stream.
        if (!isText && !isComment && !isLyrics) {
            continue;
        }

        if (payload.empty()) {
            continue;
        }
        const std::uint8_t encoding = byteAt(payload, 0);

        std::span<const std::byte> text             = payload.subspan(1);
        bool                       descriptionIsKey = false;
        // COMM and USLT lead with a three-character language code that is not
        // part of any value.
        if (isComment || isLyrics) {
            if (text.size() < 3) {
                continue;
            }
            text = text.subspan(3);
        } else if (identifier == "TXXX" || identifier == "TXX") {
            descriptionIsKey = true;
        }

        std::vector<std::string> values = decodeTextList(encoding, text);
        if (values.empty()) {
            continue;
        }

        std::string key;
        if (descriptionIsKey) {
            // TXXX names its own key: the description comes first, the value
            // after it.
            if (values.size() < 2) {
                continue;
            }
            key = lowercased(values.front());
            values.erase(values.begin());
        } else if (isComment) {
            if (values.size() < 2) {
                continue;
            }
            const std::string description = std::move(values.front());
            values.erase(values.begin());
            // A named comment is not a comment: iTunes stores its gapless
            // information in a COMM frame described as "iTunSMPB", and filing
            // that under "comment" both loses the name a reader needs and puts
            // a line of hex where a listener expects prose. FFmpeg keys these
            // by description too; only an unnamed one is the comment.
            key = description.empty() ? std::string{"comment"} : lowercased(description);
        } else if (isLyrics) {
            if (values.size() < 2) {
                continue;
            }
            values.erase(values.begin());  // the description, not the words
            key = "lyrics";
        } else if (const std::string_view mapped = keyForFrame(identifier); !mapped.empty()) {
            key = std::string{mapped};
        } else if (isText) {
            // An unrecognised text frame still carries something; FFmpeg keeps
            // these under the lowercased frame name and so does this.
            key = lowercased(identifier);
        } else {
            continue;
        }

        std::erase_if(values, [](const std::string& value) { return value.empty(); });
        if (key.empty() || values.empty()) {
            continue;
        }
        out.set(key, std::move(values));
        found = true;
    }

    return found;
}

namespace {

struct RawToc {
    bool                     topLevel = false;
    bool                     ordered  = false;
    std::vector<std::string> children;
};

/// CHAP and CTOC are four-character frames, so they exist only in .3 and .4;
/// the size rules are the narrow parser's, including the plain-integer
/// fallback for taggers that write .4 sizes wrong.
[[nodiscard]] std::size_t chapterFrameSize(std::span<const std::byte> body, std::size_t at,
                                           std::uint8_t major) {
    if (major == 3) {
        return bigEndian32(body, at + 4);
    }
    const bool plain = (byteAt(body, at + 4) & 0x80U) != 0 ||
                       (byteAt(body, at + 5) & 0x80U) != 0 ||
                       (byteAt(body, at + 6) & 0x80U) != 0 ||
                       (byteAt(body, at + 7) & 0x80U) != 0;
    return plain ? bigEndian32(body, at + 4) : syncsafe(body, at + 4);
}

/// The null-terminated element ID at the front of a CHAP or CTOC payload.
/// Returns the offset past the terminator, or npos when there is none.
[[nodiscard]] std::size_t elementIdEnd(std::span<const std::byte> payload, std::string& out) {
    std::size_t at = 0;
    while (at < payload.size() && byteAt(payload, at) != 0) {
        ++at;
    }
    if (at >= payload.size()) {
        return std::string_view::npos;
    }
    out.assign(reinterpret_cast<const char*>(payload.data()), at);
    return at + 1;
}

[[nodiscard]] bool parseChapFrame(std::span<const std::byte> payload, std::uint8_t major,
                                  Chapter& out) {
    // The element ID is kept opaque in Chapter::id: players key chapter
    // identity on it, and the writer keeps it across renames.
    const std::size_t afterId = elementIdEnd(payload, out.id);
    if (afterId == std::string_view::npos || afterId + 16 > payload.size()) {
        return false;
    }
    // Start and end in milliseconds; the two byte offsets after them are
    // ignored, as TagLib's reader ignores them -- the times are what count.
    out.startMs = bigEndian32(payload, afterId);
    out.endMs   = bigEndian32(payload, afterId + 4);

    // Embedded frames: a chapter's title is a TIT2 inside its CHAP. Walk them
    // with the same size rules; anything else is left alone.
    std::size_t at = afterId + 16;
    while (at + 10 <= payload.size()) {
        const std::string_view identifier{reinterpret_cast<const char*>(payload.data() + at), 4};
        if (identifier[0] == '\0' || !isFrameIdentifier(identifier)) {
            break;
        }
        const std::size_t frameSize = chapterFrameSize(payload, at, major);
        at += 10;
        if (frameSize == 0 || frameSize > payload.size() - at) {
            break;
        }
        const std::span<const std::byte> embedded = payload.subspan(at, frameSize);
        at += frameSize;
        if (identifier == "TIT2" && !embedded.empty() && out.title.empty()) {
            const std::vector<std::string> values =
                decodeTextList(byteAt(embedded, 0), embedded.subspan(1));
            if (!values.empty()) {
                out.title = values.front();
            }
        }
    }
    return true;
}

[[nodiscard]] bool parseCtocFrame(std::span<const std::byte> payload, RawToc& out) {
    std::string       id;
    const std::size_t afterId = elementIdEnd(payload, id);
    if (afterId == std::string_view::npos || afterId + 2 > payload.size()) {
        return false;
    }
    const std::uint8_t flags = byteAt(payload, afterId);
    out.topLevel             = (flags & 0x02U) != 0;
    out.ordered              = (flags & 0x01U) != 0;
    const std::uint8_t count = byteAt(payload, afterId + 1);
    std::size_t        at    = afterId + 2;
    for (std::uint8_t i = 0; i < count; ++i) {
        std::string       child;
        const std::size_t next = elementIdEnd(payload.subspan(at), child);
        if (next == std::string_view::npos) {
            break;  // truncated table: keep what was read
        }
        out.children.push_back(std::move(child));
        at += next;
    }
    return true;
}

}  // namespace

bool parseId3v2Chapters(std::span<const std::byte> data, ChapterList& out) {
    out.clear();
    if (!looksLikeHeader(data)) {
        return false;
    }

    const std::uint8_t  major = byteAt(data, 3);
    const std::uint8_t  flags = byteAt(data, 5);
    const std::uint32_t size  = syncsafe(data, 6);
    if (major < 2 || major > 4 || size == 0) {
        return false;
    }

    const std::size_t available =
        std::min<std::size_t>(size, data.size() - kHeaderSize);
    std::span<const std::byte> body = data.subspan(kHeaderSize, available);

    std::vector<std::byte> unsynchronised;
    if ((flags & 0x80U) != 0) {
        unsynchronised = deUnsynchronise(body);
        body           = unsynchronised;
    }
    if ((flags & 0x40U) != 0) {
        if (major == 2) {
            return false;
        }
        if (body.size() < 4) {
            return false;
        }
        const std::size_t extended =
            (major >= 4) ? syncsafe(body, 0) : bigEndian32(body, 0) + 4;
        if (extended >= body.size()) {
            return false;
        }
        body = body.subspan(extended);
    }

    // A readable .2 tag holds no chapters: CHAP and CTOC are .3 frames onward.
    if (major == 2) {
        return true;
    }

    std::vector<Chapter> chapters;
    RawToc               toc;
    bool                 haveToc = false;
    std::size_t             at      = 0;
    while (at + 10 <= body.size()) {
        const std::string_view identifier{reinterpret_cast<const char*>(body.data() + at), 4};
        if (identifier[0] == '\0' || !isFrameIdentifier(identifier)) {
            break;
        }
        const std::size_t frameSize = chapterFrameSize(body, at, major);
        at += 10;
        if (frameSize == 0 || frameSize > body.size() - at) {
            break;
        }
        const std::span<const std::byte> payload = body.subspan(at, frameSize);
        at += frameSize;

        if (identifier == "CHAP") {
            Chapter raw;
            if (!parseChapFrame(payload, major, raw)) {
                continue;
            }
            // Duplicate element IDs: the first frame wins, which is what
            // TagLib's findByElementID answers.
            const bool duplicate =
                std::any_of(chapters.begin(), chapters.end(),
                            [&raw](const Chapter& held) { return held.id == raw.id; });
            if (!duplicate) {
                chapters.push_back(std::move(raw));
            }
        } else if (identifier == "CTOC" && !haveToc) {
            RawToc raw;
            if (parseCtocFrame(payload, raw) && raw.topLevel) {
                toc     = std::move(raw);
                haveToc = true;
            }
        }
    }

    // The top-level ordered table's order when it gives one -- the order a
    // player lists -- otherwise every chapter by start. TagLib's reader
    // resolves it the same way, so the two agree by construction.
    if (haveToc && toc.ordered) {
        for (const std::string& child : toc.children) {
            const auto found = std::find_if(chapters.begin(), chapters.end(),
                                            [&child](const Chapter& raw) {
                                                return raw.id == child;
                                            });
            if (found != chapters.end()) {
                out.push_back(*found);
            }
        }
    }
    if (out.empty()) {
        for (Chapter& raw : chapters) {
            out.push_back(std::move(raw));
        }
        std::stable_sort(out.begin(), out.end(),
                         [](const Chapter& a, const Chapter& b) {
                             return a.startMs < b.startMs;
                         });
    }
    return true;
}

}  // namespace xpcog::codecs
