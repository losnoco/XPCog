// AdLib and OPL2 formats, through AdPlug.
//
// Three things here are worth more than the rest.
//
// The first is priority. AdPlug shares `.raw` with vgmstream, which will accept
// any file under that name and report it as headerless PCM -- so an AdLib Rdos
// capture plays as noise unless AdPlug is offered it first, and nothing about
// that failure looks like a failure. `.s3m` is the same argument in the other
// direction: OpenMPT has to keep it. Both are pinned below, because the
// symptom of getting either wrong is audio rather than an error.
//
// The second is the file provider. AdPlug does not take a buffer -- it takes a
// filename and something that opens filenames -- and five of its players use
// that to reach a *second* file beside the first. Sierra's SCI is the case in
// the corpus: 120 files whose instrument bank sits next to them under a name
// derived by string surgery on theirs. They either all work or all fail, which
// makes them a good witness for a provider that is subtly wrong.
//
// The third is the container. Westwood's ADL is a bank of slots rather than a
// list of songs and half of them are empty, so expanding on the declared count
// fills a playlist with rows that play nothing.
//
// The corpus cannot be committed, so those cases run against a collection
// already on the machine (`-DXPCOG_ADPLUG_CORPUS=<path>`) and skip without one.
// Registration and rejection run everywhere.

#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace xpcog;
namespace fs = std::filesystem;

namespace {

PluginRegistry& registry() {
    static PluginRegistry instance;
    static const bool     once = [] {
        registerAllCodecs(instance);
        return true;
    }();
    (void)once;
    return instance;
}

#ifdef XPCOG_ADPLUG_CORPUS
constexpr bool kHaveCorpus = true;
[[nodiscard]] fs::path corpusRoot() { return fs::path{XPCOG_ADPLUG_CORPUS}; }
#else
constexpr bool kHaveCorpus = false;
[[nodiscard]] fs::path corpusRoot() { return {}; }
#endif

[[nodiscard]] std::string lowerExtension(const fs::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

/// Up to `want` files whose extension is `dotted` (".rad", and so on).
[[nodiscard]] std::vector<fs::path> findByExtension(std::string_view dotted,
                                                    std::size_t      want) {
    std::vector<fs::path> found;
    if (!kHaveCorpus) {
        return found;
    }

    std::error_code                  error;
    fs::recursive_directory_iterator walk{
        corpusRoot(), fs::directory_options::skip_permission_denied, error};
    if (error) {
        return found;
    }

    for (const fs::directory_entry& entry : walk) {
        if (found.size() >= want) {
            break;
        }
        if (entry.is_regular_file(error) && lowerExtension(entry.path()) == dotted) {
            found.push_back(entry.path());
        }
    }
    return found;
}

struct Decoded {
    std::vector<std::int16_t> samples;
    TrackProperties           properties;
};

[[nodiscard]] Decoded decode(const Url& url, std::size_t limitSamples) {
    Decoded                    out;
    PluginRegistry::OpenResult opened = registry().open(url);
    if (!opened) {
        return out;
    }
    out.properties = opened.decoder->properties();

    AudioChunk chunk;
    while (out.samples.size() < limitSamples && opened.decoder->readAudio(chunk)) {
        const std::size_t frames = chunk.frameCount();
        if (frames == 0) {
            break;
        }
        const std::size_t channels = chunk.format().channels;
        const std::size_t at       = out.samples.size();
        out.samples.resize(at + frames * channels);
        std::memcpy(out.samples.data() + at, chunk.bytes().data(),
                    frames * channels * sizeof(std::int16_t));
    }
    return out;
}

[[nodiscard]] int peak(const std::vector<std::int16_t>& samples) {
    int highest = 0;
    for (const std::int16_t sample : samples) {
        highest = std::max(highest, std::abs(static_cast<int>(sample)));
    }
    return highest;
}

[[nodiscard]] Url writeTemp(const std::string& name, const std::string& bytes) {
    const fs::path  dir = fs::temp_directory_path() / "xpcog-adplug-tests";
    std::error_code error;
    fs::create_directories(dir, error);
    const fs::path path = dir / name;

    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file != nullptr) {
        std::fwrite(bytes.data(), 1, bytes.size(), file);
        std::fclose(file);
    }
    return Url::fromLocalPath(path);
}

constexpr std::size_t kTenSeconds = 441000 * 2;

}  // namespace

TEST_CASE("AdPlug's extensions come from the library", "[adplug]") {
    // Not from a list in this tree. A representative handful rather than all
    // forty: what is being pinned is that the list was asked for and had its
    // leading dots stripped, not the contents of upstream's player table.
    CHECK(registry().isPlayableExtension("rad"));
    CHECK(registry().isPlayableExtension("hsc"));
    CHECK(registry().isPlayableExtension("d00"));
    CHECK(registry().isPlayableExtension("laa"));
    CHECK(registry().isPlayableExtension("cmf"));
    CHECK(registry().isPlayableExtension("sci"));
}

TEST_CASE("something that is not a module is declined", "[adplug]") {
    // The factory tries every player in turn, so a file it cannot place has
    // been refused forty-odd times rather than once. Worth pinning: the same
    // walk is what decides the extensions AdPlug shares with other decoders.
    CHECK_FALSE(registry().open(writeTemp("garbage.rad", "not an AdLib module")));
    CHECK_FALSE(registry().open(writeTemp("empty.hsc", "")));
}

TEST_CASE("an AdLib capture beats the raw-PCM catch-all", "[adplug][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_ADPLUG_CORPUS=<path> to run this");
    }
    const auto raws = findByExtension(".raw", 1);
    if (raws.empty()) {
        SKIP("corpus holds no .raw AdLib capture");
    }

    // `.raw` is claimed by vgmstream as well, for headerless PCM, and vgmstream
    // accepts *anything* under that name -- so with the priorities the wrong way
    // round this file opens, reports a sample rate, plays, and is noise. Cog
    // avoids it by running AdPlug above vgmstream; this pins that the same order
    // survives here, where the numbers are different.
    PluginRegistry::OpenResult opened = registry().open(Url::fromLocalPath(raws.front()));
    REQUIRE(opened);
    INFO(opened.decoder->properties().codec);
    CHECK(opened.decoder->properties().encoding == "synthesized");
}

TEST_CASE("an AdLib Scream Tracker module still goes to OpenMPT", "[adplug][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_ADPLUG_CORPUS=<path> to run this");
    }
    const auto mods = findByExtension(".s3m", 1);
    if (mods.empty()) {
        SKIP("corpus holds no .s3m");
    }

    // The other half of the priority argument. AdPlug reads AdLib-instrument
    // S3Ms and OpenMPT reads every S3M including those, so AdPlug must stay
    // *below* it -- one notch, not two. A single number governs both this and
    // the case above, which is why they are tested together.
    PluginRegistry::OpenResult opened = registry().open(Url::fromLocalPath(mods.front()));
    REQUIRE(opened);
    const std::string codec = opened.decoder->properties().codec;
    INFO(codec);
    CHECK(codec.find("Scream Tracker") != std::string::npos);
}

TEST_CASE("the formats render audio, not silence", "[adplug][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_ADPLUG_CORPUS=<path> to run this");
    }

    // One file per family rather than many of one: what varies between these is
    // the loader and the tick rate, and a decoder that got the second wrong
    // renders silence for some formats and not others.
    constexpr std::string_view kFamilies[] = {".rad", ".hsc", ".d00", ".laa",
                                              ".cmf", ".amd", ".mtk", ".sa2"};

    bool tested = false;
    for (const std::string_view family : kFamilies) {
        const auto files = findByExtension(family, 1);
        if (files.empty()) {
            continue;
        }
        INFO(files.front().filename().string());

        const Decoded decoded = decode(Url::fromLocalPath(files.front()), kTenSeconds);
        REQUIRE_FALSE(decoded.samples.empty());
        CHECK(decoded.properties.format.sampleRate == 44100.0);
        CHECK(decoded.properties.format.channels == 2);
        CHECK(decoded.properties.encoding == "synthesized");

        // An OPL chip that was reset and never programmed emits silence
        // perfectly well, so this is what separates "the file loaded" from "the
        // sequence ran".
        CHECK(peak(decoded.samples) > 500);
        tested = true;
    }

    if (!tested) {
        SKIP("corpus holds none of the formats this checks");
    }
}

TEST_CASE("a format that needs a companion file finds it", "[adplug][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_ADPLUG_CORPUS=<path> to run this");
    }
    const auto scis = findByExtension(".sci", 1);
    if (scis.empty()) {
        SKIP("corpus holds no Sierra SCI files");
    }

    // Sierra's SCI keeps its instrument bank in a sibling file, and AdPlug asks
    // for it by taking the name it was given and rewriting the last component.
    // So this exercises the whole provider: the name handed to the factory has
    // to survive that rewrite *and* come back as something openable.
    //
    // It is the case that caught the real bug. The name is a percent-decoded
    // URL, and re-encoding it with percentEncodePath() escapes the colon in
    // "file:" -- which parses as nothing, opens nothing, and fails every SCI
    // file in the corpus while every other format keeps working.
    INFO(scis.front().string());
    const Decoded decoded = decode(Url::fromLocalPath(scis.front()), kTenSeconds);
    REQUIRE_FALSE(decoded.samples.empty());
    CHECK(peak(decoded.samples) > 500);
}

TEST_CASE("only subsongs that play become playlist entries", "[adplug][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_ADPLUG_CORPUS=<path> to run this");
    }
    const auto adls = findByExtension(".adl", 4);
    if (adls.empty()) {
        SKIP("corpus holds no Westwood ADL files");
    }

    bool tested = false;
    for (const fs::path& path : adls) {
        const std::vector<Url> songs =
            registry().expandContainer(Url::fromLocalPath(path));
        if (songs.size() < 2) {
            continue;
        }
        INFO(path.filename().string() << " -> " << songs.size() << " rows");

        // Every row has to be worth having. An ADL is a bank of slots and about
        // half of them are empty, so counting getsubsongs() offers rows that
        // select nothing and report no duration -- which is what Cog does, and
        // what the corpus made visible.
        for (const Url& song : songs) {
            PluginRegistry::OpenResult opened = registry().open(song);
            REQUIRE(opened);
            INFO(song.fragment());
            CHECK(opened.decoder->properties().totalFrames > 0);
        }
        tested = true;
        break;
    }

    if (!tested) {
        SKIP("corpus holds no ADL with more than one song");
    }
}

TEST_CASE("seeking lands where it was asked to", "[adplug][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_ADPLUG_CORPUS=<path> to run this");
    }
    const auto files = findByExtension(".rad", 1);
    if (files.empty()) {
        SKIP("corpus holds no .rad");
    }

    PluginRegistry::OpenResult opened = registry().open(Url::fromLocalPath(files.front()));
    REQUIRE(opened);

    constexpr std::int64_t kTarget = 44100 * 5;
    if (opened.decoder->properties().totalFrames <= kTarget) {
        SKIP("first module is shorter than the seek target");
    }

    // Seeking runs the sequence and throws the audio away, so what can be
    // checked is that it stops exactly where it was told rather than at the
    // tick boundary it happened to reach -- the remainder of that tick is real
    // audio and is kept for the next read rather than dropped.
    CHECK(opened.decoder->seek(kTarget) == kTarget);
    CHECK(opened.decoder->seek(0) == 0);

    AudioChunk chunk;
    CHECK(opened.decoder->readAudio(chunk));
    CHECK(chunk.frameCount() > 0);
}
