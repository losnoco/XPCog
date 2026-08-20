// How long a Shorten file is.
//
// Shorten (.shn) compresses a WAV file, and it keeps the WAV file's own header
// verbatim: the very first command in the stream is FN_VERBATIM carrying the
// original forty-odd bytes of RIFF, and the `data` chunk size in there is the
// only statement of length the format makes. Everything after it is Rice-coded
// residuals with no index, no frame count and no total.
//
// FFmpeg demuxes and decodes shorten -- and does it through an AVIO context, so
// a .shn inside an archive or behind HTTP plays, which is more than Cog's own
// plugin manages -- but its demuxer does not read that header, so a .shn opens
// with no duration at all. A lossless music file that shows 0:00 in a playlist
// and cannot be scrubbed is a poor result for the sake of one number that is
// sitting in the first kilobyte of the file.
//
// So the first kilobyte is parsed here. That means implementing enough of
// shorten's bitstream to reach the first command -- a Rice/Golomb reader and
// six header fields -- and then a plain RIFF walk. It is about a hundred lines
// and it decodes nothing: the moment the WAV header has been read, this stops.
//
// Cog gets the same number from xmms-shn's shn_get_song_length(), which reads
// the same field after the same parse; the difference is that Cog's copy of
// that parser comes with six thousand lines of threaded ring-buffered decoder
// attached.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace xpcog::codecs {

struct ShortenLength {
    /// Sample frames the wrapped RIFF header claims. A damaged or truncated
    /// file will claim more than it can deliver, which is the file's statement
    /// about itself and not something to correct here.
    std::int64_t   frames        = 0;
    std::uint32_t  channels      = 0;
    std::uint32_t  sampleRate    = 0;
    std::uint32_t  bitsPerSample = 0;
};

/// Reads the length out of the RIFF header a .shn wraps.
///
/// `data` should be the start of the file; a kilobyte is ample -- the verbatim
/// header sits before any audio. Returns nullopt for anything that is not a
/// shorten file, for a shorten file that does not start with a WAV header (the
/// format can also wrap AIFF or raw samples, and those carry no length here),
/// and for a header whose fields do not agree with each other.
[[nodiscard]] std::optional<ShortenLength> readShortenLength(
    std::span<const std::byte> data);

}  // namespace xpcog::codecs
