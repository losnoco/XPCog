// xpcog-cli -- the headless face of xpcog-core.
//
// This is the project's real test harness: it links no Qt, so it runs anywhere and
// exercises the engine without a display.

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Version.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/audio/AudioEngine.hpp"
#include "xpcog/core/audio/IAudioOutput.hpp"
#include "xpcog/core/audio/RingBuffer.hpp"
#include "xpcog/core/audio/SampleConvert.hpp"
#include "xpcog/core/library/Scanner.hpp"

#include <algorithm>
#include <cctype>
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
              "  expand <playlist>   list the tracks a playlist or container holds\n"
              "  scan <path>...      walk folders and playlists, reading tags\n"
              "  decode <in> <out>   decode to headerless native-endian PCM\n"
              "\n"
              "  play <file>...      play, gaplessly across multiple files");
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

/// Accepts a URL or a plain filesystem path, so the CLI is pleasant to use.
///
/// A trailing "#<digits>" is treated as a track fragment when the literal path
/// does not exist -- "album.cue#2" is far nicer to type than the file:// URL,
/// while a real file called "Track #1.flac" still resolves as itself.
xpcog::Url urlFromArgument(std::string_view argument) {
    if (auto parsed = xpcog::Url::parse(argument)) {
        return *parsed;
    }

    const std::filesystem::path literal{argument};
    if (!std::filesystem::exists(literal)) {
        const std::size_t hash = argument.rfind('#');
        if (hash != std::string_view::npos && hash + 1 < argument.size()) {
            const std::string_view tail = argument.substr(hash + 1);
            const bool allDigits = std::all_of(tail.begin(), tail.end(), [](char c) {
                return std::isdigit(static_cast<unsigned char>(c)) != 0;
            });
            if (allDigits) {
                return xpcog::Url::fromLocalPath(
                           std::filesystem::path{argument.substr(0, hash)})
                    .withFragment(tail);
            }
        }
    }
    return xpcog::Url::fromLocalPath(literal);
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
    // Through the Scanner rather than the decoder alone, so what is printed is
    // exactly what a scan would put in the library -- tags from the metadata
    // readers included, not just the ones a decoder happens to expose.
    xpcog::PlaylistEntry entry;
    entry.url = urlFromArgument(path);

    const xpcog::Scanner scanner{registry()};
    if (!scanner.readMetadata(entry)) {
        std::fprintf(stderr, "xpcog-cli: cannot open '%.*s': %s\n",
                     static_cast<int>(path.size()), path.data(),
                     entry.errorMessage.c_str());
        return 1;
    }

    const auto& props = entry.properties;
    const auto& fmt   = props.format;

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

    std::puts("tags:");
    const auto tagLine = [](const char* key, const std::string& value) {
        if (!value.empty()) {
            std::printf("  %-20s %s\n", key, value.c_str());
        }
    };
    tagLine("artist", entry.artist);
    tagLine("albumartist", entry.albumArtist);
    tagLine("album", entry.album);
    tagLine("title", entry.rawTitle);
    tagLine("genre", entry.genre);
    tagLine("composer", entry.composer);
    tagLine("date", entry.date);
    if (entry.track != 0) {
        std::printf("  %-20s %d\n", "track", entry.track);
    }
    if (entry.disc != 0) {
        std::printf("  %-20s %d\n", "disc", entry.disc);
    }

    for (const auto& tag : entry.metadata) {
        if (const auto* strings = std::get_if<std::vector<std::string>>(&tag.value)) {
            for (const auto& value : *strings) {
                std::printf("  %-20s %s\n", tag.key.c_str(), value.c_str());
            }
        } else if (const auto* bytes =
                       std::get_if<std::vector<std::byte>>(&tag.value)) {
            std::printf("  %-20s <%zu bytes>\n", tag.key.c_str(), bytes->size());
        }
    }

    return 0;
}

int scan(const std::vector<std::string>& arguments) {
    std::vector<xpcog::Url> inputs;
    inputs.reserve(arguments.size());
    for (const std::string& argument : arguments) {
        inputs.push_back(urlFromArgument(argument));
    }

    const xpcog::Scanner scanner{registry()};
    const auto           entries = scanner.scan(inputs);

    int failures = 0;
    for (const xpcog::PlaylistEntry& entry : entries) {
        if (entry.error) {
            ++failures;
            std::printf("  !! %s (%s)\n", entry.filename().c_str(),
                        entry.errorMessage.c_str());
            continue;
        }
        std::printf("%3d. %-44.44s %6.1f s  %s\n", entry.track, entry.display().c_str(),
                    entry.duration(), entry.properties.codec.c_str());
    }

    std::printf("\n%zu entries", entries.size());
    if (failures > 0) {
        std::printf(", %d unreadable", failures);
    }
    std::putchar('\n');
    return failures == 0 ? 0 : 1;
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

/// Plays one or more files back to back through the real engine, so the CLI
/// exercises the same gapless path the tests cover.
int play(const std::vector<std::string>& paths) {
    struct Playlist final : xpcog::AudioEngine::Delegate {
        std::vector<xpcog::Url> queue;
        std::size_t             next = 0;

        std::optional<xpcog::Url> nextTrack() override {
            return (next < queue.size()) ? std::optional{queue[next++]} : std::nullopt;
        }
        void trackBegan(const xpcog::Url& url) override {
            std::fprintf(stderr, "playing: %s\n", url.toString().c_str());
        }
        void trackFailed(const xpcog::Url& url) override {
            std::fprintf(stderr, "skipped (cannot open): %s\n", url.toString().c_str());
        }
    };

    // Probe the first track for a ring size. The engine and the output must share
    // one ring, so it is built here and handed to both.
    auto probe = registry().open(urlFromArgument(paths.front()));
    if (!probe) {
        std::fprintf(stderr, "xpcog-cli: cannot open '%s'\n", paths.front().c_str());
        return 1;
    }
    const auto fmt = probe.decoder->properties().format;
    probe          = {};

    xpcog::RingBuffer ring(
        static_cast<std::size_t>(fmt.sampleRate * 0.5) * fmt.channels);
    auto output = xpcog::makeMiniaudioOutput(ring);

    auto             store = xpcog::makeMemorySettingsStore();
    xpcog::Settings  settings(*store);
    xpcog::AudioEngine engine(registry(), *output, ring, settings);

    Playlist playlist;
    for (std::size_t i = 1; i < paths.size(); ++i) {
        playlist.queue.push_back(urlFromArgument(paths[i]));
    }
    engine.setDelegate(&playlist);

    if (!engine.play(urlFromArgument(paths.front()))) {
        std::fprintf(stderr, "xpcog-cli: cannot play '%s'\n", paths.front().c_str());
        return 1;
    }

    engine.waitUntilFinished();
    const auto underruns = engine.underrunCount();
    engine.stop();

    if (underruns > 0) {
        std::fprintf(stderr, "%llu underrun(s)\n",
                     static_cast<unsigned long long>(underruns));
    }
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

    if (command == "expand") {
        if (argc < 3) {
            return usage();
        }
        for (const auto& entry : registry().expandContainer(urlFromArgument(argv[2]))) {
            std::printf("%s\n", entry.toString().c_str());
        }
        return 0;
    }

    if (command == "scan") {
        if (argc < 3) {
            return usage();
        }
        return scan(std::vector<std::string>(argv + 2, argv + argc));
    }

    if (command == "info") {
        return (argc < 3) ? usage() : info(argv[2]);
    }

    if (command == "decode") {
        return (argc < 4) ? usage() : decode(argv[2], argv[3]);
    }

    if (command == "play") {
        if (argc < 3) {
            return usage();
        }
        return play(std::vector<std::string>(argv + 2, argv + argc));
    }

    std::fprintf(stderr, "xpcog-cli: unknown command '%.*s'\n\n",
                 static_cast<int>(command.size()), command.data());
    return usage();
}
