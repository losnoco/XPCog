// Shorten (.shn), and specifically the one thing about it that is ours.
//
// FFmpeg demuxes and decodes shorten, so the decoder is not in question here --
// what is, is the length. Shorten states it only in the WAV header it wraps,
// FFmpeg's demuxer does not read that header, and without it a lossless music
// file opens showing 0:00 and cannot be scrubbed. So codecs/common parses the
// first kilobyte to recover it, and this is what says that parse is right.
//
// The parse is checkable without any fixture at all, because a shorten header
// can be *written*: the bitstream is a unary-plus-k-bits code, and writing it
// is the same twenty lines as reading it. The synthetic file below carries a
// WAV header whose data size is chosen so the expected frame count can be
// written down rather than derived, and every rejection case is a one-line
// change to it.
//
// Two things this deliberately does not claim. It does not claim the length is
// the number of frames that will come out -- for a truncated file it is not,
// and reporting what the file says about itself is the right answer either way.
// And it does not test shorten decoding, which is FFmpeg's and is covered by
// FFmpeg.

#include "common/ShortenHeader.hpp"

#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
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

// --- writing a shorten header ----------------------------------------------

/// The mirror of the reader: bits go out most-significant first, and a `uvar`
/// is `value >> k` zeros, a one, and then the low k bits.
class BitWriter {
public:
    void bits(unsigned value, int count) {
        for (int i = count - 1; i >= 0; --i) {
            bit((value >> i) & 1U);
        }
    }
    void uvar(unsigned value, int k) {
        for (unsigned i = 0; i < (value >> k); ++i) {
            bit(0);
        }
        bit(1);
        bits(value, k);
    }
    /// ulong: a uvar naming how many bits the value needs, then the value in
    /// that many bits.
    void ulong(unsigned value) {
        int width = 0;
        while ((value >> width) != 0) {
            ++width;
        }
        uvar(static_cast<unsigned>(width), 2);
        uvar(value, width);
    }

    [[nodiscard]] std::vector<std::byte> take() {
        // Pad the last byte with zeros. The reader stops at the verbatim chunk
        // and never looks at them.
        while (pending_ != 0) {
            bit(0);
        }
        return std::move(out_);
    }

private:
    void bit(unsigned value) {
        current_ = static_cast<unsigned char>((current_ << 1) | (value & 1U));
        pending_ = (pending_ + 1) % 8;
        if (pending_ == 0) {
            out_.push_back(static_cast<std::byte>(current_));
            current_ = 0;
        }
    }

    std::vector<std::byte> out_;
    unsigned char          current_ = 0;
    int                    pending_ = 0;
};

void putLe16(std::vector<unsigned char>& out, std::uint32_t value) {
    out.push_back(static_cast<unsigned char>(value & 0xFF));
    out.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
}
void putLe32(std::vector<unsigned char>& out, std::uint32_t value) {
    putLe16(out, value & 0xFFFF);
    putLe16(out, value >> 16);
}
void putTag(std::vector<unsigned char>& out, const char* tag) {
    out.insert(out.end(), tag, tag + 4);
}

/// A 44-byte canonical RIFF/WAVE header.
[[nodiscard]] std::vector<unsigned char> waveHeader(std::uint32_t channels,
                                                    std::uint32_t sampleRate,
                                                    std::uint32_t bits,
                                                    std::uint32_t dataBytes) {
    const std::uint32_t blockAlign = channels * (bits / 8);
    std::vector<unsigned char> header;
    putTag(header, "RIFF");
    putLe32(header, 36 + dataBytes);
    putTag(header, "WAVE");
    putTag(header, "fmt ");
    putLe32(header, 16);
    putLe16(header, 1);  // PCM
    putLe16(header, channels);
    putLe32(header, sampleRate);
    putLe32(header, sampleRate * blockAlign);
    putLe16(header, blockAlign);
    putLe16(header, bits);
    putTag(header, "data");
    putLe32(header, dataBytes);
    return header;
}

struct ShnOptions {
    std::string   magic       = "ajkg";
    int           version     = 2;
    unsigned      fileType    = 5;  // TYPE_S16LH, what a 16-bit WAV encodes as
    unsigned      channels    = 2;
    unsigned      command     = 9;  // FN_VERBATIM
    std::vector<unsigned char> header = waveHeader(2, 44100, 16, 40 * 4);
};

/// A shorten file's header and first command, and nothing after it: no audio,
/// which is exactly as much as the length parser reads.
[[nodiscard]] std::vector<std::byte> makeShn(const ShnOptions& options) {
    BitWriter bits;
    bits.ulong(options.fileType);
    bits.ulong(options.channels);
    bits.ulong(256);  // blocksize
    bits.ulong(0);    // maxnlpc
    bits.ulong(0);    // nmean
    bits.ulong(0);    // nskip

    bits.uvar(options.command, 2);  // FNSIZE
    bits.uvar(static_cast<unsigned>(options.header.size()), 5);
    for (const unsigned char byte : options.header) {
        bits.uvar(byte, 8);
    }

    std::vector<std::byte> out;
    for (const char c : options.magic) {
        out.push_back(static_cast<std::byte>(c));
    }
    out.push_back(static_cast<std::byte>(options.version));
    const std::vector<std::byte> body = bits.take();
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

#ifdef XPCOG_SHORTEN_FILE
constexpr bool kHaveFile = true;
[[nodiscard]] fs::path shortenFile() { return fs::path{XPCOG_SHORTEN_FILE}; }
#else
constexpr bool kHaveFile = false;
[[nodiscard]] fs::path shortenFile() { return {}; }
#endif

}  // namespace

TEST_CASE("the length is read out of the wrapped WAV header", "[shorten]") {
    // 40 frames of 16-bit stereo is 160 bytes, and that is the number the
    // header carries -- so the answer is 40 and nothing about it is derived
    // from the size of the file, which holds no audio at all.
    const auto file  = makeShn(ShnOptions{});
    const auto found = codecs::readShortenLength(file);

    REQUIRE(found.has_value());
    CHECK(found->frames == 40);
    CHECK(found->channels == 2);
    CHECK(found->sampleRate == 44100);
    CHECK(found->bitsPerSample == 16);
}

TEST_CASE("the length survives an unusual format", "[shorten]") {
    // Six channels at 24 bits: 18 bytes a frame. Live recordings in shorten are
    // overwhelmingly 16-bit stereo, which is exactly why the arithmetic wants a
    // case that is not.
    ShnOptions options;
    options.channels = 6;
    options.header   = waveHeader(6, 48000, 24, 100 * 18);

    const auto found = codecs::readShortenLength(makeShn(options));
    REQUIRE(found.has_value());
    CHECK(found->frames == 100);
    CHECK(found->channels == 6);
    CHECK(found->sampleRate == 48000);
    CHECK(found->bitsPerSample == 24);
}

TEST_CASE("anything that is not a shorten header reads as no length",
          "[shorten]") {
    CHECK_FALSE(codecs::readShortenLength(std::span<const std::byte>{}).has_value());

    {
        ShnOptions options;
        options.magic = "ajkh";  // one letter out
        CHECK_FALSE(codecs::readShortenLength(makeShn(options)).has_value());
    }
    {
        // Version 0 is shorten 1.x, from 1994: every header field has its own
        // fixed width there rather than the self-describing ulong used since,
        // so reading it with this parser would produce numbers rather than an
        // error. Declined instead.
        ShnOptions options;
        options.version = 0;
        CHECK_FALSE(codecs::readShortenLength(makeShn(options)).has_value());
    }
    {
        ShnOptions options;
        options.version = 4;  // above anything the format ever shipped
        CHECK_FALSE(codecs::readShortenLength(makeShn(options)).has_value());
    }
    {
        // A file that kept no header. shorten can be told to discard it, and
        // can wrap AIFF or raw samples instead -- in all of those the first
        // command is a data command and there is no length to find.
        ShnOptions options;
        options.command = 3;  // FN_DIFF1
        CHECK_FALSE(codecs::readShortenLength(makeShn(options)).has_value());
    }
    {
        // A verbatim chunk that is not RIFF at all.
        ShnOptions options;
        options.header = std::vector<unsigned char>(44, 'x');
        CHECK_FALSE(codecs::readShortenLength(makeShn(options)).has_value());
    }
    {
        // A header whose channel count contradicts the stream's. One of the two
        // is wrong and there is no way to tell which, so neither is reported.
        ShnOptions options;
        options.channels = 2;
        options.header   = waveHeader(1, 44100, 16, 160);
        CHECK_FALSE(codecs::readShortenLength(makeShn(options)).has_value());
    }
    {
        // Truncated partway through the wrapped header.
        const auto file = makeShn(ShnOptions{});
        REQUIRE(file.size() > 12);
        CHECK_FALSE(codecs::readShortenLength(
                        std::span<const std::byte>{file.data(), 12})
                        .has_value());
    }
}

TEST_CASE("a run of zeros is not a shorten header", "[shorten]") {
    // The unary prefix is "count zeros until a one", so a file of zero bytes
    // that happened to start with the magic would otherwise be counted through
    // to the end of the buffer, several times over. Bounded rather than fast.
    std::vector<std::byte> zeros(512, std::byte{0});
    std::memcpy(zeros.data(), "ajkg\x02", 5);
    CHECK_FALSE(codecs::readShortenLength(zeros).has_value());
}

TEST_CASE("the decoder claims .shn", "[shorten]") {
    // Through FFmpeg rather than a dedicated codec: Cog carries six thousand
    // lines of xmms-shn for this, whose reader takes a filesystem path and
    // therefore cannot read a .shn out of an archive or over HTTP. FFmpeg's
    // reads through an AVIO context, which can.
    CHECK(registry().isPlayableExtension("shn"));
}

TEST_CASE("a real shorten file decodes and knows its length", "[shorten][corpus]") {
    if (!kHaveFile || !fs::exists(shortenFile())) {
        SKIP("no file: configure with -DXPCOG_SHORTEN_FILE=<path to a .shn>");
    }

    PluginRegistry::OpenResult opened =
        registry().open(Url::fromLocalPath(shortenFile()));
    REQUIRE(opened);

    const TrackProperties props = opened.decoder->properties();
    CHECK(props.codec == "Shorten");
    CHECK(props.lossless);
    CHECK(props.format.channels > 0);
    CHECK(props.format.sampleRate > 0.0);

    // The number this whole file exists for. Without the header parse it is
    // zero, and zero is what a stream of unknown length reports -- so this is
    // the assertion that separates "FFmpeg opened it" from "we know how long
    // it is".
    CHECK(props.totalFrames > 0);

    AudioChunk chunk;
    REQUIRE(opened.decoder->readAudio(chunk));
    CHECK(chunk.frameCount() > 0);

    // And the audio is audio. A shorten stream decoded with the wrong bit shift
    // or the wrong prediction order still produces samples; it produces the
    // wrong ones, and silence is the shape that failure most often takes.
    const auto*  samples = reinterpret_cast<const float*>(chunk.bytes().data());
    const std::size_t count = chunk.frameCount() * chunk.format().channels;
    bool              heard = false;
    for (std::size_t i = 0; i < count; ++i) {
        heard = heard || samples[i] != 0.0F;
    }
    CHECK(heard);
}
