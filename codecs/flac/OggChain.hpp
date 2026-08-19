// Walking the Ogg container to find where one logical bitstream ends and the
// next begins.
//
// A chained Ogg file is several complete streams concatenated -- which is what
// an Icecast station sends across a track change, and what `cat a.oga b.oga`
// produces. Each is a whole track with its own STREAMINFO, Vorbis comment and
// sample count, so a file of them is a container in exactly the sense the
// registry already means: one URL in, one per track out.
//
// libFLAC 1.5 can decode a chain end to end, but it offers no way to ask what
// the links *are* -- their lengths only after indexing the whole file, and their
// metadata not at all, since seeking into a link does not re-deliver its
// headers. Reading the page structure gives all of it directly, and lets each
// link be handed to libFLAC as the ordinary single stream it is.
//
// Page headers only: the segment table says how long a page's payload is, so the
// walk seeks over the audio rather than reading it.

#pragma once

#include "xpcog/core/Plugin.hpp"

#include <cstdint>
#include <vector>

namespace xpcog::codecs {

/// One logical bitstream within an Ogg file.
struct OggLink {
    std::int64_t  begin  = 0;  ///< offset of the page carrying its first packet
    std::int64_t  end    = 0;  ///< one past its last page
    std::uint32_t serial = 0;
};

/// True when `source` starts with an Ogg page whose first packet is the Ogg FLAC
/// mapping header. Distinguishes an Ogg FLAC file from Ogg Vorbis or Opus, which
/// wear the same extensions. Leaves the read position at the start.
[[nodiscard]] bool isOggFlacStream(ISource& source);

/// Whether the file plausibly holds more than one logical bitstream, decided by
/// comparing the first page's serial number with the last page's.
///
/// Two small reads rather than a walk. Every `.ogg` and `.oga` in a library would
/// otherwise be walked end to end at scan time just to learn it has one link.
///
/// Sound because RFC 3533 requires every logical bitstream in a physical one to
/// have a distinct serial number -- so a file whose first and last pages agree
/// has one link. A file that breaks that rule (`cat x.oga x.oga`, which repeats
/// the serial rather than choosing a new one) reads as unchained and plays as
/// its first link. That is also the rule libFLAC's demuxer relies on to ignore
/// pages belonging to another link, so a file violating it has no correct
/// reading available here anyway.
///
/// Conservative: anything it cannot read cleanly answers false, which leaves the
/// file treated as the single track it almost certainly is.
[[nodiscard]] bool looksChained(ISource& source);

/// Every logical bitstream in `source`, in order. Empty when the file is not
/// Ogg or its page structure does not hold together.
[[nodiscard]] std::vector<OggLink> readOggLinks(ISource& source);

}  // namespace xpcog::codecs
