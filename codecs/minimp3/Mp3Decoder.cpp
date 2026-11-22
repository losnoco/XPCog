// MP3, on minimp3. Port of Cog Plugins/minimp3/MP3Decoder.m.
//
// Two decoders in one, chosen by whether the source can seek, and that is Cog's
// design rather than an accident:
//
//   * A file goes through minimp3-ex's stream parser, which indexes frames,
//     reads the Xing/Info/LAME header and hands back a sample count and gapless
//     padding. Seeking and an accurate length both depend on it.
//   * A stream cannot be indexed -- there is no end to scan to and no seeking
//     back -- so it is decoded a frame at a time out of a sliding buffer with
//     plain minimp3, which needs nothing but the bytes in hand.
//
// The gapless story has two halves. LAME's is in the Xing/Info header and
// minimp3-ex applies it; iTunes writes its own instead, as an `iTunSMPB` comment
// in the ID3v2 tag, which minimp3 knows nothing about. Reading it is what stops
// an AAC-era iTunes rip gaining a click between tracks -- see applyItunesGapless.

#include "../common/Id3v2.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"

#include "minimp3_ex.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog {
namespace {

/// The inherent delay of a conforming MP3 decoder, in samples. Not an iTunes
/// number and not an encoder's: any standard decoder for the format exhibits it,
/// which is why the same figure turns up across unrelated implementations.
///
/// It matters here because the two gapless conventions count from different
/// places. LAME's header measures its delay from the first frame; iTunes
/// measures its padding from the start of the *decoded* signal, so the two are
/// offset by exactly this. Use one convention with the other's numbers and the
/// track moves by twelve milliseconds.
///
/// Written 528 + 1 rather than 529 because that is the form it appears in
/// wherever it is written down at all -- which is rarely. It is folklore that
/// implementations copy from each other rather than derive, so leaving the
/// arithmetic visible is the closest thing to a citation available.
constexpr std::uint32_t kDecoderDelay = 528 + 1;

/// Bounds from Cog, and from what the format can express: padding longer than a
/// couple of frames is a corrupt tag rather than a long fade.
constexpr std::uint32_t kMaxStartPadding = 576 * 2 * 32;
constexpr std::uint32_t kMaxEndPadding   = 576 * 2 * 64;

[[nodiscard]] std::string_view codecNameFor(int layer) {
    switch (layer) {
        case 1: return "MP1";
        case 2: return "MP2";
        default: return "MP3";
    }
}

class Mp3Decoder final : public IDecoder {
public:
    ~Mp3Decoder() override { Mp3Decoder::close(); }

    bool open(ISource* source) override {
        close();

        source_ = source;
        if (source_ == nullptr) {
            return false;
        }
        seekable_ = source_->seekable();

        int chapterIndex = -1;
#if XPCOG_WITH_FFMPEG
        // "file.mp3#2" is a chapter FFMPEGContainer listed, which only FFmpeg
        // knows how to play; refusing it passes it to FFmpeg's decoder, next
        // in priority. (A cue sheet's tracks open their file without a
        // fragment.)
        if (!source_->url().fragment().empty()) {
            return false;
        }
#else
        // Without FFmpeg there is no other decoder to take the chapter, so
        // minimp3 plays the span itself. Numeric fragments on seekable files
        // only: a stream cannot be indexed, and anything else is not a
        // chapter this decoder could name.
        if (!source_->url().fragment().empty()) {
            if (!seekable_) {
                return false;
            }
            const std::string_view fragment = source_->url().fragment();
            const auto parsed = std::from_chars(
                fragment.data(), fragment.data() + fragment.size(), chapterIndex);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != fragment.data() + fragment.size() || chapterIndex < 0) {
                return false;
            }
        }
#endif

        output_.resize(MINIMP3_MAX_SAMPLES_PER_FRAME);

        if (!(seekable_ ? openSeekable(chapterIndex) : openStreaming())) {
            return false;
        }

        syncFormat();
        seconds_  = 0.0;
        framePos_ = 0;
        return format_.valid();
    }

    bool readAudio(AudioChunk& out) override {
        // A chapter ends where its span ends, mid-frame if it must: the last
        // chunk is truncated, and the next call is the end of the stream.
        if (chapterMode_ && framePos_ >= totalFrames_) {
            return false;
        }
        while (framesReady_ == 0) {
            if (!pull()) {
                return false;
            }
            syncFormat();
        }
        if (chapterMode_ &&
            static_cast<std::int64_t>(framesReady_) > totalFrames_ - framePos_) {
            framesReady_ = static_cast<std::size_t>(totalFrames_ - framePos_);
        }

        out.clear();
        out.setFormat(format_);
        out.lossless        = false;
        out.streamTimestamp = seconds_;
        out.streamTimeRatio = 1.0;
        out.assign(output_.data(), framesReady_);

        framePos_ += static_cast<std::int64_t>(framesReady_);
        seconds_ += out.duration();
        framesReady_ = 0;
        return true;
    }

    std::int64_t seek(std::int64_t frame) override {
        if (!exOpen_ || !seekable_) {
            return -1;
        }
        if (frame == framePos_) {
            return frame;
        }
        if (totalFrames_ > 0 && frame > totalFrames_) {
            frame = totalFrames_;
        }

        // Chapter positions are relative to the chapter's start; the ex
        // layer's sample space is the whole file's, gapless-adjusted.
        const std::int64_t absolute = chapterMode_ ? chapterStart_ + frame : frame;
        // Sample units, interleaved: mp3dec_ex counts what it hands back.
        const std::uint64_t target =
            static_cast<std::uint64_t>(absolute) *
            static_cast<std::uint64_t>(ex_.info.channels);
        if (mp3dec_ex_seek(&ex_, target) != 0) {
            return -1;
        }

        framesReady_ = 0;
        framePos_    = frame;
        seconds_     = (format_.sampleRate > 0.0)
                           ? static_cast<double>(frame) / format_.sampleRate
                           : 0.0;
        return frame;
    }

    void close() override {
        if (exOpen_) {
            mp3dec_ex_close(&ex_);
            exOpen_ = false;
        }
        source_       = nullptr;
        inputFilled_  = 0;
        inputEof_     = false;
        framesReady_  = 0;
        foundItunes_  = false;
        id3Length_    = 0;
        totalFrames_  = 0;
        framePos_     = 0;
        layer_        = 0;
        format_       = {};
        chapterMode_  = false;
        chapterStart_ = 0;
        chapterEnd_   = 0;
        chapters_.clear();
        chapterTitle_.clear();
        fileTitle_.clear();
    }

    void interrupt() override {
        if (source_ != nullptr) {
            source_->interrupt();
        }
    }

    [[nodiscard]] TrackProperties properties() const override {
        TrackProperties props;
        props.format      = format_;
        props.totalFrames = totalFrames_;
        props.bitrateKbps = bitrateKbps_;
        props.seekable    = seekable_;
        props.lossless    = false;
        props.codec       = std::string{codecNameFor(layer_)};
        props.encoding    = "lossy";
        return props;
    }

    /// A chapter row carries its own title, and the file's title names the
    /// whole -- FFmpeg's chapter harvest files that under album, and so does
    /// this, so a chapter row reads the same in either build.
    [[nodiscard]] MetadataMap metadata() const override {
        MetadataMap tags;
        if (chapterMode_) {
            if (!chapterTitle_.empty()) {
                tags.set("title", chapterTitle_);
            }
            if (!fileTitle_.empty()) {
                tags.set("album", fileTitle_);
            }
        }
        return tags;
    }

private:
    // --- minimp3-ex I/O ---------------------------------------------------

    static std::size_t readCb(void* buffer, std::size_t bytes, void* user) {
        auto*              self = static_cast<ISource*>(user);
        const std::int64_t got  = self->read(buffer, static_cast<std::int64_t>(bytes));
        return (got <= 0) ? 0 : static_cast<std::size_t>(got);
    }

    static int seekCb(std::uint64_t position, void* user) {
        auto* self = static_cast<ISource*>(user);
        return self->seek(static_cast<std::int64_t>(position), SEEK_SET) ? 0 : -1;
    }

    // --- opening ----------------------------------------------------------

    bool openSeekable(int chapterIndex) {
        if (source_->seek(0, SEEK_END)) {
            fileSize_ = source_->tell();
        }
        if (!source_->seek(0, SEEK_SET)) {
            return false;
        }

        // Before minimp3 sees the file: it skips the ID3v2 tag without reading
        // it, and the gapless numbers iTunes writes are inside. Read once;
        // the chapter frames ride the same bytes.
        const std::vector<std::byte> tag = readId3v2Tag();
        foundItunes_ = applyItunesGapless(tag);
        if (chapterIndex >= 0) {
            codecs::parseId3v2Chapters(tag, chapters_);
        }
        if (!source_->seek(0, SEEK_SET)) {
            return false;
        }

        io_.read      = &Mp3Decoder::readCb;
        io_.read_data = source_;
        io_.seek      = &Mp3Decoder::seekCb;
        io_.seek_data = source_;

        if (mp3dec_ex_open_cb(&ex_, &io_, MP3D_SEEK_TO_SAMPLE) != 0) {
            return false;
        }
        exOpen_ = true;

        if (foundItunes_) {
            // start_delay is what seeking measures from, to_skip is what the
            // first read drops, and detected_samples is where the end is
            // trimmed. All three, or the padding is removed at one end only.
            const auto channels = static_cast<std::uint64_t>(ex_.info.channels);
            ex_.start_delay = ex_.to_skip =
                static_cast<int>(startPadding_ * ex_.info.channels);
            ex_.detected_samples = static_cast<std::uint64_t>(totalFrames_) * channels;
            ex_.samples =
                (static_cast<std::uint64_t>(totalFrames_) + startPadding_ + endPadding_) *
                channels;
        }

        // A frame up front, both to learn the format and to prove the file
        // decodes at all. Retried: the opening frames of a file that begins with
        // junk can come back empty while the parser resynchronises.
        mp3d_sample_t* samples = nullptr;
        std::size_t    got     = 0;
        for (int retry = 10; retry > 0 && got == 0; --retry) {
            got = mp3dec_ex_read_frame(&ex_, &samples, &info_,
                                       MINIMP3_MAX_SAMPLES_PER_FRAME);
        }
        if (got == 0 || samples == nullptr || info_.channels <= 0) {
            return false;
        }
        framesReady_ = got / static_cast<std::size_t>(info_.channels);
        std::memcpy(output_.data(), samples, got * sizeof(mp3d_sample_t));

        if (!foundItunes_) {
            // detected_samples is the Xing/Info header's count with LAME's
            // padding already removed; samples is the raw total, used when
            // there is no such header.
            const std::uint64_t total =
                (ex_.detected_samples != 0) ? ex_.detected_samples : ex_.samples;
            totalFrames_ =
                static_cast<std::int64_t>(total / static_cast<std::uint64_t>(info_.channels));
        }

        if (totalFrames_ > 0 && info_.hz > 0 && fileSize_ > static_cast<std::int64_t>(id3Length_)) {
            const double seconds =
                static_cast<double>(totalFrames_) / static_cast<double>(info_.hz);
            bitrateKbps_ = static_cast<std::int32_t>(
                static_cast<double>(fileSize_ - static_cast<std::int64_t>(id3Length_)) *
                8.0 / seconds / 1000.0);
        }

        // The chapter span, set up last so the bitrate above keeps the whole
        // file's numbers. Only the no-FFmpeg build ever asks for one.
        if (chapterIndex >= 0) {
            if (static_cast<std::size_t>(chapterIndex) >= chapters_.size()) {
                return false;
            }
            const Chapter& chapter = chapters_[static_cast<std::size_t>(chapterIndex)];
            // Milliseconds to frames at the file's own rate, rounded to
            // nearest -- av_rescale_q's convention, so the span agrees with
            // FFmpeg's within a frame.
            const auto toFrames = [this](std::uint64_t ms) {
                return (static_cast<std::int64_t>(ms) * info_.hz + 500) / 1000;
            };
            chapterStart_ = std::min(toFrames(chapter.startMs), totalFrames_);
            chapterEnd_   = std::clamp(toFrames(chapter.endMs), chapterStart_, totalFrames_);
            if (chapterEnd_ <= chapterStart_) {
                return false;
            }
            chapterTitle_ = chapter.title;
            MetadataMap fileTags;
            if (codecs::parseId3v2(tag, fileTags)) {
                fileTitle_ = std::string{fileTags.first("title")};
            }
            totalFrames_ = chapterEnd_ - chapterStart_;
            chapterMode_ = true;
            // Land on the chapter's first sample, dropping the opening frame
            // the format proof decoded. Sample 0 here is the first real audio
            // sample: the ex layer has already applied the encoder delay, so
            // chapter 0 is gapless-correct by construction.
            const std::uint64_t target =
                static_cast<std::uint64_t>(chapterStart_) *
                static_cast<std::uint64_t>(ex_.info.channels);
            if (mp3dec_ex_seek(&ex_, target) != 0) {
                return false;
            }
            framesReady_ = 0;
            framePos_    = 0;
            seconds_     = 0.0;
        }
        return true;
    }

    bool openStreaming() {
        input_.resize(MINIMP3_BUF_SIZE);
        const std::int64_t got =
            source_->read(input_.data(), static_cast<std::int64_t>(input_.size()));
        inputFilled_ = (got > 0) ? static_cast<std::size_t>(got) : 0;
        if (inputFilled_ == 0) {
            return false;
        }

        mp3dec_init(&ex_.mp3d);
        if (mp3dec_detect_buf(input_.data(), inputFilled_) != 0) {
            return false;
        }

        // Decode until a frame comes out. The early returns are frames minimp3
        // consumed without producing audio -- an ID3 tag, a Xing header, junk it
        // resynchronised past -- which is exactly what a stream opens with.
        for (;;) {
            const int samples =
                mp3dec_decode_frame(&ex_.mp3d, input_.data(), static_cast<int>(inputFilled_),
                                    output_.data(), &info_);
            if (info_.frame_bytes <= 0 ||
                static_cast<std::size_t>(info_.frame_bytes) > inputFilled_) {
                return false;
            }
            inputFilled_ -= static_cast<std::size_t>(info_.frame_bytes);
            std::memmove(input_.data(), input_.data() + info_.frame_bytes, inputFilled_);
            if (samples > 0) {
                framesReady_ = static_cast<std::size_t>(samples);
                break;
            }
        }

        bitrateKbps_ = info_.bitrate_kbps;
        return true;
    }

    // --- gapless ----------------------------------------------------------

    /// The whole ID3v2 tag at the start of the file, or empty when there is
    /// none. Leaves the source positioned after the tag; the caller rewinds.
    [[nodiscard]] std::vector<std::byte> readId3v2Tag() {
        std::array<std::byte, 10> head{};
        if (source_->read(head.data(), static_cast<std::int64_t>(head.size())) !=
            static_cast<std::int64_t>(head.size())) {
            return {};
        }

        const std::size_t length = codecs::id3v2TagLength(head);
        if (length <= head.size()) {
            return {};
        }
        id3Length_ = length;

        std::vector<std::byte> tag(length);
        std::memcpy(tag.data(), head.data(), head.size());
        std::size_t filled = head.size();
        while (filled < length) {
            const std::int64_t got =
                source_->read(tag.data() + filled,
                              static_cast<std::int64_t>(length - filled));
            if (got <= 0) {
                return {};
            }
            filled += static_cast<std::size_t>(got);
        }
        return tag;
    }

    /// Reads iTunes's `iTunSMPB` comment out of the ID3v2 tag bytes. Returns
    /// true when it named a padding this decoder can act on.
    ///
    /// The field is a line of hex: a zero, the leading padding, the trailing
    /// padding, the true sample count, another zero, and the offset of the last
    /// eight frames. Only four of those matter here, and every one is bounded
    /// before use -- a wrong padding is silence at the start of a track or a
    /// truncated ending, both of which look like a bad rip rather than a bad
    /// parse.
    bool applyItunesGapless(std::span<const std::byte> tag) {
        if (tag.empty()) {
            return false;
        }
        MetadataMap tags;
        if (!codecs::parseId3v2(tag, tags)) {
            return false;
        }
        const std::string value{tags.first("itunsmpb")};
        if (value.empty()) {
            return false;
        }

        std::uint32_t unused    = 0;
        std::uint32_t startPad  = 0;
        std::uint32_t endPad    = 0;
        std::uint32_t unused2   = 0;
        std::uint64_t duration  = 0;
        std::uint64_t lastEight = 0;
        if (std::sscanf(value.c_str(),
                        "%" SCNx32 " %" SCNx32 " %" SCNx32 " %" SCNx64 " %" SCNx32
                        " %" SCNx64,
                        &unused, &startPad, &endPad, &duration, &unused2,
                        &lastEight) != 6) {
            return false;
        }

        if (duration > static_cast<std::uint64_t>(INT64_MAX) ||
            startPad > kMaxStartPadding || endPad > kMaxEndPadding) {
            return false;
        }
        // The last-frames offset has to land inside the audio. A tag copied
        // between files fails here, which is the point.
        if (fileSize_ <= 0 ||
            lastEight >= static_cast<std::uint64_t>(fileSize_ -
                                                    static_cast<std::int64_t>(id3Length_))) {
            return false;
        }
        // Below the decoder's own delay there is nothing to take off the end,
        // and the subtraction would wrap.
        if (endPad < kDecoderDelay) {
            return false;
        }

        startPadding_ = startPad + kDecoderDelay;
        endPadding_   = endPad - kDecoderDelay;
        totalFrames_  = static_cast<std::int64_t>(duration);
        return true;
    }

    // --- decoding ---------------------------------------------------------

    /// Advances by one step. False when nothing more will come.
    bool pull() { return seekable_ ? pullSeekable() : pullStreaming(); }

    bool pullSeekable() {
        mp3d_sample_t*    samples = nullptr;
        const std::size_t got =
            mp3dec_ex_read_frame(&ex_, &samples, &info_, MINIMP3_MAX_SAMPLES_PER_FRAME);
        if (got == 0 || samples == nullptr || info_.channels <= 0) {
            return false;
        }
        framesReady_ = got / static_cast<std::size_t>(info_.channels);
        std::memcpy(output_.data(), samples, got * sizeof(mp3d_sample_t));
        return true;
    }

    bool pullStreaming() {
        if (const std::size_t room = input_.size() - inputFilled_;
            room > 0 && !inputEof_) {
            const std::int64_t got = source_->read(input_.data() + inputFilled_,
                                                   static_cast<std::int64_t>(room));
            if (got > 0) {
                inputFilled_ += static_cast<std::size_t>(got);
            } else {
                // Only an empty read ends a stream. Cog treats a *short* read as
                // the end too, which is right for a file and wrong for a socket:
                // an HTTP source returns what has arrived, and the rest is still
                // coming.
                inputEof_ = true;
            }
        }
        if (inputFilled_ == 0) {
            return false;
        }

        const int samples =
            mp3dec_decode_frame(&ex_.mp3d, input_.data(), static_cast<int>(inputFilled_),
                                output_.data(), &info_);
        if (info_.frame_bytes <= 0) {
            return false;  // nothing in the buffer to consume; no way forward
        }
        if (static_cast<std::size_t>(info_.frame_bytes) > inputFilled_) {
            // A frame straddling the end of the buffer: more input finishes it.
            return !inputEof_;
        }

        inputFilled_ -= static_cast<std::size_t>(info_.frame_bytes);
        std::memmove(input_.data(), input_.data() + info_.frame_bytes, inputFilled_);
        if (samples > 0) {
            framesReady_ = static_cast<std::size_t>(samples);
        }
        return true;
    }

    /// MP3 allows the sample rate, channel count and layer to change between
    /// frames, and streams that splice together do exactly that.
    void syncFormat() {
        const auto rate     = static_cast<double>(info_.hz);
        const auto channels = static_cast<std::uint32_t>(info_.channels);
        if (rate <= 0.0 || channels == 0) {
            return;
        }
        if (rate == format_.sampleRate && channels == format_.channels &&
            info_.layer == layer_) {
            return;
        }

        const bool first      = !format_.valid();
        format_.sampleRate    = rate;
        format_.channels      = channels;
        format_.channelConfig = guessChannelConfig(channels);
        format_.format        = SampleFormat::F32;
        format_.bitsPerSample = 32;
        layer_                = info_.layer;
        if (!first) {
            notifyChanged(true, false);
        }
    }

    ISource* source_   = nullptr;
    bool     seekable_ = false;

    mp3dec_ex_t         ex_{};
    mp3dec_io_t         io_{};
    mp3dec_frame_info_t info_{};
    bool                exOpen_ = false;

    std::vector<std::uint8_t> input_;
    std::size_t               inputFilled_ = 0;
    bool                      inputEof_    = false;

    std::vector<mp3d_sample_t> output_;
    std::size_t                framesReady_ = 0;

    AudioFormat  format_{};
    std::int64_t totalFrames_ = 0;
    std::int64_t framePos_    = 0;
    std::int64_t fileSize_    = 0;
    std::int32_t bitrateKbps_ = 0;
    int          layer_       = 0;
    double       seconds_     = 0.0;

    std::uint32_t startPadding_ = 0;
    std::uint32_t endPadding_   = 0;
    bool          foundItunes_  = false;
    std::size_t   id3Length_    = 0;

    // --- chapters: the no-FFmpeg build plays the spans itself --------------
    bool        chapterMode_  = false;
    std::int64_t chapterStart_ = 0;
    std::int64_t chapterEnd_   = 0;
    ChapterList chapters_;
    std::string chapterTitle_;
    std::string fileTitle_;
};

constexpr std::string_view kExtensions[] = {"mp3", "mp2", "mpa", "m2a"};
constexpr std::string_view kMimeTypes[]  = {"audio/mpeg", "audio/mp3", "audio/x-mp3"};

}  // namespace
}  // namespace xpcog

void xpcog_register_minimp3(xpcog::PluginRegistry& r) {
    r.addDecoder({
        // Cog's priority for this decoder. Above FFmpeg, which also claims mp3.
        .name       = "Mp3Decoder",
        .priority   = 2.0F,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = xpcog::kMimeTypes,
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::Mp3Decoder>();
        },
        .available = nullptr,
    });
}
