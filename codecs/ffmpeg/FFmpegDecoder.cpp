// Port of Cog Plugins/FFMPEG/FFMPEGDecoder.m.
//
// This is the catch-all decoder: AAC, ALAC, WMA, AC3, DTS, TAK, TTA, Musepack,
// APE and the MP4/MKV/ASF containers, plus anything else the build's FFmpeg
// carries. It runs at low priority so a dedicated decoder always wins for the
// formats that have one.
//
// Cog's FFmpeg links AudioToolbox purely because its bundled build enables the
// audiotoolboxdec codecs. That is a build configuration, not a code dependency;
// the vcpkg FFmpeg here is fully portable.

#include "../common/Id3v2.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog {
namespace {

/// FFmpeg wants to seek and read through our own ISource rather than open paths
/// itself, so a custom AVIOContext is installed. 64 KiB matches Cog's buffer.
constexpr int kIoBufferSize = 64 * 1024;

static int readPacket(void* client, std::uint8_t* buffer, int size) {
    auto*              self = static_cast<ISource*>(client);
    const std::int64_t got  = self->read(buffer, size);
    if (got <= 0) {
        return AVERROR_EOF;
    }
    return static_cast<int>(got);
}

static std::int64_t seekIo(void* client, std::int64_t offset, int whence) {
    auto* self = static_cast<ISource*>(client);

    if (whence == AVSEEK_SIZE) {
        if (!self->seekable()) {
            return -1;
        }
        const std::int64_t saved = self->tell();
        self->seek(0, SEEK_END);
        const std::int64_t size = self->tell();
        self->seek(saved, SEEK_SET);
        return size;
    }

    // FFmpeg may OR in AVSEEK_FORCE, which carries no meaning for us.
    whence &= ~AVSEEK_FORCE;
    if (!self->seek(offset, whence)) {
        return -1;
    }
    return self->tell();
}

bool openFormatContext(ISource* source, AVIOContext*& ioContext, AVFormatContext*& formatContext) {
    auto* ioBuffer = static_cast<unsigned char*>(av_malloc(kIoBufferSize));
    if (ioBuffer == nullptr) {
        return false;
    }

    ioContext = avio_alloc_context(ioBuffer, kIoBufferSize, 0, source,
                                   &readPacket, nullptr,
                                   source->seekable() ? &seekIo : nullptr);
    if (ioContext == nullptr) {
        av_free(ioBuffer);
        return false;
    }
    ioContext->seekable = source->seekable() ? AVIO_SEEKABLE_NORMAL : 0;

    formatContext     = avformat_alloc_context();
    if (formatContext == nullptr) {
        return false;
    }
    formatContext->pb = ioContext;
    formatContext->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (avformat_open_input(&formatContext, "", nullptr, nullptr) != 0) {
        formatContext = nullptr;  // avformat_open_input frees it on failure
        return false;
    }
    if (avformat_find_stream_info(formatContext, nullptr) < 0) {
        return false;
    }

    return true;
}

/// The subsong a fragment names. We number subsongs starting from zero.
[[nodiscard]] unsigned int songFromFragment(const Url& url) {
    const std::string_view fragment = url.fragment();
    unsigned int           value    = 0;
    for (const char c : fragment) {
        if (c < '0' || c > '9') {
            return 0;
        }
        value = value * 10 + static_cast<unsigned int>(c - '0');
    }
    return value;
}

class FFmpegDecoder final : public IDecoder {
public:
    ~FFmpegDecoder() override { FFmpegDecoder::close(); }

    bool open(ISource* source) override {
        close();
        // FFmpeg writes diagnostics straight to stderr, which corrupts the CLI's
        // output and is noise in test logs. Errors surface through return codes.
        static const int quiet = [] {
            av_log_set_level(AV_LOG_QUIET);
            return 0;
        }();
        (void)quiet;

        source_ = source;
        if (source_ == nullptr) {
            return false;
        }

        if (!openFormatContext(source, io_, format_ctx_)) {
            return false;
        }

        // MPEG-TS carries HLS's timed metadata in its own elementary stream
        // (stream_type 0x15), which av_find_best_stream will never pick because
        // it is not audio. Noted here so its packets can be recognised in the
        // read loop rather than discarded with everything else non-audio.
        for (unsigned int i = 0; i < format_ctx_->nb_streams; ++i) {
            if (format_ctx_->streams[i]->codecpar->codec_id == AV_CODEC_ID_TIMED_ID3) {
                timedId3Streams_.push_back(static_cast<int>(i));
            }
        }

        const AVCodec* codec = nullptr;
        streamIndex_ = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_AUDIO, -1, -1,
                                           &codec, 0);
        if (streamIndex_ < 0 || codec == nullptr) {
            return false;
        }

        AVStream* stream = format_ctx_->streams[streamIndex_];

        codec_ctx_ = avcodec_alloc_context3(codec);
        if (codec_ctx_ == nullptr ||
            avcodec_parameters_to_context(codec_ctx_, stream->codecpar) < 0 ||
            avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
            return false;
        }

        const int channels = codec_ctx_->ch_layout.nb_channels;
        audioFormat_.channels      = static_cast<std::uint32_t>(channels);
        audioFormat_.sampleRate    = static_cast<double>(codec_ctx_->sample_rate);
        audioFormat_.format        = SampleFormat::F32;
        audioFormat_.bitsPerSample = 32;
        audioFormat_.channelConfig = channelConfigFrom(codec_ctx_->ch_layout);

        // Everything is normalised to interleaved float32 here rather than
        // carrying FFmpeg's many planar and integer layouts through the chain.
        if (!setupResampler()) {
            return false;
        }

        if (stream->duration != AV_NOPTS_VALUE) {
            totalFrames_ = av_rescale_q(stream->duration, stream->time_base,
                                        AVRational{1, codec_ctx_->sample_rate});
        } else if (format_ctx_->duration != AV_NOPTS_VALUE) {
            totalFrames_ = av_rescale(format_ctx_->duration, codec_ctx_->sample_rate,
                                      AV_TIME_BASE);
        }

        subsongIndex_ = songFromFragment(source->url());
        if (subsongIndex_ < format_ctx_->nb_chapters) {
            AVRational tb = {1, codec_ctx_->sample_rate};
            AVChapter *chapter = format_ctx_->chapters[subsongIndex_];
            startFrames_ = av_rescale_q(chapter->start, chapter->time_base, tb);
            endFrames_   = av_rescale_q(chapter->end, chapter->time_base, tb);
            totalFrames_ = endFrames_ - startFrames_;
        }

        bitrateKbps_ = static_cast<std::int32_t>(
            (codec_ctx_->bit_rate ? codec_ctx_->bit_rate : format_ctx_->bit_rate) / 1000);

        codecName_ = codec->long_name != nullptr ? codec->long_name : codec->name;
        lossless_  = isLosslessCodec(codec_ctx_->codec_id);

        packet_ = av_packet_alloc();
        frame_  = av_frame_alloc();
        if (packet_ == nullptr || frame_ == nullptr) {
            return false;
        }

        readTags();
        // Anything the demuxer raised while probing is already in tags_; leaving
        // the flag set would announce a change on the first read that is not one.
        clearMetadataEvents();
        framePos_ = 0;
        if (startFrames_) {
            seek(0);
        }
        return audioFormat_.valid();
    }

    bool readAudio(AudioChunk& out) override {
        const double timestamp =
            (audioFormat_.sampleRate > 0.0)
                ? static_cast<double>(framePos_ - startFrames_) / audioFormat_.sampleRate
                : 0.0;

        for (;;) {
            int status = avcodec_receive_frame(codec_ctx_, frame_);

            if (status == 0) {
                // av_seek_frame lands on a packet boundary at or before the
                // target, so the first frames after a seek belong to an earlier
                // position. Work out how many to drop for a sample-accurate seek.
                if (resolveSeek_) {
                    const std::int64_t pts =
                        (frame_->best_effort_timestamp != AV_NOPTS_VALUE)
                            ? frame_->best_effort_timestamp
                            : frame_->pts;
                    if (pts != AV_NOPTS_VALUE) {
                        const AVStream* stream = format_ctx_->streams[streamIndex_];
                        const std::int64_t landed =
                            av_rescale_q(pts, stream->time_base,
                                         AVRational{1, codec_ctx_->sample_rate});
                        skipFrames_ = (landed < seekTarget_) ? seekTarget_ - landed : 0;
                        framePos_   = std::max(landed, seekTarget_);
                    }
                    resolveSeek_ = false;
                }

                const int channels = static_cast<int>(audioFormat_.channels);
                converted_.resize(static_cast<std::size_t>(frame_->nb_samples) * channels);

                auto*     dst    = reinterpret_cast<std::uint8_t*>(converted_.data());
                int       frames = swr_convert(swr_, &dst, frame_->nb_samples,
                                               const_cast<const std::uint8_t**>(frame_->data),
                                               frame_->nb_samples);
                av_frame_unref(frame_);
                if (frames <= 0) {
                    continue;
                }

                std::size_t offset = 0;
                if (skipFrames_ > 0) {
                    if (skipFrames_ >= frames) {
                        // The whole frame precedes the target; fetch another.
                        skipFrames_ -= frames;
                        continue;
                    }
                    offset = static_cast<std::size_t>(skipFrames_) * channels;
                    frames -= static_cast<int>(skipFrames_);
                    skipFrames_ = 0;
                }

                if (endFrames_ && framePos_ + frames > endFrames_) {
                    frames = endFrames_ - framePos_;
                    if (frames <= 0) {
                        return false;
                    }
                }

                out.clear();
                out.setFormat(audioFormat_);
                out.lossless        = lossless_;
                out.streamTimestamp = timestamp;
                out.streamTimeRatio = 1.0;
                out.assign(converted_.data() + offset, static_cast<std::size_t>(frames));

                framePos_ += frames;
                return true;
            }

            if (status == AVERROR_EOF) {
                return false;
            }
            if (status != AVERROR(EAGAIN)) {
                return false;
            }

            // The decoder wants more input.
            const int read = av_read_frame(format_ctx_, packet_);
            // Checked whatever av_read_frame returned: a demuxer can raise the
            // flag on the same call that reports end of input.
            harvestMidStreamTags();
            if (read < 0) {
                // Flush whatever is still buffered inside the decoder.
                avcodec_send_packet(codec_ctx_, nullptr);
                if (drained_) {
                    return false;
                }
                drained_ = true;
                continue;
            }

            if (packet_->stream_index == streamIndex_) {
                avcodec_send_packet(codec_ctx_, packet_);
            } else if (isTimedId3Stream(packet_->stream_index)) {
                applyTimedId3(packet_->data, packet_->size);
            }
            av_packet_unref(packet_);
        }
    }

    std::int64_t seek(std::int64_t frame) override {
        if (format_ctx_ == nullptr || streamIndex_ < 0) {
            return -1;
        }

        // Offset by current chapter start
        frame += startFrames_;

        AVStream* stream = format_ctx_->streams[streamIndex_];

        // Seek behind the target and decode forward into it. Codecs with
        // overlapping transform windows (AAC and friends) reconstruct a block
        // from its predecessor, so starting cold at a packet boundary yields
        // audibly wrong samples for the first block. Decoding a little history
        // first warms that state up; the extra frames are discarded below.
        const std::int64_t preRoll = codec_ctx_->sample_rate / 4;  // 250 ms
        const std::int64_t from    = std::max<std::int64_t>(0, frame - preRoll);

        const std::int64_t target =
            av_rescale_q(from, AVRational{1, codec_ctx_->sample_rate}, stream->time_base);

        if (av_seek_frame(format_ctx_, streamIndex_, target, AVSEEK_FLAG_BACKWARD) < 0) {
            return -1;
        }
        avcodec_flush_buffers(codec_ctx_);
        drained_     = false;
        seekTarget_  = frame;
        resolveSeek_ = true;
        skipFrames_  = 0;
        framePos_    = frame;
        return frame - startFrames_;
    }

    void close() override {
        if (swr_ != nullptr) {
            swr_free(&swr_);
        }
        if (frame_ != nullptr) {
            av_frame_free(&frame_);
        }
        if (packet_ != nullptr) {
            av_packet_free(&packet_);
        }
        if (codec_ctx_ != nullptr) {
            avcodec_free_context(&codec_ctx_);
        }
        if (format_ctx_ != nullptr) {
            avformat_close_input(&format_ctx_);
        }
        if (io_ != nullptr) {
            // avio owns the buffer, which it may have reallocated.
            av_freep(&io_->buffer);
            avio_context_free(&io_);
        }
        source_      = nullptr;
        streamIndex_ = -1;
        drained_     = false;
        timedId3Streams_.clear();
    }

    void interrupt() override {
        if (source_ != nullptr) {
            source_->interrupt();
        }
    }

    [[nodiscard]] TrackProperties properties() const override {
        TrackProperties props;
        props.format      = audioFormat_;
        props.totalFrames = totalFrames_;
        props.bitrateKbps = bitrateKbps_;
        props.seekable    = source_ != nullptr && source_->seekable();
        props.lossless    = lossless_;
        props.codec       = codecName_;
        props.encoding    = lossless_ ? "lossless" : "lossy";
        props.replayGain  = replayGain_;
        return props;
    }

    [[nodiscard]] MetadataMap metadata() const override { return tags_; }

private:
    [[nodiscard]] static bool isLosslessCodec(AVCodecID id) {
        switch (id) {
            case AV_CODEC_ID_FLAC:
            case AV_CODEC_ID_ALAC:
            case AV_CODEC_ID_APE:
            case AV_CODEC_ID_TAK:
            case AV_CODEC_ID_TTA:
            case AV_CODEC_ID_WAVPACK:
            case AV_CODEC_ID_SHORTEN:
            case AV_CODEC_ID_MLP:
            case AV_CODEC_ID_TRUEHD:
            case AV_CODEC_ID_WMALOSSLESS:
            case AV_CODEC_ID_PCM_S16LE:
            case AV_CODEC_ID_PCM_S24LE:
            case AV_CODEC_ID_PCM_S32LE:
                return true;
            default:
                return false;
        }
    }

    /// Maps FFmpeg's channel mask onto Cog's, which share the first 18 bit
    /// positions in the same order (FL, FR, FC, LFE, BL, BR, ...).
    [[nodiscard]] static std::uint32_t channelConfigFrom(const AVChannelLayout& layout) {
        if (layout.order == AV_CHANNEL_ORDER_NATIVE && layout.u.mask != 0) {
            return static_cast<std::uint32_t>(layout.u.mask & 0x3FFFF);
        }
        return guessChannelConfig(static_cast<std::uint32_t>(layout.nb_channels));
    }

    bool setupResampler() {
        AVChannelLayout outLayout{};
        av_channel_layout_copy(&outLayout, &codec_ctx_->ch_layout);

        const int status = swr_alloc_set_opts2(
            &swr_, &outLayout, AV_SAMPLE_FMT_FLT, codec_ctx_->sample_rate,
            &codec_ctx_->ch_layout, codec_ctx_->sample_fmt, codec_ctx_->sample_rate, 0,
            nullptr);
        av_channel_layout_uninit(&outLayout);

        return status >= 0 && swr_ != nullptr && swr_init(swr_) >= 0;
    }

    /// Mid-stream tag updates, which arrive as ID3v2 chunks spliced between
    /// audio frames rather than in a header.
    ///
    /// This is how a live stream renames the track that is playing when the
    /// transport is not SHOUTcast: an HLS packed-audio rendition carries an ID3v2
    /// tag at the head of every segment, and the ones after the first are the
    /// station saying what is on now. FFmpeg's ADTS demuxer parses them into
    /// `AVFormatContext::metadata` and raises a flag; nothing here was reading
    /// it, so every tag after the first was decoded and dropped.
    ///
    /// The flag is the whole mechanism -- an unconditional re-harvest per packet
    /// would rebuild the tag map thousands of times a second and report a change
    /// each time, which downstream reads as a new track.
    void harvestMidStreamTags() {
        if (!takeMetadataEvents()) {
            return;
        }

        // Compared, not just re-read. A station repeats the current title in
        // every segment, so the demuxer raises the flag on each one whether or
        // not the programme moved on -- and announcing that renames the playing
        // track every ten seconds for the whole broadcast, repainting the
        // playlist row and refiring every now-playing surface each time.
        MetadataMap          previousTags = tags_;
        const ReplayGainInfo previousGain = replayGain_;
        readTags();
        if (tags_.sameContentAs(previousTags) && replayGain_ == previousGain) {
            // Put the old map back, so an unchanged broadcast does not quietly
            // reshuffle the order the tags are displayed in either.
            tags_ = std::move(previousTags);
            return;
        }

        // Properties are unchanged: a tag block does not alter the format, and
        // saying it did makes the chain re-evaluate the stream for nothing.
        notifyChanged(false, true);
    }

    [[nodiscard]] bool isTimedId3Stream(int index) const {
        return std::find(timedId3Streams_.begin(), timedId3Streams_.end(), index) !=
               timedId3Streams_.end();
    }

    /// One packet of an MPEG-TS timed-metadata stream, whose payload is a whole
    /// ID3v2 tag.
    ///
    /// The other half of the same feature as harvestMidStreamTags(): a
    /// packed-audio HLS rendition splices its tags into the audio, where the
    /// demuxer parses them, and a transport-stream rendition puts them in a
    /// stream of their own, where nothing does -- libavformat exposes no public
    /// ID3 parser, so the bytes are ours to read.
    ///
    /// Announced only when something actually changed. Every HLS segment carries
    /// one of these packets whether or not the programme moved on, so reporting
    /// each one would rename the track every few seconds for the whole broadcast.
    void applyTimedId3(const std::uint8_t* payload, int bytes) {
        if (payload == nullptr || bytes <= 0) {
            return;
        }

        MetadataMap updated = tags_;
        if (!codecs::parseId3v2({reinterpret_cast<const std::byte*>(payload),
                                 static_cast<std::size_t>(bytes)},
                                updated)) {
            return;
        }
        if (updated.sameContentAs(tags_)) {
            return;
        }
        tags_ = std::move(updated);
        notifyChanged(false, true);
    }

    /// True when a demuxer has announced new metadata since the last call, which
    /// also clears the announcement.
    [[nodiscard]] bool takeMetadataEvents() {
        bool updated = false;

        if ((format_ctx_->event_flags & AVFMT_EVENT_FLAG_METADATA_UPDATED) != 0) {
            format_ctx_->event_flags &= ~AVFMT_EVENT_FLAG_METADATA_UPDATED;
            updated = true;
        }
        // Per-stream as well as per-container: a chained Ogg announces its new
        // comment header on the stream, not on the file.
        if (streamIndex_ >= 0) {
            AVStream* stream = format_ctx_->streams[streamIndex_];
            if ((stream->event_flags & AVSTREAM_EVENT_FLAG_METADATA_UPDATED) != 0) {
                stream->event_flags &= ~AVSTREAM_EVENT_FLAG_METADATA_UPDATED;
                updated = true;
            }
        }
        return updated;
    }

    void clearMetadataEvents() { (void)takeMetadataEvents(); }

    void readTags() {
        tags_.clear();
        replayGain_ = {};

        const auto harvest = [&](AVDictionary* dict, bool global) {
            const AVDictionaryEntry* entry = nullptr;
            while ((entry = av_dict_iterate(dict, entry)) != nullptr) {
                std::string key{entry->key};
                std::transform(key.begin(), key.end(), key.begin(), [](char c) {
                    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                });

                // ID3 PRIV frames, which FFmpeg surfaces under this prefix.
                // Dropped wholesale, for the same reason the timed-ID3 parser
                // beside this one drops them: they are private data for the
                // publisher's own tooling, and on a live stream they move far
                // more often than the programme does. Apple's
                // transportStreamTimestamp is in every HLS segment and holds
                // that segment's own timestamp; a station's own blob carries
                // things like an in-break flag and an ad/content marker. Keeping
                // any of them makes the comparison in harvestMidStreamTags()
                // find a change in an unchanging broadcast -- which renames the
                // playing track for something no listener can see.
                if (key.starts_with("id3v2_priv.")) {
                    continue;
                }

                if (key == "replaygain_track_gain" ||
                    (!global && key == "replaygain_gain")) {
                    replayGain_.trackGain = std::strtof(entry->value, nullptr);
                } else if (key == "replaygain_album_gain" ||
                           (global && key == "replaygain_gain")) {
                    replayGain_.albumGain = std::strtof(entry->value, nullptr);
                } else if (key == "replaygain_track_peak" ||
                           (!global && key == "replaygain_peak")) {
                    replayGain_.trackPeak = std::strtof(entry->value, nullptr);
                } else if (key == "replaygain_album_peak" ||
                           (global && key == "replaygain_peak")) {
                    replayGain_.albumPeak = std::strtof(entry->value, nullptr);
                } else if (global && key == "title") {
                    tags_.add("album", entry->value);
                } else {
                    tags_.add(key, entry->value);
                }
            }
        };

        harvest(format_ctx_->metadata, subsongIndex_ < format_ctx_->nb_chapters);
        if (streamIndex_ >= 0) {
            harvest(format_ctx_->streams[streamIndex_]->metadata, false);
        }
        if (subsongIndex_ < format_ctx_->nb_chapters) {
            harvest(format_ctx_->chapters[subsongIndex_]->metadata, false);
        }
    }

    ISource*         source_      = nullptr;
    AVIOContext*     io_          = nullptr;
    AVFormatContext* format_ctx_  = nullptr;
    AVCodecContext*  codec_ctx_   = nullptr;
    SwrContext*      swr_         = nullptr;
    AVPacket*        packet_      = nullptr;
    AVFrame*         frame_       = nullptr;
    int              streamIndex_ = -1;
    bool             drained_     = false;
    std::vector<int> timedId3Streams_;

    // Chapter bookkeeping, also used for gaplessness in some containers
    std::int64_t startFrames_  = 0;
    std::int64_t endFrames_    = 0;
    unsigned int subsongIndex_ = 0;

    // Sample-accurate seek bookkeeping.
    std::int64_t seekTarget_  = 0;
    std::int64_t skipFrames_  = 0;
    bool         resolveSeek_ = false;

    AudioFormat        audioFormat_{};
    std::vector<float> converted_;
    std::int64_t       totalFrames_ = 0;
    std::int64_t       framePos_    = 0;
    std::int32_t       bitrateKbps_ = 0;
    std::string        codecName_;
    bool               lossless_ = false;

    MetadataMap    tags_;
    ReplayGainInfo replayGain_;
};

/// A tune with multiple chapters expands to one URL per song, numbered from zero.
std::vector<Url> expandTune(const Url& url, ISource& source,
                            const PluginRegistry& /*registry*/) {
    if (!url.fragment().empty()) {
        return {url};
    }

    AVIOContext*     io          = nullptr;
    AVFormatContext* format_ctx  = nullptr;

    unsigned int subsongs = 1;
    if (openFormatContext(&source, io, format_ctx) &&
        format_ctx->nb_chapters > 1) {
        subsongs = format_ctx->nb_chapters;
    }

    if (format_ctx != nullptr) {
        avformat_close_input(&format_ctx);
    }
    if (io != nullptr) {
        // avio owns the buffer, which it may have reallocated.
        av_freep(&io->buffer);
        avio_context_free(&io);
    }

    if (subsongs <= 1) {
        return {url};
    }

    std::vector<Url> songs;
    songs.reserve(subsongs);
    for (unsigned int i = 0; i < subsongs; ++i) {
        songs.push_back(url.withFragment(std::to_string(i)));
    }
    return songs;
}

// Deliberately broad: FFmpeg is the fallback for everything without a dedicated
// decoder. Formats that do have one (flac, ogg, opus, mp3, wv) are still listed
// so FFmpeg can take over when the specialised decoder rejects a file, which is
// what MultiDecoder's priority ordering is for.
constexpr std::string_view kExtensions[] = {
    "aac", "ac3", "aif", "aifc", "aiff", "alac", "amr", "ape", "asf", "au",
    "caf", "dts", "eac3", "m4a", "m4b",  "mka",  "mkv", "mp4", "mpc", "oma",
    "opus", "ra", "rm",  "tak",  "tta",  "wav",  "wma", "wv",  "flac", "mp3",
    "ogg", "webm",
    // MPEG-TS. Cog claims none of these and its HLS plugin works around that by
    // instantiating FFMPEGDecoder by class name; here the HLS decoder names what
    // it fetched and lets the registry choose, so the transport stream has to be
    // claimed by whoever can actually demux it. A local .ts plays as a side
    // effect, which it should -- FFmpeg has always been able to read one.
    "ts", "m2ts", "mts"};

constexpr std::string_view kMimeTypes[] = {
    "audio/aac",  "audio/mp4", "audio/x-ms-wma", "audio/ac3",
    "audio/vnd.dolby.dd-raw", "audio/x-ape",     "audio/x-tta",
    // audio/aacp is what an AAC+ radio station announces. A stream URL usually
    // has no extension, so the MIME type is the only thing that names the codec
    // and a missing entry means the stream simply will not play.
    "audio/aacp",
    // The MIME half of the MPEG-TS entry above, for a segment whose name says
    // nothing.
    "video/mp2t", "audio/mp2t"};

}  // namespace
}  // namespace xpcog

void xpcog_register_ffmpeg(xpcog::PluginRegistry& r) {
    r.addContainer({
        .name       = "FFmpegContainer",
        .priority   = 0.5F,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = xpcog::kMimeTypes,
        .expand     = &xpcog::expandTune,
    });

    r.addDecoder({
        // Below default, so any dedicated decoder is tried first.
        .name       = "FFmpegDecoder",
        .priority   = 0.5F,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = xpcog::kMimeTypes,
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::FFmpegDecoder>();
        },
        .available = nullptr,
    });
}
