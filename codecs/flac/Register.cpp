// FLAC codec registration.
//
// Extensions, MIME types and priority mirror Cog Plugins/Flac/FlacDecoder.m
// (+fileTypes / +mimeTypes / +priority:2.0), plus the Ogg extensions Cog reaches
// through its MIME types alone -- an Ogg FLAC file named .oga otherwise finds no
// decoder at all here, since the only claimant on that extension is Vorbis.

#include "FlacDecoder.hpp"

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog {
namespace {

constexpr std::string_view kExtensions[] = {"flac", "oga", "ogg"};

/// Cue sheets are expanded for native FLAC only, not for the Ogg extensions.
///
/// A container claiming an extension opens every file with it just to find out
/// whether it has anything to say, and .ogg is overwhelmingly Vorbis, which
/// would fail that open every time. An Ogg FLAC carrying an embedded cue sheet
/// is rare enough not to be worth charging every Ogg file for. The chained-Ogg
/// container already claims those extensions in any case.
constexpr std::string_view kCueExtensions[] = {"flac"};
// Cog also claims application/ogg and audio/ogg here; the decoder detects an Ogg
// container itself and falls back through MultiDecoder when it is not FLAC.
constexpr std::string_view kMimeTypes[] = {"audio/x-flac", "audio/flac",
                                           "application/ogg", "audio/ogg"};

/// One entry per cue sheet track, or the file itself when it holds only one.
///
/// A FLAC carrying a cue sheet is a whole album in a single file. Without this
/// it was added as one entry: the whole album's length, the file's own tags, and
/// playback beginning at the album's first sample whichever track you picked.
std::vector<Url> expandFlacCue(const Url& url, ISource& source,
                               const PluginRegistry& /*registry*/) {
    // Already a track. Expanding again would offer the album back once per
    // track, for ever.
    if (!url.fragment().empty()) {
        return {url};
    }

    FlacDecoder decoder;
    if (!decoder.open(&source)) {
        return {url};
    }

    const std::vector<std::string> tracks = decoder.cueTracks();
    if (tracks.size() < 2) {
        return {url};
    }

    std::vector<Url> entries;
    entries.reserve(tracks.size());
    for (const std::string& track : tracks) {
        entries.push_back(url.withFragment(track));
    }
    return entries;
}

}  // namespace
}  // namespace xpcog

void xpcog_register_flac(xpcog::PluginRegistry& r) {
    r.addContainer({
        .name       = "FlacCueContainer",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kCueExtensions,
        .mimeTypes  = {},
        .expand     = &xpcog::expandFlacCue,
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
