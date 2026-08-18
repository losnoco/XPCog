#include "UnpackUrl.hpp"

#include <charconv>

namespace xpcog::codecs {
namespace {

/// The extractor token. See the header for why it is Cog's rather than ours.
constexpr std::string_view kExtractor = "fex";

}  // namespace

std::optional<UnpackTarget> parseUnpackUrl(const Url& url) {
    if (url.scheme() != "unpack") {
        return std::nullopt;
    }

    // Without the fragment, and decoded.
    //
    // The fragment is the subsong within the member -- track 3 of an archived
    // NSF -- so leaving it on makes the member "game.nsf#3", a name no archive
    // holds. Decoded because the length below counts the characters Cog counted
    // when it wrote the URL, whatever the storage encoding did to them since.
    std::string body = percentDecode(url.withoutFragment().toString());

    constexpr std::string_view kPrefix = "unpack://";
    if (!body.starts_with(kPrefix)) {
        return std::nullopt;
    }
    body.erase(0, kPrefix.size());

    // <extractor>|<length>|<archive>|<member>
    const std::size_t afterExtractor = body.find('|');
    if (afterExtractor == std::string::npos) {
        return std::nullopt;
    }
    if (std::string_view{body}.substr(0, afterExtractor) != kExtractor) {
        // Some other extractor's addressing. Refusing beats guessing at a
        // layout that only happens to start the same way.
        return std::nullopt;
    }

    const std::size_t afterLength = body.find('|', afterExtractor + 1);
    if (afterLength == std::string::npos) {
        return std::nullopt;
    }

    const std::string_view digits{body.data() + afterExtractor + 1,
                                  afterLength - afterExtractor - 1};
    std::size_t            length = 0;
    const auto [ptr, ec] =
        std::from_chars(digits.data(), digits.data() + digits.size(), length);
    if (ec != std::errc{} || ptr != digits.data() + digits.size()) {
        return std::nullopt;
    }

    // The archive path, then a separator, then at least one character of member.
    const std::size_t archiveAt = afterLength + 1;
    if (archiveAt + length + 1 >= body.size()) {
        return std::nullopt;
    }
    if (body[archiveAt + length] != '|') {
        // The length does not land on the separator, so it is not describing
        // this string -- a truncated or hand-edited URL.
        return std::nullopt;
    }

    UnpackTarget target;
    target.archive = body.substr(archiveAt, length);
    target.member  = body.substr(archiveAt + length + 1);
    if (target.archive.empty() || target.member.empty()) {
        return std::nullopt;
    }
    return target;
}

Url makeUnpackUrl(const std::filesystem::path& archive, std::string_view member) {
    // Forward slashes throughout: the URL is stored in the library and read back
    // on whatever platform opens it next, and a backslash is a path separator on
    // exactly one of them.
    std::string archivePath = archive.generic_string();

    std::string plain;
    plain += kExtractor;
    plain += '|';
    plain += std::to_string(archivePath.size());
    plain += '|';
    plain += archivePath;
    plain += '|';
    plain += member;

    // Encoded as a whole, after the length is computed from the plain text --
    // which is the order Cog reads it in. percentEncodePath leaves '/' and '.'
    // alone, so Url::extension() still finds the member's extension and picks
    // the right decoder.
    return Url::parse("unpack://" + percentEncodePath(plain)).value_or(Url{});
}

}  // namespace xpcog::codecs
