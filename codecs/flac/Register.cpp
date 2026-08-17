// FLAC codec registration.
//
// Extensions, MIME types and priority mirror Cog Plugins/Flac/FlacDecoder.m
// (+fileTypes / +mimeTypes / +priority). The decoder itself lands in M1a; the
// descriptor is registered now so the generated-registration path is exercised
// end to end from M0.

#include "xpcog/core/PluginRegistry.hpp"

namespace {

constexpr std::string_view kExtensions[] = {"flac"};
constexpr std::string_view kMimeTypes[]  = {"audio/x-flac", "audio/flac"};

}  // namespace

void xpcog_register_flac(xpcog::PluginRegistry& r) {
    r.addDecoder({
        .name       = "FlacDecoder",
        .priority   = 2.0F,  // Cog gives FLAC an above-default priority
        .extensions = kExtensions,
        .mimeTypes  = kMimeTypes,
        .create     = []() -> xpcog::DecoderPtr { return nullptr; },  // M1a
        .available  = nullptr,
    });
}
