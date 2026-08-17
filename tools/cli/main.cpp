// xpcog-cli -- the headless face of xpcog-core.
//
// This is the project's real test harness: it links no Qt, so it runs anywhere and
// exercises the engine without a display.

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Version.hpp"
#include "xpcog/core/audio/IAudioOutput.hpp"
#include "xpcog/core/audio/RingBuffer.hpp"
#include "xpcog/core/audio/SampleConvert.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

int usage() {
    std::puts("usage: xpcog-cli <command> [args]\n"
              "\n"
              "  --version           print version and exit\n"
              "  codecs              list compiled-in codecs and claimed extensions\n"
              "  info <file>         print format and tags\n"
              "  decode <in> <out>   decode to headerless native-endian PCM\n"
              "\n"
              "  play <file>         decode and play to the default audio device");
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

int play(std::string_view path) {
    auto opened = registry().open(urlFromArgument(path));
    if (!opened) {
        std::fprintf(stderr, "xpcog-cli: cannot open '%.*s'\n",
                     static_cast<int>(path.size()), path.data());
        return 1;
    }

    const auto props = opened.decoder->properties();
    const auto& fmt  = props.format;

    // Ring holds ~0.5 s, which is generous for a CLI and leaves plenty of margin
    // for the feeder. The engine will size this from the device period in M1b.
    const std::size_t ringSamples =
        static_cast<std::size_t>(fmt.sampleRate * 0.5) * fmt.channels;
    xpcog::RingBuffer ring(ringSamples);

    auto output = xpcog::makeMiniaudioOutput(ring);

    xpcog::IAudioOutput::Config config;
    config.sampleRate = fmt.sampleRate;
    config.channels   = fmt.channels;

    // Prefill before starting, so the first callbacks are not underruns.
    std::vector<float> scratch;
    xpcog::AudioChunk  chunk;
    bool               endOfStream = false;

    const auto pump = [&]() -> bool {
        if (endOfStream) {
            return false;
        }
        if (!opened.decoder->readAudio(chunk)) {
            endOfStream = true;
            return false;
        }
        const std::size_t samples = xpcog::float32SampleCount(chunk);
        scratch.resize(samples);
        xpcog::convertToFloat32(chunk, scratch);

        // Spin until the ring accepts everything. The device drains it steadily,
        // so this yields rather than burning CPU.
        std::size_t written = 0;
        while (written < samples) {
            written += ring.write(scratch.data() + written, samples - written);
            if (written < samples) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        return true;
    };

    while (ring.availableToWrite() > ringSamples / 2 && pump()) {
    }

    if (!output->start(config)) {
        std::fputs("xpcog-cli: could not open an audio device\n", stderr);
        return 1;
    }

    std::fprintf(stderr, "playing %.1f s, %u ch @ %.0f Hz (device %.0f Hz)\n",
                 props.duration(), fmt.channels, fmt.sampleRate,
                 output->negotiatedFormat().sampleRate);

    while (pump()) {
    }

    // Underruns after this point are the expected tail: the decoder is done and
    // the device keeps asking until we stop it. Only the count taken here
    // indicates a genuine dropout.
    const auto underrunsWhilePlaying = output->underrunCount();

    // Let the device drain what is still queued before tearing it down.
    while (ring.availableToRead() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(
        static_cast<int>(output->latencySeconds() * 1000.0) + 50));

    output->stop();

    if (underrunsWhilePlaying > 0) {
        std::fprintf(stderr, "WARNING: %llu underrun(s) during playback\n",
                     static_cast<unsigned long long>(underrunsWhilePlaying));
    } else {
        std::fprintf(stderr, "no underruns (%llu tail callbacks after end of stream)\n",
                     static_cast<unsigned long long>(output->underrunCount()));
    }
    return underrunsWhilePlaying > 0 ? 1 : 0;
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

    if (command == "play") {
        return (argc < 3) ? usage() : play(argv[2]);
    }

    std::fprintf(stderr, "xpcog-cli: unknown command '%.*s'\n\n",
                 static_cast<int>(command.size()), command.data());
    return usage();
}
