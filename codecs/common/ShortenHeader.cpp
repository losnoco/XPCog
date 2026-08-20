// See ShortenHeader.hpp for why this exists.
//
// The bitstream is Tony Robinson's, and the field order below is decode.c's in
// Cog Frameworks/Shorten (which is xmms-shn, which is shorten 3.x). It is read
// far enough to reach the first command and no further -- the audio residuals
// that follow are FFmpeg's business.

#include "ShortenHeader.hpp"

#include <cstring>
#include <string_view>
#include <vector>

namespace xpcog::codecs {
namespace {

/// Shorten's own names, from Frameworks/Shorten/Files/shorten/include/shorten.h.
constexpr std::string_view kMagic = "ajkg";
/// Version 3 is the last one anybody wrote and the last the reference decoder
/// reads; higher numbers exist in the format's own constants but never in files.
constexpr int kMaxSupportedVersion = 3;

constexpr int kUlongSize          = 2;   // ULONGSIZE
constexpr int kXByteSize          = 7;   // XBYTESIZE
constexpr int kFnSize             = 2;   // FNSIZE
constexpr int kVerbatimCkSizeSize = 5;   // VERBATIM_CKSIZE_SIZE
constexpr int kVerbatimByteSize   = 8;   // VERBATIM_BYTE_SIZE

constexpr int kFnVerbatim = 9;  // FN_VERBATIM

/// A WAV header is 44 bytes and a verbatim chunk holds at most 256, but a file
/// written by something that put a LIST or fact chunk in front of the data can
/// need more than one chunk. This is the ceiling on what will be gathered before
/// giving up -- far more than any real header and far less than a denial of
/// service.
constexpr std::size_t kMaxHeaderBytes = 4096;

/// An MSB-first bit reader.
///
/// The reference decoder pulls 32-bit big-endian words and consumes them from
/// the top bit down, which is the same sequence of bits as reading the byte
/// stream MSB-first -- so this is that, without the word buffer. Running off
/// the end is sticky and reads as zero, which the caller checks for once rather
/// than after every field.
class BitReader {
public:
    explicit BitReader(std::span<const std::byte> data) : data_(data) {}

    [[nodiscard]] unsigned bits(int count) {
        unsigned value = 0;
        for (int i = 0; i < count; ++i) {
            value = (value << 1) | bit();
        }
        return value;
    }

    /// uvar_get(): a unary prefix -- zero bits until a one -- shifted up by
    /// `count` and then that many more bits appended.
    [[nodiscard]] unsigned uvar(int count) {
        unsigned high = 0;
        while (bit() == 0) {
            ++high;
            // A run this long is not a number any encoder wrote; it is a file
            // of zeros being read as a header.
            if (high > 64 || exhausted_) {
                exhausted_ = true;
                return 0;
            }
        }
        return (high << count) | bits(count);
    }

    /// ulong_get(): a uvar naming a bit count, then a uvar of that width.
    [[nodiscard]] unsigned ulong() {
        const unsigned width = uvar(kUlongSize);
        if (width > 32) {
            exhausted_ = true;
            return 0;
        }
        return uvar(static_cast<int>(width));
    }

    [[nodiscard]] bool exhausted() const { return exhausted_; }

private:
    [[nodiscard]] unsigned bit() {
        if (at_ >= data_.size() * 8) {
            exhausted_ = true;
            return 0;
        }
        const std::size_t index  = at_ / 8;
        const unsigned    offset = 7 - static_cast<unsigned>(at_ % 8);
        ++at_;
        return (static_cast<unsigned char>(data_[index]) >> offset) & 1U;
    }

    std::span<const std::byte> data_;
    std::size_t                at_        = 0;
    bool                       exhausted_ = false;
};

[[nodiscard]] std::uint32_t readLe16(const unsigned char* at) {
    return static_cast<std::uint32_t>(at[0]) | (static_cast<std::uint32_t>(at[1]) << 8);
}
[[nodiscard]] std::uint32_t readLe32(const unsigned char* at) {
    return readLe16(at) | (readLe16(at + 2) << 16);
}

/// Walks the RIFF chunks for `fmt ` and `data`.
///
/// The `data` chunk's *declared* size is what is wanted, not what follows it --
/// there is nothing following it, because everything after this header was
/// compressed away.
[[nodiscard]] bool parseWaveHeader(const std::vector<unsigned char>& header,
                                   ShortenLength&                    out) {
    if (header.size() < 12 || std::memcmp(header.data(), "RIFF", 4) != 0 ||
        std::memcmp(header.data() + 8, "WAVE", 4) != 0) {
        return false;
    }

    bool        haveFormat = false;
    std::size_t at         = 12;
    while (at + 8 <= header.size()) {
        const unsigned char* id   = header.data() + at;
        const std::uint32_t  size = readLe32(header.data() + at + 4);
        at += 8;

        if (std::memcmp(id, "fmt ", 4) == 0 && size >= 16 && at + 16 <= header.size()) {
            out.channels      = readLe16(header.data() + at + 2);
            out.sampleRate    = readLe32(header.data() + at + 4);
            out.bitsPerSample = readLe16(header.data() + at + 14);
            haveFormat        = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            if (!haveFormat || out.channels == 0 || out.bitsPerSample < 8) {
                return false;
            }
            const std::uint32_t bytesPerFrame = out.channels * (out.bitsPerSample / 8);
            out.frames = static_cast<std::int64_t>(size / bytesPerFrame);
            // A `data` chunk of zero bytes is a header written before the
            // recording it describes; there is no length in it.
            return out.frames > 0;
        }

        // Chunks are padded to an even length.
        at += size + (size & 1U);
    }
    return false;
}

}  // namespace

std::optional<ShortenLength> readShortenLength(std::span<const std::byte> data) {
    if (data.size() < kMagic.size() + 1 ||
        std::memcmp(data.data(), kMagic.data(), kMagic.size()) != 0) {
        return std::nullopt;
    }
    const auto version = static_cast<int>(data[kMagic.size()]);
    // Version 0 uses a different width for every header field and predates the
    // WAV wrapping this is here to read, so it is declined rather than guessed
    // at. Nothing in circulation is version 0; shorten 1.x wrote it in 1994.
    if (version < 1 || version > kMaxSupportedVersion) {
        return std::nullopt;
    }

    BitReader in{data.subspan(kMagic.size() + 1)};

    // The file type names the *sample* format -- TYPE_S16LH for the ordinary
    // 16-bit little-endian case -- and not the container, so it says nothing
    // about whether a WAV header was preserved. TYPE_RIFF_WAVE exists in the
    // constants and shorten's encoder never writes it; xmms-shn does not test
    // it either, it just parses whatever the verbatim chunk turns out to hold.
    (void)in.ulong();  // internal_ftype
    const unsigned channels = in.ulong();
    (void)in.ulong();  // blocksize
    (void)in.ulong();  // maxnlpc, the LPC order
    (void)in.ulong();  // nmean, the running-mean window
    const unsigned skip = in.ulong();
    // The "skip" bytes are arbitrary data the encoder was asked to carry; they
    // are coded one per uvar and are not part of anything read here.
    if (skip > kMaxHeaderBytes) {
        return std::nullopt;
    }
    for (unsigned i = 0; i < skip; ++i) {
        (void)in.uvar(kXByteSize);
    }
    if (in.exhausted() || channels == 0) {
        return std::nullopt;
    }

    // The wrapped header, gathered from however many verbatim chunks it takes.
    //
    // Anything but FN_VERBATIM as the first command is a file whose header was
    // not preserved -- shorten can wrap AIFF or raw samples too, and can be
    // told not to keep the header at all -- and there is no length in one of
    // those. parseWaveHeader() is what decides; this only collects.
    // Running off the end of the buffer here is ordinary rather than an error:
    // what follows the header is audio, and the caller only handed over the
    // first kilobyte of the file. So exhaustion ends the gathering instead of
    // failing it -- but a chunk that was only half read is dropped, because a
    // partial header is worse than none.
    std::vector<unsigned char> header;
    while (true) {
        if (static_cast<int>(in.uvar(kFnSize)) != kFnVerbatim || in.exhausted()) {
            break;
        }
        const unsigned length = in.uvar(kVerbatimCkSizeSize);
        if (in.exhausted() || length == 0 ||
            header.size() + length > kMaxHeaderBytes) {
            break;
        }

        const std::size_t before = header.size();
        for (unsigned i = 0; i < length; ++i) {
            header.push_back(static_cast<unsigned char>(in.uvar(kVerbatimByteSize)));
        }
        if (in.exhausted()) {
            header.resize(before);
            break;
        }
    }
    if (header.empty()) {
        return std::nullopt;
    }

    ShortenLength found;
    if (!parseWaveHeader(header, found)) {
        return std::nullopt;
    }
    // The header the file carries and the header the stream declares should
    // agree; where they do not, the file is not describing itself and its
    // length claim is not worth reporting.
    if (found.channels != channels) {
        return std::nullopt;
    }
    return found;
}

}  // namespace xpcog::codecs
