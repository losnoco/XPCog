// FLAC codec registration.
//
// Extensions, MIME types and priority mirror Cog Plugins/Flac/FlacDecoder.m
// (+fileTypes / +mimeTypes / +priority:2.0), plus the Ogg extensions Cog reaches
// through its MIME types alone -- an Ogg FLAC file named .oga otherwise finds no
// decoder at all here, since the only claimant on that extension is Vorbis.

#include "FlacDecoder.hpp"
#include "OggChain.hpp"

#include "xpcog/core/PluginRegistry.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog {
namespace {

/// One track per chain link. A chained Ogg file is several complete streams
/// concatenated, each with its own headers and sample count, so it is a
/// container in the sense the registry already means -- and expanding it is what
/// gives every track its own name, length and seek bar instead of one nameless
/// run of everything.
std::vector<Url> expandOggChain(const Url& url, ISource& source,
                                const PluginRegistry& /*registry*/) {
    // Already one track of the file.
    if (!url.fragment().empty()) {
        return {url};
    }
    // A stream has no end to look at, and its links are handled as they arrive
    // by the decoder's chained-stream path rather than listed up front.
    if (!source.seekable()) {
        return {url};
    }
    // .ogg and .oga are worn by Vorbis and Opus too, and the chain walk below
    // says nothing about which is inside.
    if (!codecs::isOggFlacStream(source)) {
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

constexpr std::string_view kExtensions[] = {"flac", "oga", "ogg"};
// Cog also claims application/ogg and audio/ogg here; the decoder detects an Ogg
// container itself and falls back through MultiDecoder when it is not FLAC.
constexpr std::string_view kMimeTypes[] = {"audio/x-flac", "audio/flac",
                                           "application/ogg", "audio/ogg"};

/// Not "flac": a native FLAC file is never a chain, and claiming the extension
/// would take it from the container that expands an embedded cue sheet.
constexpr std::string_view kChainExtensions[] = {"oga", "ogg"};

}  // namespace
}  // namespace xpcog

void xpcog_register_flac(xpcog::PluginRegistry& r) {
    r.addContainer({
        .name       = "OggChainContainer",
        .priority   = 2.0F,
        .extensions = xpcog::kChainExtensions,
        .mimeTypes  = {},
        .expand     = &xpcog::expandOggChain,
    });

    r.addDecoder({
        .name       = "FlacDecoder",
        .priority   = 2.0F,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = xpcog::kMimeTypes,
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::FlacDecoder>();
        },
        .available = nullptr,
    });
}
