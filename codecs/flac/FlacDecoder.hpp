#pragma once

#include "xpcog/core/Plugin.hpp"

#include <FLAC/metadata.h>
#include <FLAC/stream_decoder.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace xpcog {

/// Port of Cog Plugins/Flac/FlacDecoder.m.
///
/// Deliberate deviation: Cog emits big-endian samples ("endian": "big") and lets
/// ConverterNode swap them. This emits native-endian, which the AudioFormat
/// describes accurately, avoids a byte swap per sample, and makes the output
/// directly comparable with `flac -d`.
class FlacDecoder final : public IDecoder {
public:
    ~FlacDecoder() override;

    bool         open(ISource* source) override;
    bool         readAudio(AudioChunk& out) override;
    std::int64_t seek(std::int64_t frame) override;
    void         close() override;
    void         interrupt() override;

    [[nodiscard]] TrackProperties properties() const override;
    [[nodiscard]] MetadataMap     metadata() const override;

    /// The cue sheet tracks this file holds, as URL fragments, in playing order.
    /// Empty when it is one track, which is every ordinary file. Valid after
    /// open(); this is what the container expansion enumerates.
    [[nodiscard]] std::vector<std::string> cueTracks() const;

private:
    // libFLAC callbacks. `client` is always the FlacDecoder.
    static FLAC__StreamDecoderReadStatus readCb(const FLAC__StreamDecoder*,
                                                FLAC__byte[], std::size_t*, void*);
    static FLAC__StreamDecoderSeekStatus seekCb(const FLAC__StreamDecoder*,
                                                FLAC__uint64, void*);
    static FLAC__StreamDecoderTellStatus tellCb(const FLAC__StreamDecoder*,
                                                FLAC__uint64*, void*);
    static FLAC__StreamDecoderLengthStatus lengthCb(const FLAC__StreamDecoder*,
                                                    FLAC__uint64*, void*);
    static FLAC__bool                      eofCb(const FLAC__StreamDecoder*, void*);
    static FLAC__StreamDecoderWriteStatus  writeCb(const FLAC__StreamDecoder*,
                                                   const FLAC__Frame*,
                                                   const FLAC__int32* const[], void*);
    static void metadataCb(const FLAC__StreamDecoder*, const FLAC__StreamMetadata*,
                           void*);
    static void errorCb(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus,
                        void*);

    void interleave(const FLAC__Frame* frame, const FLAC__int32* const buffer[]);

    /// Narrows the decoder to the cue sheet track named by `url`'s fragment.
    ///
    /// A FLAC carrying a cue sheet is a whole album in one file, and the
    /// fragment says which track of it was asked for. Without one -- or without
    /// a sheet -- the whole file stays the track, which is every ordinary file.
    void applyCueTrack(const Url& url);

    /// Drops the finished link's per-track state, so the next one's metadata
    /// replaces it rather than piling on top.
    void beginLink();

    FLAC__StreamDecoder* decoder_ = nullptr;
    ISource*             source_  = nullptr;

    // Decoded frames wait here between writeCb and readAudio.
    std::vector<std::byte> block_;
    std::size_t            blockFrames_ = 0;

    AudioFormat  format_{};
    std::int64_t totalFrames_ = 0;
    std::int64_t fileSize_    = 0;
    /// The byte range of the chain link being decoded. `linkEnd_` is -1 when the
    /// whole source is the stream, which is every unchained file and every live
    /// stream; otherwise reads and seeks are confined to this window so libFLAC
    /// sees one complete Ogg stream.
    std::int64_t linkBegin_   = 0;
    std::int64_t linkEnd_     = -1;
    std::int64_t framePos_    = 0;
    double       seconds_     = 0.0;

    bool hasStreamInfo_   = false;
    bool endOfStream_     = false;
    bool abort_           = false;
    bool cuesheetFound_   = false;
    bool hasVorbisComment_ = false;
    /// A chain link boundary was crossed and the new link's tags are not yet
    /// announced. Cleared by readAudio() once a frame proves them complete.
    bool linkChanged_     = false;

    std::string            cuesheet_;
    /// INDEX 01 of each track in the binary CUESHEET block, against its track
    /// number, in order. The lead-out is not among them: it marks where the
    /// audio stops rather than a track anybody plays.
    std::vector<std::pair<std::string, std::int64_t>> cueTrackOffsets_;

    /// The cue sheet track being decoded. `trackEnd_` is -1 when the whole file
    /// is the track, which is the ordinary case.
    std::int64_t   trackStart_ = 0;
    std::int64_t   trackEnd_   = -1;
    bool           hasTrack_   = false;
    MetadataMap    trackTags_;
    ReplayGainInfo trackGain_;

    std::vector<std::byte> albumArt_;
    MetadataMap            tags_;
    ReplayGainInfo         replayGain_;
};

}  // namespace xpcog
