#include "FlacDecoder.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace xpcog {
namespace {

/// Cog sizes its block buffer for the largest legal FLAC frame. Same reasoning:
/// max blocksize 65535, up to 8 channels, 4 bytes per sample.
constexpr std::size_t kMaxBlockBytes = 65535U * 8U * 4U;

/// Maps a FLAC bit depth onto the narrowest container that holds it.
/// Cog packs to the nearest byte and reports bitsPerSample separately; the same
/// applies here, but native-endian.
[[nodiscard]] SampleFormat containerFor(std::uint32_t bitsPerSample) {
    if (bitsPerSample <= 8) return SampleFormat::S8;
    if (bitsPerSample <= 16) return SampleFormat::S16;
    if (bitsPerSample <= 24) return SampleFormat::S24;
    return SampleFormat::S32;
}

[[nodiscard]] std::string lowercased(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return out;
}

[[nodiscard]] std::optional<float> parseFloat(std::string_view text) {
    try {
        return std::stof(std::string{text});
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

FlacDecoder::~FlacDecoder() { FlacDecoder::close(); }

// --- libFLAC callbacks ----------------------------------------------------

FLAC__StreamDecoderReadStatus FlacDecoder::readCb(const FLAC__StreamDecoder*,
                                                  FLAC__byte   buffer[],
                                                  std::size_t* bytes,
                                                  void*        client) {
    auto* self = static_cast<FlacDecoder*>(client);

    const std::int64_t got =
        self->source_->read(buffer, static_cast<std::int64_t>(*bytes));
    if (got < 0) {
        *bytes = 0;
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }
    if (got == 0) {
        *bytes             = 0;
        self->endOfStream_ = true;
        return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }
    *bytes = static_cast<std::size_t>(got);
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

FLAC__StreamDecoderSeekStatus FlacDecoder::seekCb(const FLAC__StreamDecoder*,
                                                  FLAC__uint64 offset,
                                                  void*        client) {
    auto* self = static_cast<FlacDecoder*>(client);
    if (!self->source_->seek(static_cast<std::int64_t>(offset), SEEK_SET)) {
        return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    }
    self->endOfStream_ = false;
    return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

FLAC__StreamDecoderTellStatus FlacDecoder::tellCb(const FLAC__StreamDecoder*,
                                                  FLAC__uint64* offset,
                                                  void*         client) {
    auto*              self = static_cast<FlacDecoder*>(client);
    const std::int64_t pos  = self->source_->tell();
    if (pos < 0) {
        return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
    }
    *offset = static_cast<FLAC__uint64>(pos);
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

FLAC__StreamDecoderLengthStatus FlacDecoder::lengthCb(const FLAC__StreamDecoder*,
                                                      FLAC__uint64* length,
                                                      void*         client) {
    auto* self = static_cast<FlacDecoder*>(client);
    if (!self->source_->seekable()) {
        *length = 0;
        return FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;
    }

    const std::int64_t saved = self->source_->tell();
    self->source_->seek(0, SEEK_END);
    *length = static_cast<FLAC__uint64>(self->source_->tell());
    self->source_->seek(saved, SEEK_SET);
    return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

FLAC__bool FlacDecoder::eofCb(const FLAC__StreamDecoder*, void* client) {
    return static_cast<FlacDecoder*>(client)->endOfStream_ ? 1 : 0;
}

void FlacDecoder::interleave(const FLAC__Frame*       frame,
                             const FLAC__int32* const buffer[]) {
    const std::uint32_t channels  = frame->header.channels;
    const std::uint32_t blocksize = frame->header.blocksize;
    const std::uint32_t bits      = frame->header.bits_per_sample;

    std::byte* out = block_.data();

    switch (containerFor(bits)) {
        case SampleFormat::S8:
            for (std::uint32_t s = 0; s < blocksize; ++s) {
                for (std::uint32_t c = 0; c < channels; ++c) {
                    const auto v = static_cast<std::int8_t>(buffer[c][s]);
                    std::memcpy(out, &v, 1);
                    out += 1;
                }
            }
            break;

        case SampleFormat::S16:
            for (std::uint32_t s = 0; s < blocksize; ++s) {
                for (std::uint32_t c = 0; c < channels; ++c) {
                    const auto v = static_cast<std::int16_t>(buffer[c][s]);
                    std::memcpy(out, &v, 2);
                    out += 2;
                }
            }
            break;

        case SampleFormat::S24:
            // Little-endian 3-byte packing, matching what WAV and `flac -d` write.
            for (std::uint32_t s = 0; s < blocksize; ++s) {
                for (std::uint32_t c = 0; c < channels; ++c) {
                    const std::int32_t v = buffer[c][s];
                    out[0] = static_cast<std::byte>(v & 0xFF);
                    out[1] = static_cast<std::byte>((v >> 8) & 0xFF);
                    out[2] = static_cast<std::byte>((v >> 16) & 0xFF);
                    out += 3;
                }
            }
            break;

        case SampleFormat::S32:
        default:
            for (std::uint32_t s = 0; s < blocksize; ++s) {
                for (std::uint32_t c = 0; c < channels; ++c) {
                    const std::int32_t v = buffer[c][s];
                    std::memcpy(out, &v, 4);
                    out += 4;
                }
            }
            break;
    }

    blockFrames_ = blocksize;
}

FLAC__StreamDecoderWriteStatus FlacDecoder::writeCb(const FLAC__StreamDecoder*,
                                                    const FLAC__Frame* frame,
                                                    const FLAC__int32* const buffer[],
                                                    void* client) {
    auto* self = static_cast<FlacDecoder*>(client);
    if (self->abort_) {
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    const std::uint32_t channels = frame->header.channels;
    const std::uint32_t bits     = frame->header.bits_per_sample;
    const auto          rate     = static_cast<double>(frame->header.sample_rate);

    // FLAC permits the stream format to change between frames; Cog reports this
    // through KVO on "properties" and we forward it the same way.
    if (channels != self->format_.channels || bits != self->format_.bitsPerSample ||
        rate != self->format_.sampleRate) {
        if (channels != self->format_.channels) {
            self->format_.channelConfig = 0;
        }
        self->format_.channels      = channels;
        self->format_.bitsPerSample = bits;
        self->format_.sampleRate    = rate;
        self->format_.format        = containerFor(bits);
        if (self->format_.channelConfig == 0) {
            self->format_.channelConfig = guessChannelConfig(channels);
        }
        self->notifyChanged(true, false);
    }

    self->interleave(frame, buffer);
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void FlacDecoder::metadataCb(const FLAC__StreamDecoder*,
                             const FLAC__StreamMetadata* metadata, void* client) {
    auto* self = static_cast<FlacDecoder*>(client);

    // Some files in the wild carry several STREAMINFO blocks and only the first
    // has sane values, so honour only the first. (Cog FlacDecoder.m:196-199.)
    if (!self->hasStreamInfo_ && metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
        const auto& info = metadata->data.stream_info;

        self->format_.channels      = info.channels;
        self->format_.sampleRate    = static_cast<double>(info.sample_rate);
        self->format_.bitsPerSample = info.bits_per_sample;
        self->format_.format        = containerFor(info.bits_per_sample);
        self->format_.bigEndian     = false;
        self->format_.channelConfig = guessChannelConfig(info.channels);

        self->totalFrames_   = static_cast<std::int64_t>(info.total_samples);
        self->hasStreamInfo_ = true;
    }

    if (metadata->type == FLAC__METADATA_TYPE_PICTURE) {
        const auto& picture = metadata->data.picture;
        const auto* begin   = reinterpret_cast<const std::byte*>(picture.data);
        self->albumArt_.assign(begin, begin + picture.data_length);
    }

    if (metadata->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
        const auto& comments = metadata->data.vorbis_comment;

        for (std::uint32_t i = 0; i < comments.num_comments; ++i) {
            char* rawName  = nullptr;
            char* rawValue = nullptr;
            if (!FLAC__metadata_object_vorbiscomment_entry_to_name_value_pair(
                    comments.comments[i], &rawName, &rawValue)) {
                continue;
            }

            const std::string name  = lowercased(rawName);
            const std::string value = rawValue;
            std::free(rawName);
            std::free(rawValue);

            if (name == "cuesheet") {
                self->cuesheet_      = value;
                self->cuesheetFound_ = true;
            } else if (name == "waveformatextensible_channel_mask") {
                if (value.starts_with("0x") || value.starts_with("0X")) {
                    self->format_.channelConfig =
                        static_cast<std::uint32_t>(std::strtoul(value.c_str() + 2,
                                                                nullptr, 16));
                }
            } else if (name == "replaygain_track_gain") {
                self->replayGain_.trackGain = parseFloat(value);
            } else if (name == "replaygain_track_peak") {
                self->replayGain_.trackPeak = parseFloat(value);
            } else if (name == "replaygain_album_gain") {
                self->replayGain_.albumGain = parseFloat(value);
            } else if (name == "replaygain_album_peak") {
                self->replayGain_.albumPeak = parseFloat(value);
            } else if (name == "unsynced lyrics" || name == "lyrics") {
                self->tags_.add("unsyncedlyrics", value);
            } else if (name == "comments:itunnorm") {
                self->replayGain_.soundcheck = value;
                self->tags_.add("soundcheck", value);
            } else {
                self->tags_.add(name, value);
            }
        }

        self->hasVorbisComment_ = true;
    }
}

void FlacDecoder::errorCb(const FLAC__StreamDecoder*,
                          FLAC__StreamDecoderErrorStatus status, void* client) {
    // A lost sync is recoverable; anything else aborts, as in Cog.
    if (status != FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC) {
        static_cast<FlacDecoder*>(client)->abort_ = true;
    }
}

// --- IDecoder -------------------------------------------------------------

bool FlacDecoder::open(ISource* source) {
    close();

    source_ = source;
    if (source_ == nullptr) {
        return false;
    }

    if (source_->seekable()) {
        source_->seek(0, SEEK_END);
        fileSize_ = source_->tell();
        source_->seek(0, SEEK_SET);
    }

    // Peek for an Ogg container. The HTTP source supports rewinding this far.
    bool          isOggFlac = false;
    unsigned char magic[4]  = {0, 0, 0, 0};
    if (source_->read(magic, 4) == 4) {
        isOggFlac = std::memcmp(magic, "OggS", 4) == 0;
    }
    source_->seek(0, SEEK_SET);

    decoder_ = FLAC__stream_decoder_new();
    if (decoder_ == nullptr) {
        return false;
    }

    if (!source_->seekable()) {
        FLAC__stream_decoder_set_md5_checking(decoder_, 0);
    }

    FLAC__stream_decoder_set_metadata_ignore_all(decoder_);
    FLAC__stream_decoder_set_metadata_respond(decoder_,
                                              FLAC__METADATA_TYPE_STREAMINFO);
    FLAC__stream_decoder_set_metadata_respond(decoder_,
                                              FLAC__METADATA_TYPE_VORBIS_COMMENT);
    FLAC__stream_decoder_set_metadata_respond(decoder_, FLAC__METADATA_TYPE_PICTURE);
    FLAC__stream_decoder_set_metadata_respond(decoder_, FLAC__METADATA_TYPE_CUESHEET);

    const bool seekable = source_->seekable();

    const auto init = isOggFlac
                          ? FLAC__stream_decoder_init_ogg_stream(
                                decoder_, readCb, seekable ? seekCb : nullptr,
                                seekable ? tellCb : nullptr,
                                seekable ? lengthCb : nullptr,
                                seekable ? eofCb : nullptr, writeCb, metadataCb,
                                errorCb, this)
                          : FLAC__stream_decoder_init_stream(
                                decoder_, readCb, seekable ? seekCb : nullptr,
                                seekable ? tellCb : nullptr,
                                seekable ? lengthCb : nullptr,
                                seekable ? eofCb : nullptr, writeCb, metadataCb,
                                errorCb, this);

    if (init != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        return false;
    }

    FLAC__stream_decoder_process_until_end_of_metadata(decoder_);

    if (!hasStreamInfo_ || !format_.valid()) {
        return false;
    }

    block_.resize(kMaxBlockBytes);
    blockFrames_ = 0;
    framePos_    = 0;
    seconds_     = 0.0;

    return true;
}

bool FlacDecoder::readAudio(AudioChunk& out) {
    while (blockFrames_ == 0) {
        if (abort_) {
            return false;
        }

        const FLAC__StreamDecoderState state =
            FLAC__stream_decoder_get_state(decoder_);

        if (state == FLAC__STREAM_DECODER_END_OF_STREAM) {
            return false;
        }
        if (state == FLAC__STREAM_DECODER_END_OF_LINK) {
            // Chained Ogg FLAC: finish this link and continue into the next.
            if (!FLAC__stream_decoder_finish_link(decoder_)) {
                return false;
            }
        }
        if (!FLAC__stream_decoder_process_single(decoder_)) {
            return false;
        }
    }

    out.clear();
    out.setFormat(format_);
    out.lossless        = true;
    out.streamTimestamp = seconds_;
    out.streamTimeRatio = 1.0;
    out.assign(block_.data(), blockFrames_);

    framePos_ += static_cast<std::int64_t>(blockFrames_);
    seconds_ += out.duration();
    blockFrames_ = 0;

    return true;
}

std::int64_t FlacDecoder::seek(std::int64_t frame) {
    if (decoder_ == nullptr) {
        return -1;
    }
    if (!FLAC__stream_decoder_seek_absolute(decoder_,
                                            static_cast<FLAC__uint64>(frame))) {
        return -1;
    }

    // The seek discards whatever was buffered, so drop it rather than emitting
    // audio from the old position. Cog leaves this stale.
    blockFrames_ = 0;
    framePos_    = frame;
    seconds_     = (format_.sampleRate > 0.0)
                       ? static_cast<double>(frame) / format_.sampleRate
                       : 0.0;
    return frame;
}

void FlacDecoder::close() {
    if (decoder_ != nullptr) {
        FLAC__stream_decoder_finish(decoder_);
        FLAC__stream_decoder_delete(decoder_);
        decoder_ = nullptr;
    }
    block_.clear();
    block_.shrink_to_fit();
    blockFrames_ = 0;
}

void FlacDecoder::interrupt() {
    if (source_ != nullptr) {
        source_->interrupt();
    }
}

TrackProperties FlacDecoder::properties() const {
    TrackProperties props;
    props.format      = format_;
    props.totalFrames = totalFrames_;
    props.seekable    = source_ != nullptr && source_->seekable();
    props.lossless    = true;
    props.codec       = "FLAC";
    props.encoding    = "lossless";
    props.replayGain  = replayGain_;

    if (!cuesheet_.empty()) {
        props.cuesheet = cuesheet_;
    }

    // Cog: fileSize * 8 / duration / 1000, guarding a zero-length stream.
    if (fileSize_ > 0 && totalFrames_ > 0 && format_.sampleRate > 0.0) {
        const double seconds =
            static_cast<double>(totalFrames_) / format_.sampleRate;
        props.bitrateKbps =
            static_cast<std::int32_t>((static_cast<double>(fileSize_) * 8.0) /
                                      seconds / 1000.0);
    }

    return props;
}

MetadataMap FlacDecoder::metadata() const {
    MetadataMap out = tags_;
    if (!cuesheet_.empty()) {
        out.set("cuesheet", cuesheet_);
    }
    if (!albumArt_.empty()) {
        out.setBytes("albumart", albumArt_);
    }
    return out;
}

}  // namespace xpcog
