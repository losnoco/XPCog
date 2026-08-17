#include "VorbisComments.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace xpcog::codecs {
namespace {

[[nodiscard]] std::string lowercased(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return out;
}

[[nodiscard]] std::optional<float> parseFloat(std::string_view text) {
    try {
        return std::stof(std::string{text});
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] int base64Value(char c) noexcept {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

[[nodiscard]] std::vector<std::byte> base64Decode(std::string_view text) {
    std::vector<std::byte> out;
    out.reserve(text.size() * 3 / 4);

    std::uint32_t accumulator = 0;
    int           bits        = 0;
    for (const char c : text) {
        const int value = base64Value(c);
        if (value < 0) {
            continue;  // padding and whitespace
        }
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::byte>((accumulator >> bits) & 0xFF));
        }
    }
    return out;
}

[[nodiscard]] std::uint32_t readBigEndian32(const std::byte* p) noexcept {
    return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0])) << 24) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2])) << 8) |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[3]));
}

}  // namespace

bool applyVorbisComment(std::string_view comment, MetadataMap& tags,
                        ReplayGainInfo& gain, std::uint32_t* channelConfig) {
    const std::size_t equals = comment.find('=');
    if (equals == std::string_view::npos) {
        return false;
    }

    const std::string      name  = lowercased(comment.substr(0, equals));
    const std::string_view value = comment.substr(equals + 1);

    if (name == "replaygain_track_gain") {
        gain.trackGain = parseFloat(value);
    } else if (name == "replaygain_track_peak") {
        gain.trackPeak = parseFloat(value);
    } else if (name == "replaygain_album_gain") {
        gain.albumGain = parseFloat(value);
    } else if (name == "replaygain_album_peak") {
        gain.albumPeak = parseFloat(value);
    } else if (name == "waveformatextensible_channel_mask") {
        if (channelConfig != nullptr &&
            (value.starts_with("0x") || value.starts_with("0X"))) {
            *channelConfig = static_cast<std::uint32_t>(
                std::strtoul(std::string{value}.c_str() + 2, nullptr, 16));
        }
    } else if (name == "metadata_block_picture") {
        auto image = parsePictureBlock(value);
        if (!image.empty()) {
            tags.setBytes("albumart", std::move(image));
        }
    } else if (name == "unsynced lyrics" || name == "lyrics") {
        tags.add("unsyncedlyrics", std::string{value});
    } else if (name == "comments:itunnorm") {
        gain.soundcheck = std::string{value};
        tags.add("soundcheck", std::string{value});
    } else {
        tags.add(name, std::string{value});
    }
    return true;
}

std::vector<std::byte> parsePictureBlock(std::string_view base64) {
    const std::vector<std::byte> block = base64Decode(base64);

    // FLAC picture block layout, all big-endian:
    //   u32 type, u32 mimeLen, mime, u32 descLen, desc,
    //   u32 width, height, depth, colours, u32 dataLen, data
    std::size_t offset = 4;  // skip picture type
    const auto  need   = [&](std::size_t bytes) { return offset + bytes <= block.size(); };

    if (!need(4)) return {};
    const std::uint32_t mimeLength = readBigEndian32(block.data() + offset);
    offset += 4 + mimeLength;

    if (!need(4)) return {};
    const std::uint32_t descriptionLength = readBigEndian32(block.data() + offset);
    offset += 4 + descriptionLength;

    // width, height, depth, colour count
    offset += 16;

    if (!need(4)) return {};
    const std::uint32_t dataLength = readBigEndian32(block.data() + offset);
    offset += 4;

    if (!need(dataLength)) return {};
    return std::vector<std::byte>(block.begin() + static_cast<std::ptrdiff_t>(offset),
                                  block.begin() +
                                      static_cast<std::ptrdiff_t>(offset + dataLength));
}

}  // namespace xpcog::codecs
