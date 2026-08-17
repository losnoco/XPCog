// xpcog-cli -- the headless face of xpcog-core.
//
// This is the project's real test harness: it links no Qt, so it runs anywhere and
// exercises the engine without a display. `play` and `decode` arrive in M1a.

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Version.hpp"

#include <cstdio>
#include <string_view>

namespace {

int usage() {
    std::puts("usage: xpcog-cli <command> [args]\n"
              "\n"
              "  --version        print version and exit\n"
              "  codecs           list compiled-in codecs and claimed extensions\n"
              "\n"
              "  play <file>      (M1a)\n"
              "  decode <in> <out> (M1a)\n"
              "  info <file>      (M1b)");
    return 2;
}

int listCodecs() {
    xpcog::PluginRegistry registry;
    xpcog::registerAllCodecs(registry);

    std::printf("%zu decoder(s), %zu source(s)\n", registry.decoderCount(),
                registry.sourceCount());
    std::fputs("extensions:", stdout);
    for (const auto& ext : registry.allExtensions()) {
        std::printf(" %s", ext.c_str());
    }
    std::putchar('\n');
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

    std::fprintf(stderr, "xpcog-cli: unknown command '%.*s'\n\n",
                 static_cast<int>(command.size()), command.data());
    return usage();
}
