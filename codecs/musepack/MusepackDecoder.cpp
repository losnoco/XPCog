// Musepack, through libmpcdec. Port of Cog Plugins/Musepack/MusepackDecoder.m.
//
// A lossy format from the late nineties, still the one its adherents reach for
// at high bitrates. Two stream versions are in circulation and libmpcdec reads
// both: SV7, and SV8 -- which is the same codec in a chunked container with a
// seek table, so an SV7 file seeks by scanning and an SV8 file seeks directly.
// Nothing here needs to know which; mpc_demux hides it.
//
// The library hands back float samples in the range libmpcdec was configured
// for, which is plain float32 unless MPC_FIXED_POINT is defined -- it is not,
// here or in Cog -- so the frames go through untouched.

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"

#include <mpc/mpcdec.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

namespace xpcog {
namespace {

/// MPC_DECODER_BUFFER_LENGTH is the library's own worst-case frame, in samples
/// across all channels. A read hands back whatever one decode call produced
/// rather than filling a fixed block, so this is a ceiling and not a target.
constexpr std::size_t kDecodeBufferSamples = MPC_DECODER_BUFFER_LENGTH;

class MusepackDecoder final : public IDecoder {
public:
    ~MusepackDecoder() override { MusepackDecoder::close(); }

    bool open(ISource* source) override {
        close();
        source_ = source;
        if (source_ == nullptr) {
            return false;
        }

        reader_.read     = &MusepackDecoder::readCb;
        reader_.seek     = &MusepackDecoder::seekCb;
        reader_.tell     = &MusepackDecoder::tellCb;
        reader_.get_size = &MusepackDecoder::getSizeCb;
        reader_.canseek  = &MusepackDecoder::canSeekCb;
        reader_.data     = source_;

        demux_ = mpc_demux_init(&reader_);
        if (demux_ == nullptr) {
            return false;
        }

        mpc_streaminfo info{};
        mpc_demux_get_info(demux_, &info);

        format_.channels   = info.channels;
        format_.sampleRate = static_cast<double>(info.sample_freq);
        format_.format     = SampleFormat::F32;
        format_.bitsPerSample = 32;
        format_.channelConfig = guessChannelConfig(format_.channels);

        // Cog hardcodes two channels here and calls the field `2 = stereo`.
        // info.channels is what the stream says, and reading it costs nothing:
        // a file that ever says otherwise decodes as itself rather than as
        // interleaved nonsense at the wrong rate.
        if (format_.channels == 0) {
            return false;
        }

        totalFrames_ = static_cast<std::int64_t>(mpc_streaminfo_get_length_samples(&info));
        bitrateKbps_ = static_cast<std::int32_t>(info.average_bitrate / 1000.0);
        framePos_    = 0;
        maxEmptyFrames_ = emptyFrameBound(info.block_pwr);

        return format_.valid();
    }

    bool readAudio(AudioChunk& out) override {
        if (demux_ == nullptr) {
            return false;
        }

        buffer_.resize(kDecodeBufferSamples);

        mpc_frame_info frame{};
        frame.buffer = buffer_.data();

        // Frames that decode to nothing are ordinary, and after a seek they are
        // the rule rather than the exception -- which is the whole reason this
        // is a loop.
        //
        // mpc_demux_seek_sample() does not land on the requested sample. It
        // seeks to the start of the block containing it and sets the decoder's
        // `samples_to_skip` to the remainder, and each decode then throws away
        // up to MPC_FRAME_LENGTH of that and reports `samples = 0`. So a seek
        // into the middle of a block is followed by as many empty frames as the
        // block is long. Returning end-of-stream at the first of them is the bug
        // this loop exists to avoid: a fixed cap of sixteen looked generous and
        // failed on a one-second file, because the count depends on the block
        // size and not on anything a caller can see.
        //
        // The bound below is that count rather than a guess; see maxEmptyFrames_.
        for (int attempt = 0; attempt < maxEmptyFrames_; ++attempt) {
            const mpc_status status = mpc_demux_decode(demux_, &frame);

            // One value, two meanings. mpc_demux_decode sets bits to -1 at the
            // "SE" end-of-stream block *and* on a decode failure, where the
            // comment in the library reads "we pretend it's end of file". The
            // status is what separates them, and Cog -- which looks only at
            // bits -- cannot tell a truncated file from a complete one. Both
            // still stop, so this is about what gets reported upward, not about
            // playing more of a broken file.
            //
            // Compared against MPC_STATUS_OK rather than through the library's
            // own MPC_IS_FAILURE: that macro lives in libmpcdec/internal.h,
            // which is not installed and is not meant to be included by a
            // caller. The enum it tests is public and has two values.
            if (frame.bits == -1) {
                truncated_ = status != MPC_STATUS_OK;
                return false;
            }
            if (frame.samples != 0) {
                break;
            }
        }
        if (frame.samples == 0) {
            return false;
        }

        const auto frames   = static_cast<std::size_t>(frame.samples);
        const auto channels = static_cast<std::size_t>(format_.channels);

        out.clear();
        out.setFormat(format_);
        out.lossless        = false;
        out.streamTimestamp = (format_.sampleRate > 0.0)
                                  ? static_cast<double>(framePos_) / format_.sampleRate
                                  : 0.0;
        out.streamTimeRatio = 1.0;

        std::byte* dst = out.allocFrames(frames);
        std::memcpy(dst, buffer_.data(), frames * channels * sizeof(MPC_SAMPLE_FORMAT));

        framePos_ += static_cast<std::int64_t>(frames);
        return true;
    }

    std::int64_t seek(std::int64_t frame) override {
        if (demux_ == nullptr) {
            return -1;
        }
        frame = std::clamp<std::int64_t>(frame, 0, totalFrames_);
        if (mpc_demux_seek_sample(demux_, static_cast<mpc_uint64_t>(frame)) !=
            MPC_STATUS_OK) {
            return -1;
        }
        framePos_ = frame;
        return frame;
    }

    void close() override {
        if (demux_ != nullptr) {
            mpc_demux_exit(demux_);
            demux_ = nullptr;
        }
        source_ = nullptr;
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
        props.seekable    = source_ != nullptr && source_->seekable();
        props.lossless    = false;
        props.codec       = "Musepack";
        props.encoding    = "lossy";
        return props;
    }

private:
    /// The most empty frames a seek can produce, from the library's own
    /// arithmetic rather than from taste.
    ///
    /// mpc_demux_seek_sample() sets `samples_to_skip` to
    /// `MPC_DECODER_SYNTH_DELAY + destsample % (MPC_FRAME_LENGTH << block_pwr)`,
    /// and adds `MPC_FRAME_LENGTH * 32` for a stream version 7 seek past the
    /// thirty-second block. Each decode consumes at most MPC_FRAME_LENGTH of it,
    /// so the number of empty frames is bounded by the frames in a block, plus
    /// 32 for the SV7 term, plus one each for the synth delay and the remainder.
    ///
    /// SV7 reports block_pwr as 0, which makes the first term 1 and leaves the
    /// SV7 allowance doing the work; SV8 files from the reference encoder use
    /// block_pwr 0 too, but the field is per-file and a large-block stream is
    /// legal, so it is read rather than assumed.
    [[nodiscard]] static int emptyFrameBound(mpc_uint32_t blockPwr) {
        return (1 << blockPwr) + 34;
    }

    static ISource* src(mpc_reader* reader) {
        return static_cast<ISource*>(reader->data);
    }

    static mpc_int32_t readCb(mpc_reader* reader, void* buffer, mpc_int32_t size) {
        return static_cast<mpc_int32_t>(src(reader)->read(buffer, size));
    }

    static mpc_bool_t seekCb(mpc_reader* reader, mpc_int32_t offset) {
        return src(reader)->seek(offset, SEEK_SET) ? MPC_TRUE : MPC_FALSE;
    }

    static mpc_int32_t tellCb(mpc_reader* reader) {
        return static_cast<mpc_int32_t>(src(reader)->tell());
    }

    /// Zero for an unseekable source, which is what libmpcdec reads as "length
    /// unknown" -- it then decodes forwards and refuses to seek.
    static mpc_int32_t getSizeCb(mpc_reader* reader) {
        auto* source = src(reader);
        if (!source->seekable()) {
            return 0;
        }
        const std::int64_t saved = source->tell();
        if (!source->seek(0, SEEK_END)) {
            return 0;
        }
        const std::int64_t size = source->tell();
        source->seek(saved, SEEK_SET);
        // The callback is int32 and the format's own header fields are too, so a
        // file past 2 GB cannot be described to the library whatever we do here.
        return static_cast<mpc_int32_t>(
            std::min<std::int64_t>(size, 0x7FFFFFFF));
    }

    static mpc_bool_t canSeekCb(mpc_reader* reader) {
        return src(reader)->seekable() ? MPC_TRUE : MPC_FALSE;
    }

    mpc_reader  reader_{};
    mpc_demux*  demux_  = nullptr;
    ISource*    source_ = nullptr;

    AudioFormat                    format_{};
    std::vector<MPC_SAMPLE_FORMAT> buffer_;
    std::int64_t                   totalFrames_    = 0;
    std::int64_t                   framePos_       = 0;
    std::int32_t                   bitrateKbps_    = 0;
    int                            maxEmptyFrames_ = 0;
    bool                           truncated_      = false;
};

// `mpc` is the only extension Cog claims, and the only one in current use.
// `mp+` and `mpp` are SV4-SV6 spellings that predate the SV7 this library's
// oldest supported stream version is, so claiming them would promise files it
// cannot decode.
constexpr std::string_view kExtensions[] = {"mpc"};
constexpr std::string_view kMimeTypes[]  = {"audio/x-musepack", "audio/musepack"};

}  // namespace
}  // namespace xpcog

void xpcog_register_musepack(xpcog::PluginRegistry& r) {
    r.addDecoder({
        .name       = "MusepackDecoder",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = xpcog::kMimeTypes,
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::MusepackDecoder>();
        },
        .available = nullptr,
    });
}
