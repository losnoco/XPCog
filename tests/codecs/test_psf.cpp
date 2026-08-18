// The PSF container: chains, tags and times.
//
// The `_lib` chain is what these are really about. A `.minigsf` is a few hundred
// bytes naming a `.gsflib` that holds the game's whole program -- in the corpus
// this was written against, 783 `.minigsf` resolve to 12 `.gsflib`, so getting
// the chain wrong means 771 files that look fine and play the wrong music, or
// nothing.
//
// Rips cannot be committed, so the chain cases run against a corpus already on
// the machine (`-DXPCOG_PSF_CORPUS=<path>`) and skip without one. The time
// parsing needs no fixture and always runs -- it is pure text, and it is the
// part that silently truncates a track when it is wrong.

#include "psf/PsfFile.hpp"

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using namespace xpcog;
using namespace xpcog::codecs;
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

#ifdef XPCOG_PSF_CORPUS
constexpr bool kHaveCorpus = true;
[[nodiscard]] fs::path corpusRoot() { return fs::path{XPCOG_PSF_CORPUS}; }
#else
constexpr bool kHaveCorpus = false;
[[nodiscard]] fs::path corpusRoot() { return {}; }
#endif

/// A handful of mini-PSFs, which are the interesting case: each one is an
/// override that means nothing without the library it names.
[[nodiscard]] std::vector<fs::path> findMiniPsfs(std::size_t want) {
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
        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension == ".minigsf" || extension == ".miniusf" ||
            extension == ".mini2sf") {
            found.push_back(entry.path());
        }
    }
    return found;
}

}  // namespace

TEST_CASE("a mini-PSF pulls in the library it names", "[psf][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto minis = findMiniPsfs(6);
    REQUIRE_FALSE(minis.empty());

    for (const fs::path& path : minis) {
        INFO(path.filename().string());

        const auto file = loadPsf(Url::fromLocalPath(path), registry());
        REQUIRE(file.has_value());

        // The version byte says which emulator the program is for, and every
        // format in this corpus has one.
        CHECK(file->version != 0);

        // The point of the whole exercise: a mini file on its own is a few
        // hundred bytes, so more than one program image means the chain was
        // followed and the library came back with it.
        INFO("programs in chain: " << file->programs.size());
        CHECK(file->programs.size() >= 2);

        // And the library is the big one. A chain that "resolved" to two tiny
        // images would pass a count check while having loaded nothing useful.
        //
        // Both sections count, because which one carries the program depends on
        // the console. GSF puts the GBA image in `exe`; USF leaves `exe` empty
        // and puts the N64 data in `reserved`, which is where lazyusf looks --
        // measured here, after this check failed on every .miniusf in the corpus
        // while the chain itself was resolving perfectly.
        std::size_t largest = 0;
        for (const PsfProgram& program : file->programs) {
            largest = std::max({largest, program.exe.size(), program.reserved.size()});
        }
        CHECK(largest > 4096);
    }
}

TEST_CASE("tags come from the outermost file", "[psf][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto minis = findMiniPsfs(20);
    REQUIRE_FALSE(minis.empty());

    // Not every rip is tagged, so this asserts across the sample rather than
    // per file: a corpus where nothing at all carried a title or a length would
    // mean the tag block is not being read.
    int titled = 0;
    int timed  = 0;
    for (const fs::path& path : minis) {
        const auto file = readPsfTags(Url::fromLocalPath(path), registry());
        REQUIRE(file.has_value());
        if (!file->tags.first("title").empty()) {
            ++titled;
        }
        if (file->length.has_value()) {
            ++timed;
        }
    }
    INFO("titled " << titled << " / timed " << timed << " of " << minis.size());
    CHECK(titled > 0);
    CHECK(timed > 0);
}

TEST_CASE("reading tags does not inflate the program", "[psf][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto minis = findMiniPsfs(1);
    REQUIRE_FALSE(minis.empty());

    // A .gsflib can be megabytes and none of it is needed to answer "what is
    // this track called". The scanner reads tags for every file in a library, so
    // the cheap path has to actually be cheap.
    const auto tagsOnly = readPsfTags(Url::fromLocalPath(minis.front()), registry());
    REQUIRE(tagsOnly.has_value());
    CHECK(tagsOnly->programs.empty());

    const auto whole = loadPsf(Url::fromLocalPath(minis.front()), registry());
    REQUIRE(whole.has_value());
    CHECK_FALSE(whole->programs.empty());
}

TEST_CASE("a missing library is a failure, not a partial load", "[psf][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto minis = findMiniPsfs(1);
    REQUIRE_FALSE(minis.empty());

    // Copied away from its library, a mini-PSF names something that is not
    // there. Loading it "successfully" with only its own few hundred bytes
    // would hand an emulator an empty machine and play silence.
    const fs::path orphan =
        fs::temp_directory_path() / "xpcog-psf-orphan" / minis.front().filename();
    std::error_code error;
    fs::create_directories(orphan.parent_path(), error);
    fs::copy_file(minis.front(), orphan, fs::copy_options::overwrite_existing, error);
    REQUIRE_FALSE(error);

    CHECK_FALSE(loadPsf(Url::fromLocalPath(orphan), registry()).has_value());
    fs::remove_all(orphan.parent_path(), error);
}

TEST_CASE("PSF times parse the way the format writes them", "[psf]") {
    // No fixture needed, and worth pinning: PSF has no intrinsic duration -- the
    // program would run for ever -- so this tag is the only thing that makes a
    // track finite. Misread it and every track in a set is the wrong length.
    using Catch::Matchers::WithinAbs;

    // Bare seconds, which is what most rips carry.
    CHECK_THAT(*parsePsfTime("90"), WithinAbs(90.0, 1e-9));
    // Minutes and seconds.
    CHECK_THAT(*parsePsfTime("1:30"), WithinAbs(90.0, 1e-9));
    CHECK_THAT(*parsePsfTime("2:05"), WithinAbs(125.0, 1e-9));
    // And hours, for the long ambient tracks that do exist.
    CHECK_THAT(*parsePsfTime("1:00:00"), WithinAbs(3600.0, 1e-9));

    // The fraction is DECIMAL, not a frame count: "1:30.5" is ninety and a half
    // seconds, not ninety seconds and five frames. Anyone arriving from cue
    // sheets, where MM:SS:FF is 75 frames to the second, gets this wrong.
    CHECK_THAT(*parsePsfTime("1:30.5"), WithinAbs(90.5, 1e-9));
    CHECK_THAT(*parsePsfTime("0.250"), WithinAbs(0.25, 1e-9));

    // Surrounding space is common in hand-edited tags and is not an error.
    CHECK_THAT(*parsePsfTime(" 1:30 "), WithinAbs(90.0, 1e-9));

    // Nothing at all is nullopt rather than zero. A track with no length tag
    // must not become a track of length zero, which would skip instantly.
    CHECK_FALSE(parsePsfTime("").has_value());
    CHECK_FALSE(parsePsfTime("   ").has_value());
    CHECK_FALSE(parsePsfTime("forever").has_value());
    CHECK_FALSE(parsePsfTime("1:").has_value());
    CHECK_FALSE(parsePsfTime(":30").has_value());
}
