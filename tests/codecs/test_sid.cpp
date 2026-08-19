// SID: that libsidplayfp runs a Commodore 64, and that subsongs are addressed
// rather than ignored.
//
// The subsong case is the one worth the most here. A C64 tune usually holds
// several -- title music, in-game music, jingles -- and they are selected by a
// number handed to the tune's own init routine, not by an offset into a file.
// So a decoder that drops the fragment still opens every URL, still reports a
// duration, and still plays: the first song, once per entry in the playlist.
// The test that catches that is two subsongs of one file rendering differently.
//
// The other half is the container, which decides how many entries appear at
// all. Cog expands `.sid` into `#1`..`#n`; note the one, since libsidplayfp
// numbers songs from one where GME numbers tracks from zero, and an off-by-one
// here is a silently missing last track.
//
// Rips cannot be committed, so these run against a corpus already on the
// machine (`-DXPCOG_SID_CORPUS=<path>`) and skip without one.

#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
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

#ifdef XPCOG_SID_CORPUS
constexpr bool kHaveCorpus = true;
[[nodiscard]] fs::path corpusRoot() { return fs::path{XPCOG_SID_CORPUS}; }
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

/// PSID files only. A set downloaded from the wild can hold PowerPacker-packed
/// tunes -- `PP20` where the magic should be -- and libsidplayfp 2.x removed
/// PP20 support, so refusing them is correct behaviour and not a case these
/// tests should be measuring.
[[nodiscard]] std::vector<fs::path> findSids(std::size_t want) {
    std::vector<fs::path> found;
    if (!kHaveCorpus) {
        return found;
    }

    std::error_code error;
    fs::recursive_directory_iterator walk{
        corpusRoot(), fs::directory_options::skip_permission_denied, error};
    if (error) {
        return found;
    }

    for (const fs::directory_entry& entry : walk) {
        if (found.size() >= want) {
            break;
        }
        if (!entry.is_regular_file(error)) {
            continue;
        }
        if (lowerExtension(entry.path()) != ".sid") {
            continue;
        }
        char        magic[4] = {};
        std::FILE*  file     = std::fopen(entry.path().string().c_str(), "rb");
        if (file == nullptr) {
            continue;
        }
        const std::size_t read = std::fread(magic, 1, 4, file);
        std::fclose(file);
        if (read != 4 || (std::memcmp(magic, "PSID", 4) != 0 &&
                          std::memcmp(magic, "RSID", 4) != 0)) {
            continue;
        }
        found.push_back(entry.path());
    }
    return found;
}

struct Decoded {
    std::vector<std::int16_t> samples;
    TrackProperties           properties;
};

[[nodiscard]] Decoded decode(const Url& url, std::size_t limitSamples) {
    Decoded out;
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

constexpr std::size_t kTenSeconds = 441000;

}  // namespace

TEST_CASE("the SID decoder is registered", "[sid]") {
    CHECK(registry().isPlayableExtension("sid"));
    CHECK(registry().isPlayableExtension("mus"));
}

TEST_CASE("a SID renders audio, not silence", "[sid][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_SID_CORPUS=<path> to run this");
    }
    const auto sids = findSids(5);
    if (sids.empty()) {
        SKIP("corpus holds no SID files");
    }

    for (const fs::path& path : sids) {
        INFO(path.filename().string());
        const Decoded decoded = decode(Url::fromLocalPath(path), kTenSeconds);
        REQUIRE_FALSE(decoded.samples.empty());
        CHECK(decoded.properties.format.sampleRate == 44100.0);
        CHECK(decoded.properties.codec == "SID");

        // A C64 with no tune loaded emits silence perfectly well, so the level
        // is what separates "the machine ran" from "the machine played".
        CHECK(peak(decoded.samples) > 500);
    }
}

TEST_CASE("a SID is mono unless the tune asks for more chips", "[sid][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_SID_CORPUS=<path> to run this");
    }
    const auto sids = findSids(20);
    if (sids.empty()) {
        SKIP("corpus holds no SID files");
    }

    // The channel count is a property of the tune rather than of the format --
    // one SID is mono, two or three are mixed to stereo -- so what is pinned
    // here is that it is one of those two and never something else.
    for (const fs::path& path : sids) {
        INFO(path.filename().string());
        PluginRegistry::OpenResult opened =
            registry().open(Url::fromLocalPath(path));
        REQUIRE(opened);
        const std::uint32_t channels = opened.decoder->properties().format.channels;
        CHECK((channels == 1 || channels == 2));
    }
}

TEST_CASE("a tune with subsongs expands to one entry each", "[sid][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_SID_CORPUS=<path> to run this");
    }
    const auto sids = findSids(60);
    if (sids.empty()) {
        SKIP("corpus holds no SID files");
    }

    bool tested = false;
    for (const fs::path& path : sids) {
        const std::vector<Url> songs = registry().expandContainer(Url::fromLocalPath(path));
        if (songs.size() < 2) {
            continue;
        }
        INFO(path.filename().string());

        // Numbered from one, not zero. libsidplayfp treats song 0 as "the
        // tune's own default", so a container that counted from zero would
        // produce one entry that is a duplicate and lose the last song.
        CHECK(songs.front().fragment() == "1");
        CHECK(songs.back().fragment() == std::to_string(songs.size()));

        // The decisive one: two subsongs of the same file must not render the
        // same audio. If the fragment is dropped anywhere between the URL and
        // selectSong(), every entry plays song one and nothing else fails --
        // the files open, the durations are right, the audio is real.
        //
        // Compared by *how much* they differ rather than by std::equal, and
        // that is not fussiness. reSIDfp does not render bit-identically twice
        // in one process: two decodes of the same song diverge by a single LSB
        // within the first twenty samples, which reads as second-run state
        // rather than anything the caller controls (a fresh process is
        // reproducible; the second decode inside one is not). An exact
        // comparison therefore passes whether or not subsong selection works,
        // and it was written that way first -- it passed with selectSong()
        // sabotaged, which is how this was found.
        const Decoded first = decode(songs.front(), kTenSeconds);
        const Decoded other = decode(songs.back(), kTenSeconds);
        REQUIRE_FALSE(first.samples.empty());
        REQUIRE_FALSE(other.samples.empty());

        const std::size_t common =
            std::min(first.samples.size(), other.samples.size());
        REQUIRE(common > 0);

        // A threshold far above that LSB and far below a musical difference.
        constexpr int kNoiseFloor = 256;
        std::size_t   differing   = 0;
        for (std::size_t i = 0; i < common; ++i) {
            if (std::abs(static_cast<int>(first.samples[i]) -
                         static_cast<int>(other.samples[i])) > kNoiseFloor) {
                ++differing;
            }
        }
        // Two different pieces of music disagree across most of their length;
        // the same piece twice disagrees essentially nowhere.
        CHECK(differing > common / 10);
        tested = true;
        break;
    }

    if (!tested) {
        SKIP("corpus holds no SID with more than one subsong");
    }
}
