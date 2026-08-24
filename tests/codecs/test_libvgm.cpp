// VGM, S98, DRO and GYM -- chip register logs, replayed on emulated hardware.
//
// The claim that matters most here is not about decoding at all: it is that
// **this decoder is the one that gets these files**. Every extension it claims
// is already claimed by something else -- `.vgm` and `.vgz` by Game_Music_Emu
// and vgmstream, `.gym` by Game_Music_Emu, `.dro` by AdPlug and vgmstream --
// and unlike `.ahx` or `.mus`, content does not settle it, because all of them
// genuinely read these files. The codec string is what proves which one ran.
//
// What this does *not* pin is the priority's exact value. Ties break by
// registration order, and today that order already favours libvgm, so lowering
// 1.25 to the default changes nothing -- while lowering it below the catch-alls
// hands every one of these files to Game_Music_Emu, which this catches. That is
// the regression worth catching; the number itself is insurance against an
// ordering that nobody chose.
//
// The second is that the reported length is the audio that comes out. Cog
// reports one pass through the file and then plays the loops, the fade and the
// trailing silence on top -- roughly half the real time, on a looping VGM,
// which is most of them.
//
// What is *not* tested here is the emulation, and deliberately: it is libvgm's,
// and it was checked against libvgm's own vgm2wav over all 63 files in the
// corpus -- byte-identical, with exactly the half second of trailing silence
// this configuration adds and vgm2wav's does not. That is recorded in
// docs/PORTING.md rather than reproduced here, because a test that shells out
// to a tool nobody has is a test that always skips.
//
// S98 and GYM have no fixture anywhere on hand, so nothing below opens one. The
// registration case covers that they are claimed; the players themselves are
// libvgm's and are exercised by its own suite.

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

#ifdef XPCOG_VGM_CORPUS
constexpr bool kHaveCorpus = true;
[[nodiscard]] fs::path corpusRoot() { return fs::path{XPCOG_VGM_CORPUS}; }
#else
constexpr bool kHaveCorpus = false;
[[nodiscard]] fs::path corpusRoot() { return {}; }
#endif

/// Chip logs, of any of the four kinds, up to `want` of them.
[[nodiscard]] std::vector<fs::path> findLogs(std::size_t want) {
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
        if (!entry.is_regular_file(error)) {
            continue;
        }
        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension == ".vgm" || extension == ".vgz" || extension == ".s98" ||
            extension == ".dro" || extension == ".gym") {
            found.push_back(entry.path());
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

[[nodiscard]] std::vector<std::byte> drain(IDecoder& decoder, std::size_t limitBytes) {
    std::vector<std::byte> out;
    AudioChunk             chunk;
    while (out.size() < limitBytes && decoder.readAudio(chunk)) {
        const std::size_t frames = chunk.frameCount();
        if (frames == 0) {
            break;
        }
        const std::size_t bytes = frames * chunk.format().bytesPerFrame();
        const std::size_t at    = out.size();
        out.resize(at + bytes);
        std::memcpy(out.data() + at, chunk.bytes().data(), bytes);
    }
    return out;
}

/// Highest absolute sample in a packed little-endian S24 buffer.
[[nodiscard]] int peak(const std::vector<std::byte>& bytes) {
    int highest = 0;
    for (std::size_t i = 0; i + 3 <= bytes.size(); i += 3) {
        int value = static_cast<int>(static_cast<unsigned char>(bytes[i])) |
                    (static_cast<int>(static_cast<unsigned char>(bytes[i + 1])) << 8) |
                    (static_cast<int>(static_cast<unsigned char>(bytes[i + 2])) << 16);
        if ((value & 0x800000) != 0) {
            value -= 0x1000000;
        }
        highest = std::max(highest, std::abs(value));
    }
    return highest;
}

[[nodiscard]] fs::path tempDir() {
    const fs::path  dir = fs::temp_directory_path() / "xpcog-libvgm-tests";
    std::error_code error;
    fs::create_directories(dir, error);
    return dir;
}

/// CRC-32, zlib's polynomial. Fifteen lines rather than a zlib dependency in
/// the test binary.
[[nodiscard]] std::uint32_t crc32Of(const std::vector<unsigned char>& data) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const unsigned char byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

/// `data` in a gzip container, compressed not at all.
///
/// Written here rather than shelled out to gzip, so the .vgz case runs on a
/// machine that has no gzip -- which is every Windows runner. Deflate's "stored"
/// block type is what makes that cheap: a three-bit header that is then padded
/// to a byte, a length and its complement, and the bytes themselves. A stored
/// stream is a perfectly ordinary gzip file and libvgm's loader has no idea it
/// was not compressed.
[[nodiscard]] std::vector<unsigned char> gzipStored(
    const std::vector<unsigned char>& data) {
    std::vector<unsigned char> out = {
        0x1F, 0x8B,  // magic
        0x08,        // CM: deflate
        0x00,        // FLG: no name, no extra, no comment
        0x00, 0x00, 0x00, 0x00,  // MTIME: none
        0x00,        // XFL
        0xFF,        // OS: unknown
    };

    constexpr std::size_t kMaxBlock = 65535;
    std::size_t           at        = 0;
    do {
        const std::size_t take  = std::min(kMaxBlock, data.size() - at);
        const bool        final = (at + take) >= data.size();
        out.push_back(static_cast<unsigned char>(final ? 1 : 0));
        out.push_back(static_cast<unsigned char>(take & 0xFF));
        out.push_back(static_cast<unsigned char>((take >> 8) & 0xFF));
        out.push_back(static_cast<unsigned char>(~take & 0xFF));
        out.push_back(static_cast<unsigned char>((~take >> 8) & 0xFF));
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(at),
                   data.begin() + static_cast<std::ptrdiff_t>(at + take));
        at += take;
    } while (at < data.size());

    const std::uint32_t crc  = crc32Of(data);
    const auto          size = static_cast<std::uint32_t>(data.size());
    for (const std::uint32_t value : {crc, size}) {
        for (int shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<unsigned char>((value >> shift) & 0xFF));
        }
    }
    return out;
}

[[nodiscard]] std::vector<unsigned char> readFile(const fs::path& path) {
    std::vector<unsigned char> out;
    std::FILE*                 file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) {
        return out;
    }
    std::fseek(file, 0, SEEK_END);
    out.resize(static_cast<std::size_t>(std::ftell(file)));
    std::fseek(file, 0, SEEK_SET);
    const std::size_t got = std::fread(out.data(), 1, out.size(), file);
    std::fclose(file);
    out.resize(got);
    return out;
}

void writeFile(const fs::path& path, const std::vector<unsigned char>& bytes) {
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file != nullptr) {
        std::fwrite(bytes.data(), 1, bytes.size(), file);
        std::fclose(file);
    }
}

[[nodiscard]] Url writeTemp(const std::string& name, const std::string& bytes) {
    const fs::path path = tempDir() / name;
    std::FILE*     file = std::fopen(path.string().c_str(), "wb");
    if (file != nullptr) {
        std::fwrite(bytes.data(), 1, bytes.size(), file);
        std::fclose(file);
    }
    return Url::fromLocalPath(path);
}

}  // namespace

/// The OPL4 sample ROM, as codecs/libvgm/CMakeLists.txt generated it.
extern "C" {
extern const unsigned int kYrw801RomSize;
}

TEST_CASE("the OPL4 sample ROM is whole", "[libvgm]") {
    // There is no OPL4 fixture anywhere on hand, so this cannot say the chip
    // sounds right -- but it can say the 2 MB blob compiled into the binary is
    // the size a YRW801 is. The realistic failure is not the ROM being absent,
    // which is handled and warned about; it is somebody dropping a truncated or
    // wrong file into data/ and getting a wavetable that reads off the end of
    // its own samples.
    //
    // Zero is allowed and means a build without the ROM, which is a supported
    // configuration -- see requestFile() in the decoder.
    constexpr unsigned kYrw801Size = 2U * 1024U * 1024U;
    INFO("kYrw801RomSize " << kYrw801RomSize);
    CHECK((kYrw801RomSize == 0 || kYrw801RomSize == kYrw801Size));
}

TEST_CASE("the libvgm decoder claims all four of its formats", "[libvgm]") {
    CHECK(registry().isPlayableExtension("vgm"));
    CHECK(registry().isPlayableExtension("vgz"));
    CHECK(registry().isPlayableExtension("s98"));
    CHECK(registry().isPlayableExtension("dro"));
    CHECK(registry().isPlayableExtension("gym"));
}

TEST_CASE("something that is not a chip log is declined", "[libvgm]") {
    // Not just this decoder's business. `.dro` is AdPlug's too and `.vgm` is
    // Game_Music_Emu's, and this one is tried first because of its priority --
    // so a decoder that accepted whatever it was handed would take those files
    // away from the codecs that can actually read the ones it cannot.
    for (const char* name : {"garbage.vgm", "garbage.vgz", "garbage.s98",
                             "garbage.dro", "garbage.gym"}) {
        INFO(name);
        CHECK_FALSE(registry().open(writeTemp(name, "not a register log at all")));
        CHECK_FALSE(registry().open(writeTemp(name, "")));
    }
}

TEST_CASE("a chip log renders audio, not silence", "[libvgm][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_VGM_CORPUS=<path> to run this");
    }
    const auto logs = findLogs(6);
    if (logs.empty()) {
        SKIP("corpus holds no VGM, S98, DRO or GYM files");
    }

    for (const fs::path& path : logs) {
        INFO(path.filename().string());
        PluginRegistry::OpenResult opened = registry().open(Url::fromLocalPath(path));
        REQUIRE(opened);

        const TrackProperties props = opened.decoder->properties();
        CHECK(props.format.sampleRate == 44100.0);
        CHECK(props.format.channels == 2);
        // 24 bits because that is libvgm's internal mixing scale -- anything
        // wider would be padding, and 16 would throw away resolution the
        // emulators produced.
        CHECK(props.format.format == SampleFormat::S24);
        CHECK(props.format.bitsPerSample == 24);
        CHECK(props.seekable);

        // Which decoder actually ran. Game_Music_Emu and vgmstream both claim
        // `.vgm`, and either of them opening one of these would report its own
        // name here -- Game_Music_Emu says "Sega MegaCD / SegaCD" for the very
        // first file in this corpus.
        INFO("codec " << props.codec);
        CHECK(props.codec.view().starts_with("VGM v"));

        const std::vector<std::byte> audio =
            drain(*opened.decoder, 44100 * 6 * 4);  // about four seconds
        REQUIRE_FALSE(audio.empty());
        // The clamp libvgm applies is +/-0x800000, so there is a real ceiling
        // rather than a guess, and the floor separates "it parsed" from "it
        // played the chip".
        const int level = peak(audio);
        INFO("peak " << level);
        CHECK(level > 1000);
        CHECK(level <= 0x800000);
    }
}

TEST_CASE("the reported length is the audio that comes out", "[libvgm][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_VGM_CORPUS=<path> to run this");
    }
    const auto logs = findLogs(64);
    if (logs.empty()) {
        SKIP("corpus holds no VGM, S98, DRO or GYM files");
    }

    // Deliberately a short one: this decodes to the end, and the point is the
    // agreement rather than the sample count.
    const fs::path* shortest = nullptr;
    std::int64_t    best     = 0;
    for (const fs::path& path : logs) {
        PluginRegistry::OpenResult opened = registry().open(Url::fromLocalPath(path));
        if (!opened) {
            continue;
        }
        const std::int64_t frames = opened.decoder->properties().totalFrames;
        if (frames > 0 && (shortest == nullptr || frames < best)) {
            shortest = &path;
            best     = frames;
        }
    }
    if (shortest == nullptr) {
        SKIP("corpus holds no chip log with a length");
    }
    INFO(shortest->filename().string());

    PluginRegistry::OpenResult opened = registry().open(Url::fromLocalPath(*shortest));
    REQUIRE(opened);
    const TrackProperties props = opened.decoder->properties();

    // Cog reports Tick2Second(GetTotalTicks()), which is one pass and no more,
    // while PlayerA goes on to play the loops, fade over the configured seconds
    // and add half a second of silence. This is GetTotalTime() with the loop,
    // fade and silence flags -- the number PlayerA is actually working to -- so
    // the two agree exactly rather than approximately.
    const std::vector<std::byte> audio =
        drain(*opened.decoder, static_cast<std::size_t>(-1));
    const auto delivered =
        static_cast<std::int64_t>(audio.size() / props.format.bytesPerFrame());
    CHECK(delivered == props.totalFrames);
}

TEST_CASE("a gzipped log is the same log", "[libvgm][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_VGM_CORPUS=<path> to run this");
    }
    const auto logs = findLogs(1);
    if (logs.empty()) {
        SKIP("corpus holds no chip logs");
    }

    // `.vgz` is a gzipped `.vgm` and nothing else. libvgm's loader inflates it
    // in passing, which is easy to believe and easy to have wrong: this codec
    // hands the loader a memory buffer rather than a filename, and a loader
    // that only sniffed gzip when reading from disk would fail here and
    // nowhere else.
    const std::vector<unsigned char> raw = readFile(logs.front());
    REQUIRE_FALSE(raw.empty());

    const fs::path plain  = tempDir() / "roundtrip.vgm";
    const fs::path packed = tempDir() / "roundtrip.vgz";
    writeFile(plain, raw);
    writeFile(packed, gzipStored(raw));

    PluginRegistry::OpenResult first = registry().open(Url::fromLocalPath(plain));
    PluginRegistry::OpenResult second = registry().open(Url::fromLocalPath(packed));
    REQUIRE(first);
    REQUIRE(second);
    CHECK(second.decoder->properties().totalFrames ==
          first.decoder->properties().totalFrames);

    constexpr std::size_t kTwoSeconds = 44100 * 6 * 2;
    const std::vector<std::byte> plainAudio  = drain(*first.decoder, kTwoSeconds);
    const std::vector<std::byte> packedAudio = drain(*second.decoder, kTwoSeconds);
    REQUIRE_FALSE(plainAudio.empty());
    CHECK(packedAudio == plainAudio);
}

TEST_CASE("the format is decided by content, not by extension", "[libvgm][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_VGM_CORPUS=<path> to run this");
    }
    const auto logs = findLogs(1);
    if (logs.empty()) {
        SKIP("corpus holds no chip logs");
    }

    // A VGM under a `.dro` name. The extension gets the file offered to this
    // decoder -- along with AdPlug and vgmstream, both of which also claim
    // `.dro` -- and libvgm's own player selection is what identifies it. Worth
    // pinning because the four players are chosen by sniffing, and a decoder
    // that trusted the extension would hand a VGM to the DRO player.
    const fs::path renamed = tempDir() / "actually-a-vgm.dro";
    std::error_code error;
    fs::remove(renamed, error);
    fs::copy_file(logs.front(), renamed, fs::copy_options::overwrite_existing, error);
    if (error) {
        SKIP("could not copy a fixture into place");
    }

    PluginRegistry::OpenResult opened = registry().open(Url::fromLocalPath(renamed));
    REQUIRE(opened);
    CHECK(opened.decoder->properties().codec.view().starts_with("VGM v"));
}

TEST_CASE("a chip log carries its game's tags", "[libvgm][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_VGM_CORPUS=<path> to run this");
    }
    const auto logs = findLogs(24);
    if (logs.empty()) {
        SKIP("corpus holds no chip logs");
    }

    bool titled  = false;
    bool systemed = false;
    for (const fs::path& path : logs) {
        PluginRegistry::OpenResult opened = registry().open(Url::fromLocalPath(path));
        if (!opened) {
            continue;
        }
        const MetadataMap tags = opened.decoder->metadata();
        titled   = titled || !tags.first("title").empty();
        // Not in Cog's mapping. A VGM's SYSTEM tag names the machine the log was
        // taken from -- "Sega MegaCD", "NEC PC-8801" -- which for a shelf of
        // chip logs is the field a listener actually sorts by, and dropping it
        // loses the only thing distinguishing two rips of the same tune.
        systemed = systemed || !tags.first("genre").empty();
    }

    // Not per file: a VGM's tag block is optional and plenty of rips have none.
    CHECK(titled);
    CHECK(systemed);
}

TEST_CASE("seeking lands where it was asked to", "[libvgm][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_VGM_CORPUS=<path> to run this");
    }
    const auto logs = findLogs(8);
    if (logs.empty()) {
        SKIP("corpus holds no chip logs");
    }

    for (const fs::path& path : logs) {
        PluginRegistry::OpenResult opened = registry().open(Url::fromLocalPath(path));
        REQUIRE(opened);
        constexpr std::int64_t kTarget = 44100 * 3;
        if (opened.decoder->properties().totalFrames <= kTarget * 2) {
            continue;
        }
        INFO(path.filename().string());

        // A register log has no random access either -- the chip's state at any
        // moment is every write before it -- but unlike a tracker, libvgm does
        // the replaying itself, inside PlayerA::Seek. So what can be checked is
        // that it stops where it was told and still plays afterwards.
        CHECK(opened.decoder->seek(kTarget) == kTarget);
        const std::vector<std::byte> after = drain(*opened.decoder, 44100 * 6);
        REQUIRE_FALSE(after.empty());
        CHECK(peak(after) > 0);

        // And backwards, which is a full re-run of the log.
        CHECK(opened.decoder->seek(0) == 0);
        const std::vector<std::byte> restarted = drain(*opened.decoder, 44100 * 6);
        CHECK_FALSE(restarted.empty());
        return;
    }
    SKIP("corpus holds no chip log long enough to seek three seconds into");
}
