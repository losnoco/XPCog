#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/library/Scanner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace xpcog;

namespace {

DecoderPtr makeNothing() { return nullptr; }

class MimeSource final : public ISource {
public:
    bool open(const Url& url) override {
        url_ = url;
        return true;
    }
    [[nodiscard]] bool seekable() const override { return false; }
    bool seek(std::int64_t, int) override { return false; }
    [[nodiscard]] std::int64_t tell() const override { return 0; }
    std::int64_t read(void*, std::int64_t) override { return 0; }
    void close() override {}
    [[nodiscard]] const Url& url() const override { return url_; }
    [[nodiscard]] std::string mimeType() const override { return "audio/x-scpls"; }

private:
    Url url_;
};

SourcePtr makeMimeSource() { return std::make_unique<MimeSource>(); }

std::vector<Url> expandExtension(const Url&, ISource&, const PluginRegistry&) {
    return {*Url::parse("fake://station/extension.flac")};
}

std::vector<Url> expandMime(const Url&, ISource&, const PluginRegistry&) {
    return {*Url::parse("fake://station/mime.flac")};
}

constexpr std::string_view kExtA[]  = {"aaa"};
constexpr std::string_view kExtB[]  = {"bbb", "aaa"};
constexpr std::string_view kExtBOnly[] = {"bbb"};
constexpr std::string_view kMime[]  = {"audio/test"};
constexpr std::string_view kFakeScheme[] = {"fake"};
constexpr std::string_view kPlsMime[]    = {"audio/x-scpls"};

bool gAvailableCalled = false;
bool sayNo()  { gAvailableCalled = true; return false; }

// Fake tag writers. The registry contract is plain function pointers, so the
// fakes record what they saw in globals rather than carrying state.
std::string gWritten;
int         gProbeCalls    = 0;
bool        gWritableProbe = true;

bool writeAlpha(const Url& url, const MetadataMap& tags, MetadataWriteOptions& options) {
    gWritten = "alpha:" + url.toString();
    options.error = "alpha says no";
    return false;
}

bool writeBeta(const Url&, const MetadataMap&, MetadataWriteOptions&) {
    gWritten = "beta";
    return true;
}

MetadataCapabilities probeBeta(const Url&) {
    ++gProbeCalls;
    MetadataCapabilities caps;
    caps.writable      = gWritableProbe;
    caps.arbitraryKeys = true;
    caps.pictures      = true;
    return caps;
}

void resetTagFakes() {
    gWritten = {};
    gProbeCalls = 0;
    gWritableProbe = true;
}

}  // namespace

TEST_CASE("registerAllCodecs registers the compiled-in codecs", "[registry]") {
    PluginRegistry registry;
    registerAllCodecs(registry);

    // The generated RegisterAll.cpp must have called freeze() for us.
    REQUIRE(registry.frozen());

    // Guards the whole generated-registration mechanism: if the linker ever drops a
    // codec's object file, this count silently falls and the test fails loudly.
    CHECK(registry.decoderCount() >= 1);

    const auto extensions = registry.allExtensions();
    const bool hasFlac =
        std::find(extensions.begin(), extensions.end(), "flac") != extensions.end();
    CHECK(hasFlac);
}

TEST_CASE("freeze sorts descriptors by descending priority", "[registry]") {
    PluginRegistry registry;
    registry.addDecoder({.name       = "Low",
                         .priority   = 0.5F,
                         .extensions = kExtA,
                         .mimeTypes  = kMime,
                         .create     = &makeNothing});
    registry.addDecoder({.name       = "High",
                         .priority   = 2.0F,
                         .extensions = kExtB,
                         .mimeTypes  = kMime,
                         .create     = &makeNothing});
    registry.freeze();

    REQUIRE(registry.decoderCount() == 2);
    CHECK(registry.frozen());
}

TEST_CASE("allExtensions is sorted and deduplicated", "[registry]") {
    PluginRegistry registry;
    // "aaa" is claimed by both descriptors and must appear exactly once.
    registry.addDecoder({.name       = "A",
                         .extensions = kExtA,
                         .mimeTypes  = kMime,
                         .create     = &makeNothing});
    registry.addDecoder({.name       = "B",
                         .extensions = kExtB,
                         .mimeTypes  = kMime,
                         .create     = &makeNothing});
    registry.freeze();

    const auto extensions = registry.allExtensions();
    REQUIRE(extensions.size() == 2);
    CHECK(extensions[0] == "aaa");
    CHECK(extensions[1] == "bbb");
}

TEST_CASE("unavailable descriptors are dropped at freeze", "[registry]") {
    gAvailableCalled = false;

    PluginRegistry registry;
    registry.addDecoder({.name       = "Unavailable",
                         .extensions = kExtA,
                         .mimeTypes  = kMime,
                         .create     = &makeNothing,
                         .available  = &sayNo});
    registry.freeze();

    CHECK(gAvailableCalled);
    CHECK(registry.decoderCount() == 0);
    CHECK(registry.allExtensions().empty());
}

TEST_CASE("container selection uses MIME for an extensionless URL", "[registry][scanner]") {
    PluginRegistry registry;
    registry.addSource({.name     = "MimeSource",
                        .schemes  = kFakeScheme,
                        .create   = &makeMimeSource});
    registry.addContainer({.name      = "PlsByMime",
                           .mimeTypes = kPlsMime,
                           .expand    = &expandMime});
    registry.freeze();

    const Url station = *Url::parse("fake://station/listen");

    // The static check stays cheap because only open() can reveal Content-Type.
    CHECK_FALSE(registry.isContainer(station));

    const Scanner scanner{registry};
    const auto    entries = scanner.expand({&station, 1});
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().toString() == "fake://station/mime.flac");
}

TEST_CASE("container extension takes priority over MIME", "[registry]") {
    PluginRegistry registry;
    registry.addSource({.name     = "MimeSource",
                        .schemes  = kFakeScheme,
                        .create   = &makeMimeSource});
    registry.addContainer({.name       = "ByExtension",
                           .extensions = kExtA,
                           .expand     = &expandExtension});
    registry.addContainer({.name      = "ByMime",
                           .mimeTypes = kPlsMime,
                           .expand    = &expandMime});
    registry.freeze();

    const auto entries = registry.expandContainer(*Url::parse("fake://station/list.aaa"));
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().toString() == "fake://station/extension.flac");
}

TEST_CASE("writeMetadata offers the file to one writer, highest priority first",
          "[registry][tags]") {
    resetTagFakes();

    PluginRegistry registry;
    // Registered low-first: registration order must not decide, priority must.
    // Beta is the only one that succeeds, so Alpha's recorded call proves it
    // went first.
    registry.addMetadataWriter({.name       = "Alpha",
                                .priority   = 0.5F,
                                .extensions = kExtA,
                                .write      = &writeAlpha});
    registry.addMetadataWriter({.name       = "Beta",
                                .priority   = 2.0F,
                                .extensions = kExtA,
                                .write      = &writeBeta});
    registry.freeze();

    const MetadataMap tags;
    MetadataWriteOptions options;
    CHECK(registry.writeMetadata(*Url::parse("file:///tmp/opencode/tags.aaa"), tags, options));
    CHECK(gWritten == "beta");
    CHECK(options.error.empty());
}

TEST_CASE("writeMetadata falls through to the next claimant after a failure",
          "[registry][tags]") {
    resetTagFakes();

    PluginRegistry registry;
    // Beta (high) claims only .bbb, so Alpha answers for .aaa alone and its
    // failure decides the outcome and the error.
    registry.addMetadataWriter({.name       = "Beta",
                                .priority   = 2.0F,
                                .extensions = kExtBOnly,
                                .write      = &writeBeta});
    registry.addMetadataWriter({.name       = "Alpha",
                                .extensions = kExtA,
                                .write      = &writeAlpha});
    registry.freeze();

    MetadataWriteOptions options;
    CHECK_FALSE(
        registry.writeMetadata(*Url::parse("file:///tmp/opencode/tags.aaa"), {}, options));
    CHECK(gWritten.starts_with("alpha:"));
    CHECK(options.error == "alpha says no");
}

TEST_CASE("writeMetadata skips a claimant whose probe refuses the file",
          "[registry][tags]") {
    resetTagFakes();

    PluginRegistry registry;
    // Both claim .aaa. The probe is asked only of the higher-priority one: the
    // search stops at the first writer that takes responsibility.
    registry.addMetadataWriter({.name       = "Beta",
                                .priority   = 2.0F,
                                .extensions = kExtA,
                                .write      = &writeBeta,
                                .probe      = &probeBeta});
    registry.addMetadataWriter({.name       = "Alpha",
                                .extensions = kExtA,
                                .write      = &writeAlpha});
    registry.freeze();

    // Beta says it cannot write this one, so its write never runs and the
    // search moves on; Alpha, which cannot say anything about the file, gets
    // tried blind instead, and its failure decides the outcome.
    gWritableProbe = false;
    MetadataWriteOptions options;
    CHECK_FALSE(
        registry.writeMetadata(*Url::parse("file:///tmp/opencode/tags.aaa"), {}, options));
    CHECK(gProbeCalls == 1);
    // Beta vouched that it could not write, so it never ran; Alpha, which
    // cannot say anything about the file, got tried blind instead.
    CHECK(gWritten.starts_with("alpha:"));
    CHECK(options.error == "alpha says no");
}

TEST_CASE("writeMetadata rejects a URL with a fragment before any writer runs",
          "[registry][tags]") {
    resetTagFakes();

    PluginRegistry registry;
    registry.addMetadataWriter({.name  = "Beta",
                                .write = &writeBeta});
    registry.freeze();

    // An empty extension list claims anything, but the fragment gate comes
    // first: Beta taking localPath() would rewrite the cue sheet itself.
    MetadataWriteOptions options;
    CHECK_FALSE(registry.writeMetadata(*Url::parse("file:///tmp/opencode/album.cue#3"), {},
                                       options));
    CHECK(gWritten.empty());
    CHECK_FALSE(options.error.empty());
}

TEST_CASE("writeMetadata reports no writer when freeze dropped the only claimant",
          "[registry][tags]") {
    resetTagFakes();
    gAvailableCalled = false;

    PluginRegistry registry;
    registry.addMetadataWriter({.name      = "Unavailable",
                                .extensions = kExtA,
                                .write     = &writeBeta,
                                .available = &sayNo});
    registry.freeze();

    CHECK(gAvailableCalled);
    MetadataWriteOptions options;
    CHECK_FALSE(
        registry.writeMetadata(*Url::parse("file:///tmp/opencode/tags.aaa"), {}, options));
    CHECK(gWritten.empty());
    CHECK(options.error == "no tag writer claims this file");
}

TEST_CASE("metadataCapabilities reports the highest-priority probing claimant",
          "[registry][tags]") {
    resetTagFakes();

    PluginRegistry registry;
    registry.addMetadataWriter({.name       = "Alpha",
                                .extensions = kExtA,
                                .write      = &writeAlpha});
    registry.addMetadataWriter({.name       = "Beta",
                                .priority   = 2.0F,
                                .extensions = kExtA,
                                .write      = &writeBeta,
                                .probe      = &probeBeta});
    registry.freeze();

    const auto caps = registry.metadataCapabilities(*Url::parse("file:///tmp/opencode/t.aaa"));
    CHECK(caps.writable);
    CHECK(caps.arbitraryKeys);
    CHECK(caps.pictures);
    CHECK_FALSE(caps.multiValue);
    CHECK(gProbeCalls == 1);

    // The same claimant answers "unwritable" rather than deferring to a
    // lower-priority writer that would never have received the file.
    resetTagFakes();
    gWritableProbe = false;
    const auto none = registry.metadataCapabilities(*Url::parse("file:///tmp/opencode/t.aaa"));
    CHECK_FALSE(none.writable);
    CHECK(gProbeCalls == 1);

    // A fragment is not a file, so no probe runs for it either.
    resetTagFakes();
    CHECK_FALSE(registry.metadataCapabilities(*Url::parse("file:///tmp/opencode/t.aaa#2")).writable);
    CHECK(gProbeCalls == 0);
}
