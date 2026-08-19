#include "OggChain.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

namespace xpcog::codecs {
namespace {

constexpr std::size_t kPageHeaderSize = 27;
constexpr std::uint8_t kFlagBeginning = 0x02;
constexpr std::uint8_t kFlagEnd       = 0x04;

/// A file of a million links is a file that is lying about something. The walk
/// is bounded so a malformed page table cannot spin.
constexpr std::size_t kMaxLinks = 4096;

/// How far back from the end to look for the final page. A page holds at most
/// 255 segments of 255 bytes plus its header, so one always fits.
constexpr std::int64_t kTailWindow = 128 * 1024;

[[nodiscard]] bool readAt(ISource& source, std::int64_t offset, void* out,
                          std::int64_t bytes) {
    if (!source.seek(offset, SEEK_SET)) {
        return false;
    }
    auto*        destination = static_cast<std::byte*>(out);
    std::int64_t done        = 0;
    while (done < bytes) {
        const std::int64_t got = source.read(destination + done, bytes - done);
        if (got <= 0) {
            return false;
        }
        done += got;
    }
    return true;
}

[[nodiscard]] std::uint32_t littleEndian32(const std::uint8_t* at) {
    return static_cast<std::uint32_t>(at[0]) |
           (static_cast<std::uint32_t>(at[1]) << 8) |
           (static_cast<std::uint32_t>(at[2]) << 16) |
           (static_cast<std::uint32_t>(at[3]) << 24);
}

[[nodiscard]] bool isPageHeader(const std::uint8_t* at) {
    // Version 0 as well as the capture pattern: "OggS" occurs inside packet data
    // often enough that the magic alone is not evidence of a page.
    return std::memcmp(at, "OggS", 4) == 0 && at[4] == 0;
}

[[nodiscard]] std::int64_t sourceSize(ISource& source) {
    if (!source.seek(0, SEEK_END)) {
        return -1;
    }
    return source.tell();
}

}  // namespace

OggCodec oggCodecAt(ISource& source, std::int64_t offset) {
    std::array<std::uint8_t, kPageHeaderSize> header{};
    if (!readAt(source, offset, header.data(),
                static_cast<std::int64_t>(header.size())) ||
        !isPageHeader(header.data())) {
        return OggCodec::Unknown;
    }

    const std::size_t segments = header[26];
    if (segments == 0) {
        return OggCodec::Unknown;
    }

    // Every Ogg mapping names itself in the first bytes of its first packet.
    std::array<std::uint8_t, 8> packet{};
    const std::int64_t          payload = offset +
                                 static_cast<std::int64_t>(kPageHeaderSize) +
                                 static_cast<std::int64_t>(segments);
    if (!readAt(source, payload, packet.data(),
                static_cast<std::int64_t>(packet.size()))) {
        return OggCodec::Unknown;
    }

    if (packet[0] == 0x7F && std::memcmp(packet.data() + 1, "FLAC", 4) == 0) {
        return OggCodec::Flac;
    }
    if (packet[0] == 0x01 && std::memcmp(packet.data() + 1, "vorbis", 6) == 0) {
        return OggCodec::Vorbis;
    }
    if (std::memcmp(packet.data(), "OpusHead", 8) == 0) {
        return OggCodec::Opus;
    }
    if (std::memcmp(packet.data(), "Speex   ", 8) == 0) {
        return OggCodec::Speex;
    }
    return OggCodec::Unknown;
}

std::size_t oggLinkFromFragment(const Url& url) {
    const std::string_view fragment = url.fragment();
    std::size_t            value    = 0;
    for (const char c : fragment) {
        if (c < '0' || c > '9') {
            return 0;
        }
        value = value * 10 + static_cast<std::size_t>(c - '0');
    }
    return value;
}

bool looksChained(ISource& source) {
    std::array<std::uint8_t, kPageHeaderSize> first{};
    if (!readAt(source, 0, first.data(), static_cast<std::int64_t>(first.size())) ||
        !isPageHeader(first.data())) {
        return false;
    }
    const std::uint32_t firstSerial = littleEndian32(first.data() + 14);

    const std::int64_t size = sourceSize(source);
    if (size <= static_cast<std::int64_t>(kPageHeaderSize)) {
        return false;
    }

    const std::int64_t window = std::min(size, kTailWindow);
    std::vector<std::uint8_t> tail(static_cast<std::size_t>(window));
    if (!readAt(source, size - window, tail.data(), window)) {
        return false;
    }

    // The last page of the last link, found by scanning backwards for a header
    // that also carries the end-of-stream flag. Requiring that flag is what keeps
    // stray "OggS" bytes inside audio data from answering.
    for (std::size_t at = tail.size() - kPageHeaderSize + 1; at-- > 0;) {
        if (!isPageHeader(tail.data() + at)) {
            continue;
        }
        if ((tail[at + 5] & kFlagEnd) == 0) {
            continue;
        }
        return littleEndian32(tail.data() + at + 14) != firstSerial;
    }
    return false;
}

std::vector<OggLink> readOggLinks(ISource& source) {
    std::vector<OggLink> links;

    const std::int64_t size = sourceSize(source);
    if (size <= 0) {
        return links;
    }

    std::int64_t offset = 0;
    while (offset + static_cast<std::int64_t>(kPageHeaderSize) <= size) {
        std::array<std::uint8_t, kPageHeaderSize> header{};
        if (!readAt(source, offset, header.data(),
                    static_cast<std::int64_t>(header.size())) ||
            !isPageHeader(header.data())) {
            break;
        }

        const std::size_t segments = header[26];
        std::vector<std::uint8_t> table(segments);
        if (segments > 0 &&
            !readAt(source, offset + static_cast<std::int64_t>(kPageHeaderSize),
                    table.data(), static_cast<std::int64_t>(segments))) {
            break;
        }

        std::int64_t payload = 0;
        for (const std::uint8_t length : table) {
            payload += length;
        }
        const std::int64_t pageSize = static_cast<std::int64_t>(kPageHeaderSize) +
                                      static_cast<std::int64_t>(segments) + payload;

        if ((header[5] & kFlagBeginning) != 0) {
            if (links.size() >= kMaxLinks) {
                break;
            }
            links.push_back(OggLink{offset, offset, littleEndian32(header.data() + 14)});
        }
        if (links.empty()) {
            break;  // audio before any beginning-of-stream page; not a walkable file
        }

        offset += pageSize;
        links.back().end = offset;
    }

    return links;
}

}  // namespace xpcog::codecs
