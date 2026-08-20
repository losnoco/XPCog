// Reading out of archives, both shapes of it.
//
// An archive of several tracks becomes one playlist row per member and is read
// through `unpack://`; a `.itz` is one module in a wrapper and is read as
// itself, with the unwrapping hidden inside the source. The two go together
// here because they share the libarchive plumbing and disagree about everything
// above it, which is exactly where a change to one quietly breaks the other.
//
// The archives are built here rather than checked in: a binary fixture cannot be
// read in a diff, and libarchive can write what it is about to read back.

#include "archive/ArchiveReader.hpp"
#include "archive/CompressedFileSource.hpp"
#include "archive/UnpackUrl.hpp"

#include "common/SourceBytes.hpp"

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
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

fs::path testDir() {
    static const fs::path dir = [] {
        auto path = fs::temp_directory_path() / "xpcog-archive-tests";
        fs::create_directories(path);
        return path;
    }();
    return dir;
}

using Entry = std::pair<std::string, std::string>;

/// Big enough to cross the 64 KiB read block several times, and not
/// compressible to nothing, so the chunked read loop is actually exercised.
std::string payload(std::size_t bytes, unsigned seed) {
    std::string out;
    out.reserve(bytes);
    unsigned state = seed * 2654435761U + 1U;
    for (std::size_t i = 0; i < bytes; ++i) {
        state = state * 1103515245U + 12345U;
        out.push_back(static_cast<char>((state >> 16) & 0xFF));
    }
    return out;
}

fs::path writeZip(const std::string& name, const std::vector<Entry>& entries) {
    const fs::path path = testDir() / name;

    struct archive* writer = archive_write_new();
    REQUIRE(writer != nullptr);
    REQUIRE(archive_write_set_format_zip(writer) == ARCHIVE_OK);
    REQUIRE(archive_write_open_filename(writer, path.string().c_str()) == ARCHIVE_OK);

    for (const auto& [member, contents] : entries) {
        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, member.c_str());
        archive_entry_set_size(entry, static_cast<la_int64_t>(contents.size()));
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        REQUIRE(archive_write_header(writer, entry) == ARCHIVE_OK);
        if (!contents.empty()) {
            REQUIRE(archive_write_data(writer, contents.data(), contents.size()) ==
                    static_cast<la_ssize_t>(contents.size()));
        }
        archive_entry_free(entry);
    }

    REQUIRE(archive_write_close(writer) == ARCHIVE_OK);
    archive_write_free(writer);
    return path;
}

fs::path writeRaw(const std::string& name, const std::string& contents) {
    const fs::path path = testDir() / name;
    std::FILE*     file = std::fopen(path.string().c_str(), "wb");
    REQUIRE(file != nullptr);
    std::fwrite(contents.data(), 1, contents.size(), file);
    std::fclose(file);
    return path;
}

/// The bytes a source serves, as a string, for comparing against what went in.
std::string readWhole(ISource& source) {
    const auto bytes = codecs::readAllBytes(source);
    REQUIRE(bytes.has_value());
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

/// A function rather than a namespace-scope `const bool`, and the difference is
/// not style: initialising one of those calls registerAllCodecs() *before main*,
/// where a codec whose registrar reads another translation unit's globals reads
/// them before that unit has initialised them. AdPlug is such a codec and this
/// crashed the whole suite at startup when it landed. See test_containers.cpp.
[[nodiscard]] bool haveArchive() {
    static const bool answer =
        registry().makeSource(*Url::parse("unpack://fex|1|a|b")) != nullptr;
    return answer;
}

/// Whether a module decoder is in this build, which is what decides if `.mod`
/// counts as playable and so whether an archive will offer one.
[[nodiscard]] bool haveModules() {
    static const bool answer = registry().isPlayableExtension("it");
    return answer;
}

}  // namespace

TEST_CASE("a member is read out of an archive", "[archive]") {
    if (!haveArchive()) {
        SKIP("archive codec not built");
    }

    const std::string module = payload(200000, 7);
    const fs::path    zip = writeZip("pack.zip", {{"songs/one.mod", module},
                                                  {"readme.txt", "not a track"}});

    SourcePtr source = registry().makeSource(codecs::makeUnpackUrl(zip, "songs/one.mod"));
    REQUIRE(source != nullptr);
    REQUIRE(source->open(codecs::makeUnpackUrl(zip, "songs/one.mod")));

    CHECK(source->seekable());
    CHECK(readWhole(*source) == module);

    // And it is a random-access view of the member, not a one-shot stream:
    // decoders seek constantly, which is the whole reason it is decompressed
    // whole in the first place.
    REQUIRE(source->seek(64, SEEK_SET));
    CHECK(source->tell() == 64);
    std::string window(16, '\0');
    CHECK(source->read(window.data(), 16) == 16);
    CHECK(window == module.substr(64, 16));

    REQUIRE(source->seek(0, SEEK_END));
    CHECK(source->tell() == static_cast<std::int64_t>(module.size()));
    CHECK(source->read(window.data(), 16) == 0);
}

TEST_CASE("a member the archive does not hold does not open", "[archive]") {
    if (!haveArchive()) {
        SKIP("archive codec not built");
    }

    const fs::path zip = writeZip("stale.zip", {{"one.mod", "x"}});

    // A playlist written before the archive was rebuilt names a member that is
    // no longer there; the row has to fail rather than open empty.
    const Url gone   = codecs::makeUnpackUrl(zip, "two.mod");
    SourcePtr source = registry().makeSource(gone);
    REQUIRE(source != nullptr);
    CHECK(!source->open(gone));
}

TEST_CASE("an archive offers only the members something can play", "[archive]") {
    if (!haveArchive()) {
        SKIP("archive codec not built");
    }
    if (!haveModules()) {
        SKIP("no module decoder built, so nothing in the archive is playable");
    }

    const fs::path zip = writeZip("mixed.zip", {
                                                   {"cover.jpg", "not audio"},
                                                   {"01 Song.it", payload(4096, 1)},
                                                   {"notes/readme.txt", "hello"},
                                                   // What a macOS-made zip carries
                                                   // beside the real files: this one
                                                   // ends in a playable extension and
                                                   // would otherwise appear as a
                                                   // second, unopenable copy.
                                                   {"__MACOSX/._01 Song.it", "fork"},
                                                   {".DS_Store", "junk"},
                                               });

    const std::vector<Url> tracks = registry().expandContainer(Url::fromLocalPath(zip));
    REQUIRE(tracks.size() == 1);

    const auto target = codecs::parseUnpackUrl(tracks.front());
    REQUIRE(target.has_value());
    CHECK(target->member == "01 Song.it");
}

TEST_CASE("archive bookkeeping files are recognised as such", "[archive]") {
    CHECK(codecs::isArchiveJunk("__MACOSX/._Track.spc"));
    CHECK(codecs::isArchiveJunk("._Track.spc"));
    CHECK(codecs::isArchiveJunk("sub/dir/._Track.spc"));
    CHECK(codecs::isArchiveJunk(".DS_Store"));
    CHECK(codecs::isArchiveJunk("sub/Thumbs.db"));

    // And real files are not, including ones whose names start awkwardly.
    CHECK(!codecs::isArchiveJunk("Track.spc"));
    CHECK(!codecs::isArchiveJunk(".hidden.mod"));
    CHECK(!codecs::isArchiveJunk("_Track.spc"));
    CHECK(!codecs::isArchiveJunk("__MACOSX_not_really/song.mod"));
}

TEST_CASE("a compressed module is unwrapped where nobody sees it", "[archive][module]") {
    if (!haveArchive()) {
        SKIP("archive codec not built");
    }

    const std::string module = payload(150000, 3);
    // Named with no extension at all inside, which packers do and which is why
    // the wrapper trusts the outer name rather than inspecting the member.
    const fs::path itz = writeZip("song.itz", {{"SONG", module}});
    const Url      url = Url::fromLocalPath(itz);

    SourcePtr source = registry().makeSource(url);
    REQUIRE(source != nullptr);
    REQUIRE(source->open(url));

    // The decoder sees the module, not the zip.
    CHECK(readWhole(*source) == module);

    // And it still sees the URL the user opened, which is also what picks the
    // decoder: lose the `itz` here and nothing claims the file.
    CHECK(source->url().extension() == "itz");
    CHECK(source->url() == url);
}

TEST_CASE("the wrapper skips bookkeeping to find the module", "[archive][module]") {
    if (!haveArchive()) {
        SKIP("archive codec not built");
    }

    const std::string module = payload(8192, 11);
    // The junk comes first, so taking the first entry blindly would serve it.
    const fs::path mdz = writeZip("mac.mdz", {{"__MACOSX/._song.mod", "fork"},
                                              {"song.mod", module}});
    const Url      url = Url::fromLocalPath(mdz);

    SourcePtr source = registry().makeSource(url);
    REQUIRE(source != nullptr);
    REQUIRE(source->open(url));
    CHECK(readWhole(*source) == module);
}

TEST_CASE("a compressed extension over something that is not an archive fails",
          "[archive][module]") {
    if (!haveArchive()) {
        SKIP("archive codec not built");
    }

    const fs::path fake = writeRaw("lying.s3z", payload(1024, 5));
    const Url      url  = Url::fromLocalPath(fake);

    SourcePtr source = registry().makeSource(url);
    REQUIRE(source != nullptr);
    // Refused rather than passed through as-is. Serving the raw bytes would hand
    // the decoder a file it cannot parse and report the failure a step later,
    // where it looks like a corrupt module instead of a mislabelled file.
    CHECK(!source->open(url));
}

TEST_CASE("only the claimed extensions are wrapped", "[archive][module]") {
    if (!haveArchive()) {
        SKIP("archive codec not built");
    }

    const std::string contents = payload(2048, 9);
    const fs::path    plain    = writeRaw("plain.mod", contents);
    const Url         url      = Url::fromLocalPath(plain);

    SourcePtr source = registry().makeSource(url);
    REQUIRE(source != nullptr);
    REQUIRE(source->open(url));
    CHECK(readWhole(*source) == contents);
}

TEST_CASE("the module decoder claims exactly what can be unwrapped",
          "[archive][module]") {
    if (!haveArchive() || !haveModules()) {
        SKIP("archive codec or module decoder not built");
    }

    // The two lists have to agree: a decoder claiming an extension no wrapper
    // unpacks advertises files that cannot be opened, and a wrapper for an
    // extension no decoder claims never gets asked.
    for (const std::string_view extension : codecs::compressedModuleExtensions()) {
        INFO(std::string(extension));
        CHECK(registry().isPlayableExtension(extension));
    }
}
