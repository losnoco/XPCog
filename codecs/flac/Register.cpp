// FLAC codec registration.
//
// Extensions, MIME types and priority mirror Cog Plugins/Flac/FlacDecoder.m
// (+fileTypes / +mimeTypes / +priority:2.0), plus the Ogg extensions Cog reaches
// through its MIME types alone -- an Ogg FLAC file named .oga otherwise finds no
// decoder at all here, since the only claimant on that extension is Vorbis.

#include "FlacDecoder.hpp"

#include "xpcog/core/PluginRegistry.hpp"

#include <memory>
#include <string_view>

namespace xpcog {
namespace {

constexpr std::string_view kExtensions[] = {"flac", "oga", "ogg"};
// Cog also claims application/ogg and audio/ogg here; the decoder detects an Ogg
// container itself and falls back through MultiDecoder when it is not FLAC.
constexpr std::string_view kMimeTypes[] = {"audio/x-flac", "audio/flac",
                                           "application/ogg", "audio/ogg"};

}  // namespace
}  // namespace xpcog

void xpcog_register_flac(xpcog::PluginRegistry& r) {
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
