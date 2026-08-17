// FLAC codec registration.
//
// Extensions, MIME types and priority mirror Cog Plugins/Flac/FlacDecoder.m
// (+fileTypes / +mimeTypes / +priority:2.0).

#include "FlacDecoder.hpp"

#include "xpcog/core/PluginRegistry.hpp"

#include <memory>
#include <string_view>

namespace {

constexpr std::string_view kExtensions[] = {"flac"};
// Cog also claims application/ogg and audio/ogg here; the decoder detects an Ogg
// container itself and falls back through MultiDecoder when it is not FLAC.
constexpr std::string_view kMimeTypes[] = {"audio/x-flac", "audio/flac",
                                           "application/ogg", "audio/ogg"};

}  // namespace

void xpcog_register_flac(xpcog::PluginRegistry& r) {
    r.addDecoder({
        .name       = "FlacDecoder",
        .priority   = 2.0F,
        .extensions = kExtensions,
        .mimeTypes  = kMimeTypes,
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::FlacDecoder>();
        },
        .available = nullptr,
    });
}
