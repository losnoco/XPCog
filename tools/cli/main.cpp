// xpcog-cli -- the headless face of xpcog-core.
//
// This is the project's real test harness: it links no UI toolkit, so it runs
// anywhere and exercises the engine without a display.

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
#ifdef XPCOG_HAS_REST
#    include "CliPlayerControl.hpp"

#    include "xpcog/core/remote/RemoteServer.hpp"
#    include "xpcog/core/remote/Token.hpp"
#endif

#include "xpcog/core/SerialExecutor.hpp"
#include "xpcog/core/library/Scanner.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <thread>

#ifdef _WIN32
#    include <io.h>
#else
#    include <unistd.h>
#endif

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
              "  play <file>...      play, gaplessly across multiple files\n"
              "\n"
              "  serve [options] [file]...\n"
              "                      run the REST remote control over these tracks\n"
              "    --port N          port to bind (default 7799, 0 for any free one)\n"
              "    --address A       address to bind (default 127.0.0.1)\n"
              "    --token-file F    read the access token from F\n"
              "                      (else $XPCOG_REMOTE_TOKEN, else one is printed)\n"
              "    --read-only       serve reads and refuse every write\n"
              "\n"
              "  What `serve` cannot do, and answers 501 for: skipping a track that\n"
              "  will not open, cover art, ratings, and any desktop integration. It\n"
              "  has no 409 either -- a slow track holds its queue and requests time\n"
              "  out as 503 instead. See docs/REST.md.");
    return 2;
}

xpcog::PluginRegistry& registry() {
    // Constructed in place: PluginRegistry is deliberately non-copyable, since the
    // decoders it hands out reference descriptors stored inside it.
    static xpcog::PluginRegistry instance;
    static const bool            once = [] {
        // Defaults, held for the life of the process: the CLI has no settings
        // file, and codecs read these live. Without them every synthesised
        // format would fall back internally to the same numbers by a longer
        // route, so this is about the CLI agreeing with the app rather than
        // about behaviour.
        static auto            store    = xpcog::makeMemorySettingsStore();
        static xpcog::Settings defaults{*store};
        instance.setSettings(&defaults);

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
        if (rg.trackGain) std::printf(" track %+.2f dB", static_cast<double>(*rg.trackGain));
        if (rg.albumGain) std::printf(" album %+.2f dB", static_cast<double>(*rg.albumGain));
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
    // Promoted to a column like the rest of these, and so removed from the map
    // below -- so without a line of its own it is the one thing a scan stores
    // that this never showed. It reads as "no lyrics in the file", which is a
    // different and wrong answer.
    tagLine("unsyncedlyrics", entry.unsyncedLyrics);
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
#ifdef XPCOG_HAS_REST

/// Is there a terminal on stdin? Decides how `serve` waits.
bool isInteractive() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

int serve(const std::vector<std::string>& args) {
    std::string address   = "127.0.0.1";
    int         port      = 7799;
    std::string tokenFile;
    bool        readOnly  = false;
    std::vector<std::string> files;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        auto               value = [&](const char* name) -> std::string {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "xpcog-cli: %s needs a value\n", name);
                std::exit(2);
            }
            return args[++i];
        };
        if (arg == "--port") {
            port = std::atoi(value("--port").c_str());
        } else if (arg == "--address") {
            address = value("--address");
        } else if (arg == "--token-file") {
            tokenFile = value("--token-file");
        } else if (arg == "--read-only") {
            readOnly = true;
        } else if (arg.rfind("--", 0) == 0) {
            std::fprintf(stderr, "xpcog-cli: unknown option '%s'\n", arg.c_str());
            return 2;
        } else {
            files.push_back(arg);
        }
    }

    // The token, in the order a script would want: a file it wrote, then the
    // environment, then one generated and printed. Printed rather than stored,
    // because this host has nowhere to store it -- the application's keeps it in
    // the system password store, and inventing a file to hold a credential here
    // would be the worse half of that idea.
    std::string token;
    if (!tokenFile.empty()) {
        std::ifstream file{tokenFile};
        if (!file) {
            std::fprintf(stderr, "xpcog-cli: cannot read '%s'\n", tokenFile.c_str());
            return 1;
        }
        std::getline(file, token);
        while (!token.empty() && (token.back() == '\r' || token.back() == '\n')) {
            token.pop_back();
        }
    } else if (const char* fromEnv = std::getenv("XPCOG_REMOTE_TOKEN");
               fromEnv != nullptr && *fromEnv != '\0') {
        token = fromEnv;
    } else {
        token = xpcog::remote::generateRemoteToken();
        if (token.empty()) {
            std::fputs("xpcog-cli: the system random generator would not answer\n",
                       stderr);
            return 1;
        }
        std::printf("access token: %s\n", token.c_str());
        std::puts("  (not stored; pass --token-file or set XPCOG_REMOTE_TOKEN to keep one)");
    }

    // 48 kHz stereo is a guess for the ring, and it is only a guess: the engine
    // reopens the device for whatever a track actually is. Sized like the play
    // command's, from the first track where there is one.
    xpcog::AudioFormat format;
    format.sampleRate = 48000;
    format.channels   = 2;
    if (!files.empty()) {
        if (auto probe = registry().open(urlFromArgument(files.front()))) {
            format = probe.decoder->properties().format;
        }
    }

    xpcog::RingBuffer ring(
        static_cast<std::size_t>(format.sampleRate * 0.5) * format.channels);
    auto output = xpcog::makeMiniaudioOutput(ring);

    auto              store = xpcog::makeMemorySettingsStore();
    xpcog::Settings   settings(*store);
    xpcog::AudioEngine engine(registry(), *output, ring, settings);

    xpcog::cli::CliPlayerControl control(registry(), engine, settings);
    engine.setDelegate(&control);

    if (!files.empty()) {
        std::vector<xpcog::Url> inputs;
        inputs.reserve(files.size());
        for (const std::string& file : files) {
            inputs.push_back(urlFromArgument(file));
        }
        xpcog::Scanner scanner(registry());
        control.playlist().insert(0, scanner.scan(inputs));
        std::printf("playlist: %zu track(s)\n", control.playlist().size());
    }

    // The SerialExecutor *is* the interface thread. Everything the gate
    // dispatches lands on it, one at a time, which is the same contract the
    // application's CallAfter provides -- so CliPlayerControl may touch the
    // playlist unlocked for the same reason AppPlayerControl may.
    xpcog::SerialExecutor executor;

    xpcog::remote::ServerConfig config;
    config.address    = address;
    config.port       = port;
    config.token      = token;
    config.allowWrite = !readOnly;

    xpcog::remote::RemoteServer server{
        control, [&executor](std::function<void()> job) { executor.post(std::move(job)); },
        std::move(config)};

    std::string error;
    if (!server.start(&error)) {
        std::fprintf(stderr, "xpcog-cli: %s\n", error.c_str());
        return 1;
    }

    std::printf("listening on http://%s:%d%s\n", address.c_str(), server.boundPort(),
                readOnly ? "  (read-only)" : "");
    std::printf("  docs: http://%s:%d/docs\n", address.c_str(), server.boundPort());
    std::fflush(stdout);

    // How this stops depends on whether anybody is typing at it.
    //
    // On a terminal, Enter. Redirected -- under nohup, a service manager, or a
    // test harness -- stdin is at end of file immediately, and waiting on it
    // would have the server bind a port and exit in the same breath. So that case
    // sleeps instead and is ended by a signal, which is what those callers send.
    //
    // No signal handler: Ctrl-C ends the process without running destructors, and
    // nothing here needs flushing. The settings are in memory and the playlist
    // was never persisted.
    if (isInteractive()) {
        std::puts("press Enter to stop");
        static_cast<void>(std::getchar());
    } else {
        std::puts("running until terminated");
        std::fflush(stdout);
        for (;;) {
            std::this_thread::sleep_for(std::chrono::hours{1});
        }
    }

    server.stop();
    engine.stop();
    return 0;
}

#else  // XPCOG_HAS_REST

int serve(const std::vector<std::string>&) {
    std::fputs("xpcog-cli: this build has no remote-control server "
               "(configure with -DXPCOG_WITH_REST=ON)\n",
               stderr);
    return 1;
}

#endif  // XPCOG_HAS_REST

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
        void streamMetadataChanged(const xpcog::Url&,
                                   const xpcog::MetadataMap& tags) override {
            // What a radio station is playing right now, from either half of the
            // seam: a SHOUTcast StreamTitle beside the audio, or an ID3v2 tag
            // inside it. Printing it is also the only way to watch that path
            // work against a real station without a GUI.
            const std::string artist{tags.first("artist")};
            const std::string title{tags.first("title")};
            if (artist.empty() && title.empty()) {
                return;
            }
            std::fprintf(stderr, "now playing: %s%s%s\n", artist.c_str(),
                         (artist.empty() || title.empty()) ? "" : " - ", title.c_str());
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

    if (command == "serve") {
        return serve({argv + 2, argv + argc});
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
