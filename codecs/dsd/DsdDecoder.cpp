// DSD files: DSF and DSDIFF.
//
// One-bit audio at 2.8 MHz and up, as it comes off an SACD. The decoding is
// already here -- vendor/dsd2pcm does the eight-to-one decimation and
// AudioConverter runs one filter per channel ahead of everything else, which is
// what a DSD `.wv` has been going through since M6. What was missing is the two
// containers the format actually ships in.
//
// ---------------------------------------------------------------------------
// Why this is not part of codecs/ffmpeg, which is where the plan put it
// ---------------------------------------------------------------------------
// Cog reads both through FFmpeg, and not through FFmpeg's DSD *decoders*: it
// matches AV_CODEC_ID_DSD_LSBF and friends, sets `rawDSD`, and hands the
// packets on untouched so its own filter does the work (FFMPEGDecoder.m:259).
// Copying that shape was the plan here too, and two things stopped it.
//
// **FFmpeg cannot read DSDIFF at all.** It has a `dsf` demuxer and nothing for
// `.dff` -- checked against the format's own list rather than assumed. Cog's
// FFmpeg plugin claims `dff`, `dsdiff` and `wsd` regardless, which is a claim
// its demuxer cannot honour. So half of this needed a reader written here
// whatever else was decided.
//
// **XPCOG_WITH_FFMPEG is off by default.** Putting `.dsf` behind it would have
// made one of the two containers depend on an optional and very large
// dependency while the other did not, which is not a distinction anybody
// listening to an SACD rip would be able to predict.
//
// Both containers are simple -- a header and a run of bytes -- so they are read
// here, and the one convention that matters about DSD lives in one file rather
// than two.
//
// ---------------------------------------------------------------------------
// The convention, which is WavPack's
// ---------------------------------------------------------------------------
// The *byte* rate is reported, not the 2.8 MHz bit rate, and the format is
// SampleFormat::DSD with one bit per sample. One frame is one byte per channel.
// That keeps duration, seeking and every frame count in the engine true, and
// the filter downstream turns each byte into exactly one float. Cog instead
// reports the native bit rate and multiplies its frame counts by eight
// everywhere. See codecs/wavpack, which established this here.
//
// Bit order is the other half of it. dsd2pcm walks each byte from 0x80 down, so
// it wants MSB first. DSDIFF stores MSB first and interleaved, and feeds
// straight through; DSF stores LSB first in per-channel blocks, and needs both
// undone. Getting either wrong is not silence -- it is a full-scale rasp, since
// a reversed one-bit stream is still a legal one-bit stream.

#include "common/Id3v2.hpp"
#include "common/TextEncoding.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog {
namespace {

constexpr std::size_t kFramesPerRead = 16384;

/// Every byte with its bits in the opposite order, for DSF.
///
/// Built rather than written out: 256 lines of hex is 256 chances to transpose
/// two of them, and a table that is wrong in one entry produces audio that is
/// right except for a rasp on one sample value in 256.
constexpr std::array<std::uint8_t, 256> kReversedBits = [] {
    std::array<std::uint8_t, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        auto value = static_cast<std::uint8_t>(i);
        std::uint8_t reversed = 0;
        for (int bit = 0; bit < 8; ++bit) {
            reversed = static_cast<std::uint8_t>((reversed << 1) | (value & 1U));
            value    = static_cast<std::uint8_t>(value >> 1);
        }
        table[i] = reversed;
    }
    return table;
}();

[[nodiscard]] std::uint32_t readU32Le(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

[[nodiscard]] std::uint64_t readU64Le(const std::uint8_t* p) {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | p[static_cast<std::size_t>(i)];
    }
    return value;
}

[[nodiscard]] std::uint64_t readU64Be(const std::uint8_t* p) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | p[static_cast<std::size_t>(i)];
    }
    return value;
}

[[nodiscard]] bool readExactly(ISource& source, void* into, std::size_t bytes) {
    auto* out = static_cast<std::uint8_t*>(into);
    std::size_t got = 0;
    while (got < bytes) {
        const std::int64_t read =
            source.read(out + got, static_cast<std::int64_t>(bytes - got));
        if (read <= 0) {
            return false;
        }
        got += static_cast<std::size_t>(read);
    }
    return true;
}

/// The channel mask for a DSD file's channel count.
///
/// Both containers describe their layout -- DSF by a channel-type enumeration,
/// DSDIFF by four-character channel ids -- and both agree with the ordinary
/// mask for everything up to 5.1. Rather than two partial translations, the
/// count is handed to the one place that already answers this question.
[[nodiscard]] std::uint32_t maskForChannels(std::uint32_t channels) {
    return guessChannelConfig(channels);
}

// ---------------------------------------------------------------------------
// The container readers
// ---------------------------------------------------------------------------

/// What both containers present to the decoder: interleaved, MSB-first bytes,
/// one per channel per frame, which is exactly what AudioConverter's decimation
/// filter takes.
class DsdReader {
public:
    virtual ~DsdReader() = default;

    [[nodiscard]] virtual std::size_t read(std::uint8_t* out, std::size_t frames) = 0;
    virtual bool                      seek(std::int64_t frame)                    = 0;

    [[nodiscard]] std::uint32_t channels() const { return channels_; }
    [[nodiscard]] double        byteRate() const { return byteRate_; }
    [[nodiscard]] std::int64_t  totalFrames() const { return totalFrames_; }
    [[nodiscard]] const MetadataMap& tags() const { return tags_; }

protected:
    std::uint32_t channels_    = 0;
    double        byteRate_    = 0.0;
    std::int64_t  totalFrames_ = 0;
    MetadataMap   tags_;
};

/// DSF: Sony's container, and the one FFmpeg can read.
///
/// Two things about its data layout have to be undone. It is stored in blocks
/// of `blockSize` bytes *per channel* -- all of channel 0, then all of channel
/// 1, then the next block -- and each byte holds its eight one-bit samples
/// least significant first. The block size is 4096 in every file anyone has
/// seen, and the last block is zero-padded past the stated sample count.
class DsfReader final : public DsdReader {
public:
    [[nodiscard]] bool open(ISource& source) {
        source_ = &source;
        if (!source.seekable() || !source.seek(0, SEEK_SET)) {
            return false;
        }

        std::array<std::uint8_t, 28> header{};
        if (!readExactly(source, header.data(), header.size()) ||
            std::memcmp(header.data(), "DSD ", 4) != 0) {
            return false;
        }
        const std::uint64_t id3Offset = readU64Le(header.data() + 20);

        std::array<std::uint8_t, 52> fmt{};
        if (!readExactly(source, fmt.data(), fmt.size()) ||
            std::memcmp(fmt.data(), "fmt ", 4) != 0) {
            return false;
        }

        // Only format id 0 is defined, and it means raw DSD. Anything else is a
        // file this reader would misinterpret rather than fail on.
        if (readU32Le(fmt.data() + 16) != 0) {
            return false;
        }

        channels_ = readU32Le(fmt.data() + 24);
        const std::uint32_t bitRate       = readU32Le(fmt.data() + 28);
        const std::uint32_t bitsPerSample = readU32Le(fmt.data() + 32);
        const std::uint64_t bitsPerChannel = readU64Le(fmt.data() + 36);
        blockSize_                        = readU32Le(fmt.data() + 44);

        if (channels_ == 0 || channels_ > 8 || bitRate == 0 || blockSize_ == 0 ||
            (bitsPerSample != 1 && bitsPerSample != 8)) {
            return false;
        }
        // 1 means the eight samples in a byte run least significant first, 8
        // means most significant first. Every file in the wild says 1.
        lsbFirst_ = (bitsPerSample == 1);

        byteRate_    = static_cast<double>(bitRate) / 8.0;
        totalFrames_ = static_cast<std::int64_t>(bitsPerChannel / 8);

        std::array<std::uint8_t, 12> data{};
        if (!readExactly(source, data.data(), data.size()) ||
            std::memcmp(data.data(), "data", 4) != 0) {
            return false;
        }
        dataOffset_ = source.tell();

        readId3(source, id3Offset);
        return seek(0);
    }

    [[nodiscard]] std::size_t read(std::uint8_t* out, std::size_t frames) override {
        std::size_t produced = 0;
        while (produced < frames) {
            if (blockPos_ == blockFrames_ && !fillBlock()) {
                break;
            }

            const auto take = static_cast<std::size_t>(
                std::min<std::int64_t>(static_cast<std::int64_t>(frames - produced),
                                       blockFrames_ - blockPos_));
            if (take == 0) {
                break;
            }

            // Planar to interleaved, and the bit reversal folded into the same
            // pass: two walks over a megabyte per block would cost more than
            // the filter that follows.
            const auto channels = static_cast<std::size_t>(channels_);
            for (std::size_t frame = 0; frame < take; ++frame) {
                const std::size_t at = static_cast<std::size_t>(blockPos_) + frame;
                for (std::size_t channel = 0; channel < channels; ++channel) {
                    const std::uint8_t byte =
                        block_[(channel * blockSize_) + at];
                    out[((produced + frame) * channels) + channel] =
                        lsbFirst_ ? kReversedBits[byte] : byte;
                }
            }

            produced += take;
            blockPos_ += static_cast<std::int64_t>(take);
        }
        return produced;
    }

    bool seek(std::int64_t frame) override {
        frame = std::clamp<std::int64_t>(frame, 0, totalFrames_);

        const std::int64_t blockIndex = frame / blockSize_;
        blockPos_    = frame % blockSize_;
        blockFrames_ = 0;

        const std::int64_t offset =
            dataOffset_ + (blockIndex * blockSize_ * channels_);
        if (!source_->seek(offset, SEEK_SET)) {
            return false;
        }
        framePos_ = blockIndex * blockSize_;
        // The block the position lands inside still has to be loaded, and
        // blockPos_ survives fillBlock() because it is where reading resumes.
        const std::int64_t within = blockPos_;
        if (!fillBlock()) {
            return frame == totalFrames_;  // seeking to the end is not a failure
        }
        blockPos_ = within;
        return true;
    }

private:
    /// Reads one block of every channel, clamped to the stated sample count.
    [[nodiscard]] bool fillBlock() {
        if (framePos_ >= totalFrames_) {
            return false;
        }
        block_.resize(static_cast<std::size_t>(blockSize_) * channels_);
        if (!readExactly(*source_, block_.data(), block_.size())) {
            return false;
        }

        // The final block is padded with zeroes past the end of the audio, and
        // a DSD zero byte is not silence -- it is full-scale negative. Playing
        // the padding is a click at the end of every file.
        blockFrames_ = std::min<std::int64_t>(blockSize_, totalFrames_ - framePos_);
        blockPos_    = 0;
        framePos_ += blockFrames_;
        return blockFrames_ > 0;
    }

    void readId3(ISource& source, std::uint64_t offset) {
        if (offset == 0 || !source.seek(static_cast<std::int64_t>(offset), SEEK_SET)) {
            return;
        }
        // Bounded: a corrupt pointer must not turn into an allocation the size
        // of the file, and no ID3v2 tag a music file carries is larger.
        constexpr std::size_t kMaxTag = 8U * 1024U * 1024U;
        std::vector<std::byte> tag(kMaxTag);
        const std::int64_t     got =
            source.read(tag.data(), static_cast<std::int64_t>(tag.size()));
        if (got <= 0) {
            return;
        }
        tag.resize(static_cast<std::size_t>(got));
        codecs::parseId3v2(tag, tags_);
    }

    ISource* source_ = nullptr;

    std::int64_t dataOffset_ = 0;
    std::uint32_t blockSize_ = 0;
    bool          lsbFirst_  = true;

    std::vector<std::uint8_t> block_;
    /// Frames of real audio in the loaded block, and how far into it reading is.
    std::int64_t blockFrames_ = 0;
    std::int64_t blockPos_    = 0;
    /// Frames of the file the loaded block ends at.
    std::int64_t framePos_ = 0;
};

/// DSDIFF: Philips' container, the one on the SACD authoring side.
///
/// An IFF file with 64-bit chunk sizes: FRM8 wraps a PROP describing the
/// stream and a DSD chunk holding it. The audio is interleaved and MSB-first,
/// so reading it is a copy.
class DffReader final : public DsdReader {
public:
    [[nodiscard]] bool open(ISource& source) {
        source_ = &source;
        if (!source.seekable() || !source.seek(0, SEEK_SET)) {
            return false;
        }

        std::array<std::uint8_t, 16> header{};
        if (!readExactly(source, header.data(), header.size()) ||
            std::memcmp(header.data(), "FRM8", 4) != 0 ||
            std::memcmp(header.data() + 12, "DSD ", 4) != 0) {
            return false;
        }

        std::uint32_t bitRate = 0;
        for (;;) {
            std::array<std::uint8_t, 12> chunk{};
            if (!readExactly(source, chunk.data(), chunk.size())) {
                break;
            }
            const std::uint64_t size = readU64Be(chunk.data() + 4);
            const std::int64_t  body = source.tell();

            if (std::memcmp(chunk.data(), "PROP", 4) == 0) {
                if (!readProperties(source, body, size, bitRate)) {
                    return false;
                }
            } else if (std::memcmp(chunk.data(), "DSD ", 4) == 0) {
                dataOffset_ = body;
                dataBytes_  = static_cast<std::int64_t>(size);
                break;  // the audio is the last thing worth finding
            } else if (std::memcmp(chunk.data(), "DIIN", 4) == 0) {
                readTitleAndArtist(source, body, size);
            }

            // Chunks are padded to an even length, and the pad byte is not
            // counted in the size.
            if (!source.seek(body + static_cast<std::int64_t>(size) +
                                 static_cast<std::int64_t>(size & 1U),
                             SEEK_SET)) {
                break;
            }
        }

        if (channels_ == 0 || bitRate == 0 || dataBytes_ <= 0) {
            return false;
        }
        byteRate_    = static_cast<double>(bitRate) / 8.0;
        totalFrames_ = dataBytes_ / channels_;
        return seek(0);
    }

    [[nodiscard]] std::size_t read(std::uint8_t* out, std::size_t frames) override {
        const std::int64_t left = totalFrames_ - framePos_;
        if (left <= 0) {
            return 0;
        }
        const auto want = static_cast<std::size_t>(
            std::min<std::int64_t>(static_cast<std::int64_t>(frames), left));

        const std::size_t bytes = want * channels_;
        std::size_t       got   = 0;
        while (got < bytes) {
            const std::int64_t read =
                source_->read(out + got, static_cast<std::int64_t>(bytes - got));
            if (read <= 0) {
                break;
            }
            got += static_cast<std::size_t>(read);
        }

        // Whole frames only: a partial one would shift every channel by one
        // from there on, which sounds like the stereo image collapsing.
        const std::size_t produced = got / channels_;
        framePos_ += static_cast<std::int64_t>(produced);
        return produced;
    }

    bool seek(std::int64_t frame) override {
        frame = std::clamp<std::int64_t>(frame, 0, totalFrames_);
        if (!source_->seek(dataOffset_ + (frame * channels_), SEEK_SET)) {
            return false;
        }
        framePos_ = frame;
        return true;
    }

private:
    /// PROP/SND: the sample rate, the channel list, and how it is compressed.
    [[nodiscard]] bool readProperties(ISource& source, std::int64_t body,
                                      std::uint64_t size, std::uint32_t& bitRate) {
        std::array<std::uint8_t, 4> kind{};
        if (!readExactly(source, kind.data(), kind.size()) ||
            std::memcmp(kind.data(), "SND ", 4) != 0) {
            return false;
        }

        const std::int64_t end = body + static_cast<std::int64_t>(size);
        while (source.tell() < end) {
            std::array<std::uint8_t, 12> chunk{};
            if (!readExactly(source, chunk.data(), chunk.size())) {
                return false;
            }
            const std::uint64_t length = readU64Be(chunk.data() + 4);
            const std::int64_t  at     = source.tell();

            if (std::memcmp(chunk.data(), "FS  ", 4) == 0 && length >= 4) {
                std::array<std::uint8_t, 4> rate{};
                if (!readExactly(source, rate.data(), rate.size())) {
                    return false;
                }
                bitRate = (static_cast<std::uint32_t>(rate[0]) << 24) |
                          (static_cast<std::uint32_t>(rate[1]) << 16) |
                          (static_cast<std::uint32_t>(rate[2]) << 8) |
                          static_cast<std::uint32_t>(rate[3]);
            } else if (std::memcmp(chunk.data(), "CHNL", 4) == 0 && length >= 2) {
                std::array<std::uint8_t, 2> count{};
                if (!readExactly(source, count.data(), count.size())) {
                    return false;
                }
                channels_ = (static_cast<std::uint32_t>(count[0]) << 8) |
                            static_cast<std::uint32_t>(count[1]);
                if (channels_ == 0 || channels_ > 8) {
                    return false;
                }
            } else if (std::memcmp(chunk.data(), "CMPR", 4) == 0 && length >= 4) {
                std::array<std::uint8_t, 4> kindId{};
                if (!readExactly(source, kindId.data(), kindId.size())) {
                    return false;
                }
                // `DSD ` is uncompressed and is the only thing this reads.
                // `DST ` is losslessly compressed DSD and needs a decoder of its
                // own -- a real one, not a stub -- so a DST file is declined
                // here rather than played as noise.
                if (std::memcmp(kindId.data(), "DSD ", 4) != 0) {
                    return false;
                }
            }

            if (!source.seek(at + static_cast<std::int64_t>(length) +
                                 static_cast<std::int64_t>(length & 1U),
                             SEEK_SET)) {
                return false;
            }
        }
        return true;
    }

    /// DIIN/DITI and DIIN/DIAR: the title and artist, which is all the format
    /// carries that a playlist wants.
    void readTitleAndArtist(ISource& source, std::int64_t body, std::uint64_t size) {
        const std::int64_t end = body + static_cast<std::int64_t>(size);
        while (source.tell() < end) {
            std::array<std::uint8_t, 12> chunk{};
            if (!readExactly(source, chunk.data(), chunk.size())) {
                return;
            }
            const std::uint64_t length = readU64Be(chunk.data() + 4);
            const std::int64_t  at     = source.tell();

            const bool isTitle  = std::memcmp(chunk.data(), "DITI", 4) == 0;
            const bool isArtist = std::memcmp(chunk.data(), "DIAR", 4) == 0;
            if ((isTitle || isArtist) && length > 4 && length < 4096) {
                // Four bytes of count, then the text; the count is redundant
                // with the chunk length and is not trusted over it.
                std::vector<char> text(static_cast<std::size_t>(length));
                if (readExactly(source, text.data(), text.size())) {
                    std::string value{text.begin() + 4, text.end()};
                    while (!value.empty() && value.back() == '\0') {
                        value.pop_back();
                    }
                    if (!value.empty()) {
                        tags_.set(isTitle ? "title" : "artist",
                                  codecs::toUtf8(std::move(value)));
                    }
                }
            }

            if (!source.seek(at + static_cast<std::int64_t>(length) +
                                 static_cast<std::int64_t>(length & 1U),
                             SEEK_SET)) {
                return;
            }
        }
    }

    ISource*     source_     = nullptr;
    std::int64_t dataOffset_ = 0;
    std::int64_t dataBytes_  = 0;
    std::int64_t framePos_   = 0;
};

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

class DsdDecoder final : public IDecoder {
public:
    bool open(ISource* source) override {
        close();
        if (source == nullptr) {
            return false;
        }

        // By content, not by extension. `.dsf` is claimed by codecs/sdsf too --
        // Sega's Dreamcast Sound Format, an entirely different thing wearing the
        // same three letters -- and neither needs a priority for it, because
        // each recognises its own magic and declines everything else. The same
        // rule settles `.ahx` between Hively and vgmstream.
        auto dsf = std::make_unique<DsfReader>();
        if (dsf->open(*source)) {
            reader_ = std::move(dsf);
            codec_  = "DSD Stream File";
        } else {
            auto dff = std::make_unique<DffReader>();
            if (!dff->open(*source)) {
                return false;
            }
            reader_ = std::move(dff);
            codec_  = "DSDIFF";
        }

        format_.channels      = reader_->channels();
        format_.sampleRate    = reader_->byteRate();
        format_.format        = SampleFormat::DSD;
        format_.bitsPerSample = 1;
        format_.channelConfig = maskForChannels(reader_->channels());

        framePos_ = 0;
        scratch_.resize(kFramesPerRead * reader_->channels());
        return format_.valid() && reader_->totalFrames() > 0;
    }

    [[nodiscard]] TrackProperties properties() const override {
        TrackProperties props;
        props.format      = format_;
        props.totalFrames = reader_ ? reader_->totalFrames() : 0;
        props.seekable    = true;
        props.lossless    = true;
        props.codec       = codec_;
        // Lossless because nothing is thrown away between the file and the
        // filter: these containers hold the bit stream exactly as it was
        // authored. What the decimation does to it afterwards is the chain's
        // business, and the same is true of a DSD `.wv`.
        props.encoding    = "lossless";
        // Eight one-bit samples per byte per channel, which is the whole of what
        // makes this format large.
        props.bitrateKbps = static_cast<std::int32_t>(
            format_.sampleRate * 8.0 * format_.channels / 1000.0);
        return props;
    }

    [[nodiscard]] MetadataMap metadata() const override {
        return reader_ ? reader_->tags() : MetadataMap{};
    }

    bool readAudio(AudioChunk& out) override {
        if (!reader_) {
            return false;
        }
        const std::size_t frames = reader_->read(scratch_.data(), kFramesPerRead);
        if (frames == 0) {
            return false;
        }

        out.clear();
        out.setFormat(format_);
        out.lossless        = true;
        out.streamTimestamp = static_cast<double>(framePos_) / format_.sampleRate;
        out.streamTimeRatio = 1.0;

        std::byte* dst = out.allocFrames(frames);
        std::memcpy(dst, scratch_.data(), frames * format_.channels);
        out.setFrameCount(frames);

        framePos_ += static_cast<std::int64_t>(frames);
        return true;
    }

    std::int64_t seek(std::int64_t frame) override {
        if (!reader_) {
            return -1;
        }
        frame = std::clamp<std::int64_t>(frame, 0, reader_->totalFrames());
        if (!reader_->seek(frame)) {
            return -1;
        }
        framePos_ = frame;
        return frame;
    }

    void close() override {
        reader_.reset();
        scratch_.clear();
        framePos_ = 0;
    }

private:
    std::unique_ptr<DsdReader> reader_;
    AudioFormat                format_{};
    std::string                codec_;
    std::int64_t               framePos_ = 0;
    std::vector<std::uint8_t>  scratch_;
};

/// `wsd` is deliberately absent. Cog claims it beside these two; it is Wideband
/// Single-bit Data, a third container again, and claiming an extension whose
/// layout has never been read against a real file is how a decoder comes to
/// play noise confidently.
constexpr std::string_view kExtensionList[] = {"dsf", "dff", "dsdiff"};
constexpr std::span<const std::string_view> kExtensions{kExtensionList};

}  // namespace
}  // namespace xpcog

void xpcog_register_dsd(xpcog::PluginRegistry& r) {
    r.addDecoder({
        .name       = "DsdDecoder",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = {},
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::DsdDecoder>();
        },
        .available = nullptr,
    });
}
