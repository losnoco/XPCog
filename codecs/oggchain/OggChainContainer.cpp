// Chained Ogg files, expanded into one track per link.
//
// A chained Ogg file is several complete logical bitstreams concatenated, each
// with its own headers, tags and sample count. That is a container in exactly
// the sense the registry already means, so `album.oga` becomes `album.oga#0`,
// `#1`, ... and every track gets its own name, length and seek bar instead of
// one nameless run of everything.
//
// One container rather than three. Chaining is a property of Ogg, not of what is
// inside it -- the page structure that says where a link begins is the same
// whether the packets are FLAC, Vorbis or Opus -- so counting links needs no
// codec knowledge at all. Only the decision to offer them does: a chain of
// something this build cannot decode is better left as the single unplayable
// entry it already is than torn into several.
//
// Registered separately from any decoder because it belongs to none of them, and
// because a build with FLAC switched off should still expand a chained Vorbis
// file.

#include "../common/OggChain.hpp"

#include "xpcog/core/PluginRegistry.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace xpcog {
namespace {

/// Codecs whose decoders here understand a link fragment. A chain of anything
/// else -- Speex, Theora, something unrecognised -- is left alone: expanding it
/// would replace one entry that fails to open with several.
[[nodiscard]] bool canDecodeLinks(codecs::OggCodec codec) {
    switch (codec) {
        case codecs::OggCodec::Flac:
        case codecs::OggCodec::Vorbis:
        case codecs::OggCodec::Opus:
            return true;
        default:
            return false;
    }
}

std::vector<Url> expandOggChain(const Url& url, ISource& source,
                                const PluginRegistry& /*registry*/) {
    // Already one track of the file.
    if (!url.fragment().empty()) {
        return {url};
    }

    // A stream has no end to look at and no fixed set of links to list: its
    // chain is handled as it arrives, by decoders that carry on across a
    // boundary and report the new link's tags when they reach it.
    if (!source.seekable()) {
        return {url};
    }

    if (!canDecodeLinks(codecs::oggCodecAt(source, 0))) {
        return {url};
    }
    if (!codecs::looksChained(source)) {
        return {url};
    }

    const std::vector<codecs::OggLink> links = codecs::readOggLinks(source);
    if (links.size() <= 1) {
        return {url};
    }

    std::vector<Url> tracks;
    tracks.reserve(links.size());
    for (std::size_t i = 0; i < links.size(); ++i) {
        tracks.push_back(url.withFragment(std::to_string(i)));
    }
    return tracks;
}

/// Not "flac": a native FLAC file is never a chain, and claiming the extension
/// would take it from the container that expands an embedded cue sheet. An Ogg
/// FLAC file is named .oga or .ogg.
constexpr std::string_view kExtensions[] = {"ogg", "oga", "opus"};

}  // namespace
}  // namespace xpcog

void xpcog_register_oggchain(xpcog::PluginRegistry& r) {
    r.addContainer({
        .name       = "OggChainContainer",
        // Above FFmpeg's chapter container, which claims .ogg too. A container
        // that declines hands the extension on, so both still get their turn.
        .priority   = 2.0F,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = {},
        .expand     = &xpcog::expandOggChain,
    });
}
