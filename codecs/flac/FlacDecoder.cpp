#include "FlacDecoder.hpp"

#include "common/CueSheet.hpp"

#include "../common/OggChain.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
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

/// How far a sample has to move left to sit at the top of that container.
///
/// libFLAC hands back a 32-bit integer carrying the sample in its N least
/// significant bits, sign-extended -- so a 12-bit sample spans about +/-2^11,
/// not +/-2^15. Writing that into a 16-bit container unchanged is a recording
/// four times too quiet, because everything downstream scales by the container
/// and has no reason to ask what the source depth was. Moving the valid bits up
/// to meet the container is exact, being a multiply by a power of two, and it is
/// what Cog's own default case does: it pads to the nearest byte upward and
/// emits the sample left-aligned (FlacDecoder.m:161-176).
[[nodiscard]] std::uint32_t containerShift(std::uint32_t bitsPerSample) {
    const std::uint32_t containerBits = ((bitsPerSample + 7U) / 8U) * 8U;
    return containerBits > bitsPerSample ? containerBits - bitsPerSample : 0U;
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

    // Clamped to the link being decoded. Together with the offsets in seekCb,
    // tellCb and lengthCb this presents libFLAC with a virtual file containing
    // exactly this link, so a read or a seek cannot leave it by construction.
    //
    // Belt and braces, and worth knowing which: libFLAC would stop at the link's
    // end anyway, because chained decoding is off here and its demuxer ignores
    // pages carrying another serial number. Removing this clamp changes nothing
    // observable on a well-formed file -- verified, not assumed. It is kept
    // because that leaves containment resting on a rule of the Ogg spec and on
    // libFLAC's enforcement of it, where this makes it structural.
    auto want = static_cast<std::int64_t>(*bytes);
    if (self->linkEnd_ >= 0) {
        const std::int64_t remaining = self->linkEnd_ - self->source_->tell();
        if (remaining <= 0) {
            *bytes             = 0;
            self->endOfStream_ = true;
            return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
        }
        want = std::min(want, remaining);
    }

    const std::int64_t got = self->source_->read(buffer, want);
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
    // Offsets are the link's, not the file's: libFLAC is decoding what it
    // believes is a whole stream starting at zero.
    if (!self->source_->seek(self->linkBegin_ + static_cast<std::int64_t>(offset),
                             SEEK_SET)) {
        return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    }
    self->endOfStream_ = false;
    return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

FLAC__StreamDecoderTellStatus FlacDecoder::tellCb(const FLAC__StreamDecoder*,
                                                  FLAC__uint64* offset,
                                                  void*         client) {
    auto*              self = static_cast<FlacDecoder*>(client);
    const std::int64_t pos  = self->source_->tell() - self->linkBegin_;
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

    if (self->linkEnd_ >= 0) {
        *length = static_cast<FLAC__uint64>(self->linkEnd_ - self->linkBegin_);
        return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
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
    const std::uint32_t shift     = containerShift(bits);

    std::byte* out = block_.data();

    // Through unsigned, so shifting a negative sample stays arithmetic rather
    // than relying on how signed overflow is defined.
    const auto aligned = [shift](FLAC__int32 sample) {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(sample) << shift);
    };

    switch (containerFor(bits)) {
        case SampleFormat::S8:
            for (std::uint32_t s = 0; s < blocksize; ++s) {
                for (std::uint32_t c = 0; c < channels; ++c) {
                    const auto v = static_cast<std::int8_t>(aligned(buffer[c][s]));
                    std::memcpy(out, &v, 1);
                    out += 1;
                }
            }
            break;

        case SampleFormat::S16:
            for (std::uint32_t s = 0; s < blocksize; ++s) {
                for (std::uint32_t c = 0; c < channels; ++c) {
                    const auto v = static_cast<std::int16_t>(aligned(buffer[c][s]));
                    std::memcpy(out, &v, 2);
                    out += 2;
                }
            }
            break;

        case SampleFormat::S24:
            // Little-endian 3-byte packing, matching what WAV and `flac -d` write.
            for (std::uint32_t s = 0; s < blocksize; ++s) {
                for (std::uint32_t c = 0; c < channels; ++c) {
                    const std::int32_t v = aligned(buffer[c][s]);
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
                    const std::int32_t v = aligned(buffer[c][s]);
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

    if (metadata->type == FLAC__METADATA_TYPE_CUESHEET) {
        // The offsets-only sheet. It carries no titles, so a file that also has
        // a CUESHEET tag is better served by that; this is what is left when it
        // does not, and it is still enough to cut the album into tracks.
        const auto& sheet = metadata->data.cue_sheet;
        self->cueTrackOffsets_.clear();

        for (std::uint32_t t = 0; t < sheet.num_tracks; ++t) {
            const auto& track = sheet.tracks[t];
            // The lead-out closes the sheet rather than naming a track. CD-DA
            // numbers it 170, and a non-CD sheet 255.
            if (track.num_indices == 0 || track.number == 170 || track.number == 255) {
                continue;
            }

            // INDEX 01 is where the music starts; INDEX 00 is the pre-gap, and
            // belongs to the track before it as far as playback is concerned.
            auto offset = static_cast<std::int64_t>(track.offset);
            for (std::uint32_t i = 0; i < track.num_indices; ++i) {
                if (track.indices[i].number == 1) {
                    offset += static_cast<std::int64_t>(track.indices[i].offset);
                    break;
                }
            }

            // Two digits, because that is how a cue sheet writes a track number
            // and the fragment has to match either spelling of the same track.
            std::string number = std::to_string(track.number);
            if (number.size() < 2) {
                number.insert(number.begin(), '0');
            }
            self->cueTrackOffsets_.emplace_back(std::move(number), offset);
        }
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

    const bool seekable = source_->seekable();

    // A chained file is a container: every link is a whole track with its own
    // headers, and the URL's fragment says which one to play. Handing libFLAC
    // just that link's bytes means it reads an ordinary single stream, so the
    // link's own STREAMINFO, Vorbis comment, length and seek table all arrive
    // through the paths that already work -- which decoding the chain
    // end-to-end would not give, since seeking into a link does not re-deliver
    // its headers.
    //
    // looksChained() first: it is two reads, where readOggLinks() walks every
    // page header in the file. An ordinary single-stream .ogg must not pay for
    // that at every open.
    linkBegin_ = 0;
    linkEnd_   = -1;
    if (isOggFlac && seekable && codecs::looksChained(*source_)) {
        const std::vector<codecs::OggLink> links = codecs::readOggLinks(*source_);
        if (links.size() > 1) {
            const std::size_t index =
                std::min(codecs::oggLinkFromFragment(source_->url()), links.size() - 1);
            linkBegin_ = links[index].begin;
            linkEnd_   = links[index].end;
            fileSize_  = linkEnd_ - linkBegin_;
        }
    }
    if (!source_->seek(linkBegin_, SEEK_SET)) {
        return false;
    }

    decoder_ = FLAC__stream_decoder_new();
    if (decoder_ == nullptr) {
        return false;
    }

    if (!seekable) {
        FLAC__stream_decoder_set_md5_checking(decoder_, 0);
    }

    // A live Ogg FLAC stream restarts its bitstream at every track change: the
    // encoder ends one logical stream and begins another, with a fresh serial
    // number, STREAMINFO and Vorbis comment. libFLAC calls those chain links,
    // and *by default it stops at the end of the first one* -- which on an
    // Icecast station means playback ends when the first song does. The
    // END_OF_LINK state the read loop handles is only ever reported when this
    // is on, so without it that branch is unreachable.
    //
    // Streams only. A seekable chained file is expanded into one track per link
    // instead (see above and OggChain.hpp), which gives each its own name and
    // length; decoding the chain straight through would give one nameless track
    // of everything.
    if (isOggFlac && !seekable) {
        FLAC__stream_decoder_set_decode_chained_stream(decoder_, 1);
    }

    FLAC__stream_decoder_set_metadata_ignore_all(decoder_);
    FLAC__stream_decoder_set_metadata_respond(decoder_,
                                              FLAC__METADATA_TYPE_STREAMINFO);
    FLAC__stream_decoder_set_metadata_respond(decoder_,
                                              FLAC__METADATA_TYPE_VORBIS_COMMENT);
    FLAC__stream_decoder_set_metadata_respond(decoder_, FLAC__METADATA_TYPE_PICTURE);
    FLAC__stream_decoder_set_metadata_respond(decoder_, FLAC__METADATA_TYPE_CUESHEET);

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

    // After the metadata, because that is where the sheet comes from.
    applyCueTrack(source_->url());

    return true;
}

void FlacDecoder::applyCueTrack(const Url& url) {
    trackStart_ = 0;
    trackEnd_   = -1;
    hasTrack_   = false;
    trackTags_  = {};
    trackGain_  = {};

    const std::string_view fragment = url.fragment();
    if (fragment.empty()) {
        return;
    }

    const double rate = format_.sampleRate;

    // The tag sheet first: it is the one with titles on it.
    if (!cuesheet_.empty()) {
        const codecs::CueSheet sheet = codecs::CueSheet::parse(cuesheet_, url);
        if (const codecs::CueTrack* track = sheet.findTrack(fragment)) {
            const codecs::CueTrack* next = sheet.nextInSameFile(sheet.indexOf(track));

            trackStart_ = track->startFrame(rate);
            trackEnd_   = (next != nullptr) ? next->startFrame(rate) : totalFrames_;
            trackTags_  = track->metadata();
            trackGain_  = track->replayGain;
            hasTrack_   = true;
        }
    }

    if (!hasTrack_) {
        for (std::size_t i = 0; i < cueTrackOffsets_.size(); ++i) {
            if (cueTrackOffsets_[i].first != fragment) {
                continue;
            }
            trackStart_ = cueTrackOffsets_[i].second;
            trackEnd_   = (i + 1 < cueTrackOffsets_.size())
                              ? cueTrackOffsets_[i + 1].second
                              : totalFrames_;
            trackTags_.set("tracknumber", {cueTrackOffsets_[i].first});
            hasTrack_ = true;
            break;
        }
    }

    // A sheet naming a track that does not fit the audio is not one to trust.
    // Playing the whole file is wrong, but it is wrong audibly rather than by
    // returning nothing at all.
    if (hasTrack_ && (trackStart_ < 0 || trackEnd_ <= trackStart_)) {
        trackStart_ = 0;
        trackEnd_   = -1;
        hasTrack_   = false;
        trackTags_  = {};
        trackGain_  = {};
    }

    if (hasTrack_) {
        static_cast<void>(seek(0));
    }
}

bool FlacDecoder::readAudio(AudioChunk& out) {
    // The track ended even though the file has not. Checked before decoding, so
    // the last block of a track is not paid for twice over.
    if (trackEnd_ >= 0 && framePos_ >= trackEnd_) {
        return false;
    }

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
            beginLink();
            if (!FLAC__stream_decoder_finish_link(decoder_)) {
                return false;
            }
        }
        if (!FLAC__stream_decoder_process_single(decoder_)) {
            return false;
        }
    }

    // Announced here rather than from the metadata callback: a link's blocks
    // arrive one at a time, and the tags are only complete once a frame follows
    // them. Reporting per block would name the track from a half-read comment.
    if (linkChanged_) {
        linkChanged_ = false;
        notifyChanged(false, true);
    }

    // A block straddling the boundary is cut at it, or the track bleeds into
    // the one after -- which on an album in a single file is every track.
    std::size_t frames = blockFrames_;
    if (trackEnd_ >= 0) {
        const std::int64_t remaining = trackEnd_ - framePos_;
        if (static_cast<std::int64_t>(frames) > remaining) {
            frames = static_cast<std::size_t>(remaining);
        }
    }

    out.clear();
    out.setFormat(format_);
    out.lossless        = true;
    out.streamTimestamp = seconds_;
    out.streamTimeRatio = 1.0;
    out.assign(block_.data(), frames);

    framePos_ += static_cast<std::int64_t>(frames);
    seconds_ += out.duration();
    blockFrames_ = 0;

    return frames > 0;
}

std::int64_t FlacDecoder::seek(std::int64_t frame) {
    if (decoder_ == nullptr) {
        return -1;
    }

    // Callers seek within the track; libFLAC only knows the file.
    const std::int64_t target = trackStart_ + frame;

    // Drop stale pre-seek audio BEFORE seeking, never after.
    //
    // FLAC__stream_decoder_seek_absolute decodes the block containing the target
    // sample and hands it to the write callback as part of seeking. Clearing
    // blockFrames_ afterwards therefore discards exactly the data the seek just
    // produced, losing up to a block at every seek -- which is what made cue
    // sheet tracks start late and decode to the wrong bytes.
    blockFrames_ = 0;

    if (!FLAC__stream_decoder_seek_absolute(decoder_,
                                            static_cast<FLAC__uint64>(target))) {
        return -1;
    }

    framePos_ = target;
    seconds_     = (format_.sampleRate > 0.0)
                       ? static_cast<double>(frame) / format_.sampleRate
                       : 0.0;
    return frame;
}

void FlacDecoder::beginLink() {
    // A chain link is the next track, and its Vorbis comment describes that one
    // rather than the one just finished. These accumulate -- tags_ appends
    // repeated names, because a FLAC file may legitimately carry several ARTIST
    // lines -- so without clearing them a station's tag list grows by a whole
    // track every few minutes and the title is whichever song came first.
    tags_             = {};
    replayGain_       = {};
    albumArt_.clear();
    cuesheet_.clear();
    cueTrackOffsets_.clear();
    cuesheetFound_    = false;
    hasVorbisComment_ = false;
    // The new link carries its own STREAMINFO, which is not the "several
    // STREAMINFO blocks in one stream" case the metadata callback guards against.
    hasStreamInfo_    = false;
    linkChanged_      = true;
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

    // The per-stream state too, because open() begins by calling this and tags_
    // appends rather than replaces -- a second open on the same decoder would
    // otherwise report both files' tags and the first one's cue sheet.
    tags_       = {};
    replayGain_ = {};
    albumArt_.clear();
    cuesheet_.clear();
    cueTrackOffsets_.clear();
    cuesheetFound_    = false;
    hasVorbisComment_ = false;
    hasStreamInfo_    = false;
    trackStart_       = 0;
    trackEnd_         = -1;
    hasTrack_         = false;
    trackTags_        = {};
    trackGain_        = {};
}

void FlacDecoder::interrupt() {
    if (source_ != nullptr) {
        source_->interrupt();
    }
}

TrackProperties FlacDecoder::properties() const {
    TrackProperties props;
    props.format      = format_;
    props.totalFrames = hasTrack_ ? trackEnd_ - trackStart_ : totalFrames_;
    props.seekable    = source_ != nullptr && source_->seekable();
    props.lossless    = true;
    props.codec       = "FLAC";
    props.encoding    = "lossless";
    props.replayGain  = replayGain_;

    // The track's own gain wins field by field, so a sheet that names only a
    // track gain does not throw away the file's album gain along with it.
    if (hasTrack_) {
        if (trackGain_.trackGain) props.replayGain.trackGain = trackGain_.trackGain;
        if (trackGain_.trackPeak) props.replayGain.trackPeak = trackGain_.trackPeak;
        if (trackGain_.albumGain) props.replayGain.albumGain = trackGain_.albumGain;
        if (trackGain_.albumPeak) props.replayGain.albumPeak = trackGain_.albumPeak;
    }

    // Only for the file as a whole. Reporting it for a track would offer the
    // sheet back to whatever expands containers, which has already used it.
    if (!cuesheet_.empty() && !hasTrack_) {
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

std::vector<std::string> FlacDecoder::cueTracks() const {
    // The tag sheet first, for the same reason applyCueTrack prefers it: the
    // fragments have to be the ones that will later be looked up in it.
    if (!cuesheet_.empty() && source_ != nullptr) {
        const codecs::CueSheet sheet = codecs::CueSheet::parse(cuesheet_, source_->url());

        std::vector<std::string> tracks;
        tracks.reserve(sheet.tracks().size());
        for (const codecs::CueTrack& track : sheet.tracks()) {
            tracks.push_back(track.track);
        }
        if (!tracks.empty()) {
            return tracks;
        }
    }

    std::vector<std::string> tracks;
    tracks.reserve(cueTrackOffsets_.size());
    for (const auto& [number, offset] : cueTrackOffsets_) {
        static_cast<void>(offset);
        tracks.push_back(number);
    }
    return tracks;
}

MetadataMap FlacDecoder::metadata() const {
    MetadataMap out = tags_;

    // The track's tags replace the file's where they overlap: on an album in one
    // file the file-level TITLE is the album's, and the track's is the song's.
    for (const auto& [key, value] : trackTags_) {
        if (const auto* strings = std::get_if<std::vector<std::string>>(&value)) {
            out.set(key, *strings);
        } else if (const auto* bytes = std::get_if<std::vector<std::byte>>(&value)) {
            out.setBytes(key, *bytes);
        }
    }

    if (!cuesheet_.empty() && !hasTrack_) {
        out.set("cuesheet", cuesheet_);
    }
    if (!albumArt_.empty()) {
        out.setBytes("albumart", albumArt_);
    }
    return out;
}

}  // namespace xpcog
