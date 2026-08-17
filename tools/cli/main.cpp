// xpcog-cli -- the headless face of xpcog-core.
//
// This is the project's real test harness: it links no Qt, so it runs anywhere and
// exercises the engine without a display.

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Version.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

int usage() {
    std::puts("usage: xpcog-cli <command> [args]\n"
              "\n"
              "  --version           print version and exit\n"
              "  codecs              list compiled-in codecs and claimed extensions\n"
              "  info <file>         print format and tags\n"
              "  decode <in> <out>   decode to headerless native-endian PCM\n"
              "\n"
              "  play <file>         (M1a, pending audio output)");
    return 2;
}

xpcog::PluginRegistry& registry() {
    // Constructed in place: PluginRegistry is deliberately non-copyable, since the
    // decoders it hands out reference descriptors stored inside it.
    static xpcog::PluginRegistry instance;
    static const bool            once = [] {
        xpcog::registerAllCodecs(instance);
        return true;
    }();
    (void)once;
    return instance;
}

/// Accepts either a URL or a plain filesystem path, so the CLI is pleasant to use.
xpcog::Url urlFromArgument(std::string_view argument) {
    if (auto parsed = xpcog::Url::parse(argument)) {
        return *parsed;
    }
    return xpcog::Url::fromLocalPath(std::filesystem::path{argument});
}

const char* sampleFormatName(xpcog::SampleFormat format) {
    switch (format) {
        case xpcog::SampleFormat::U8:  return "u8";
        case xpcog::SampleFormat::S8:  return "s8";
        case xpcog::SampleFormat::S16: return "s16";
        case xpcog::SampleFormat::S24: return "s24";
        case xpcog::SampleFormat::S32: return "s32";
        case xpcog::SampleFormat::F32: return "f32";
        case xpcog::SampleFormat::F64: return "f64";
        case xpcog::SampleFormat::DSD: return "dsd";
    }
    return "?";
}

int listCodecs() {
    const auto& r = registry();
    std::printf("%zu decoder(s), %zu source(s)\n", r.decoderCount(), r.sourceCount());
    std::fputs("extensions:", stdout);
    for (const auto& ext : r.allExtensions()) {
        std::printf(" %s", ext.c_str());
    }
    std::putchar('\n');
    return 0;
}

int info(std::string_view path) {
    auto opened = registry().open(urlFromArgument(path));
    if (!opened) {
        std::fprintf(stderr, "xpcog-cli: cannot open '%.*s'\n",
                     static_cast<int>(path.size()), path.data());
        return 1;
    }

    const auto props = opened.decoder->properties();
    const auto& fmt  = props.format;

    std::printf("codec:        %s (%s)\n", props.codec.c_str(), props.encoding.c_str());
    std::printf("sample rate:  %.0f Hz\n", fmt.sampleRate);
    std::printf("channels:     %u (config 0x%05X)\n", fmt.channels, fmt.channelConfig);
    std::printf("sample fmt:   %s, %u bits\n", sampleFormatName(fmt.format),
                fmt.bitsPerSample);
    std::printf("frames:       %lld\n", static_cast<long long>(props.totalFrames));
    std::printf("duration:     %.3f s\n", props.duration());
    std::printf("bitrate:      %d kbps\n", props.bitrateKbps);
    std::printf("seekable:     %s\n", props.seekable ? "yes" : "no");

    if (const auto& rg = props.replayGain; !rg.empty()) {
        std::fputs("replaygain:  ", stdout);
        if (rg.trackGain) std::printf(" track %+.2f dB", *rg.trackGain);
        if (rg.albumGain) std::printf(" album %+.2f dB", *rg.albumGain);
        std::putchar('\n');
    }

    const auto tags = opened.decoder->metadata();
    if (!tags.empty()) {
        std::puts("tags:");
        for (const auto& entry : tags) {
            if (const auto* strings =
                    std::get_if<std::vector<std::string>>(&entry.value)) {
                for (const auto& value : *strings) {
                    std::printf("  %-20s %s\n", entry.key.c_str(), value.c_str());
                }
            } else if (const auto* bytes =
                           std::get_if<std::vector<std::byte>>(&entry.value)) {
                std::printf("  %-20s <%zu bytes>\n", entry.key.c_str(), bytes->size());
            }
        }
    }

    return 0;
}

int decode(std::string_view input, std::string_view output) {
    auto opened = registry().open(urlFromArgument(input));
    if (!opened) {
        std::fprintf(stderr, "xpcog-cli: cannot open '%.*s'\n",
                     static_cast<int>(input.size()), input.data());
        return 1;
    }

    const std::string outPath{output};
    std::FILE*        out = std::fopen(outPath.c_str(), "wb");
    if (out == nullptr) {
        std::fprintf(stderr, "xpcog-cli: cannot write '%s'\n", outPath.c_str());
        return 1;
    }

    xpcog::AudioChunk chunk;
    std::int64_t      frames = 0;

    while (opened.decoder->readAudio(chunk)) {
        const auto bytes = chunk.bytes();
        if (!bytes.empty() &&
            std::fwrite(bytes.data(), 1, bytes.size(), out) != bytes.size()) {
            std::fprintf(stderr, "xpcog-cli: short write to '%s'\n", outPath.c_str());
            std::fclose(out);
            return 1;
        }
        frames += static_cast<std::int64_t>(chunk.frameCount());
    }

    std::fclose(out);

    const auto& fmt = opened.decoder->properties().format;
    std::fprintf(stderr, "decoded %lld frames, %s %u ch @ %.0f Hz\n",
                 static_cast<long long>(frames), sampleFormatName(fmt.format),
                 fmt.channels, fmt.sampleRate);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return usage();
    }

    const std::string_view command{argv[1]};

    if (command == "--version" || command == "-v" || command == "version") {
        const auto banner = xpcog::versionBanner();
        std::printf("%.*s\n", static_cast<int>(banner.size()), banner.data());
        return 0;
    }

    if (command == "codecs") {
        return listCodecs();
    }

    if (command == "info") {
        return (argc < 3) ? usage() : info(argv[2]);
    }

    if (command == "decode") {
        return (argc < 4) ? usage() : decode(argv[2], argv[3]);
    }

    std::fprintf(stderr, "xpcog-cli: unknown command '%.*s'\n\n",
                 static_cast<int>(command.size()), command.data());
    return usage();
}
