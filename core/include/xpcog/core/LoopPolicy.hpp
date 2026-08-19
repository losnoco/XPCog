// Whether a track that declares a loop is allowed to play it for ever.
//
// Sequenced, chiptune and emulated formats mostly do not end. A tracker module
// repeats, a chiptune's play routine is called until something stops it, and a
// game rip loops its music section because the game did. What ends them here is
// a length and a fade, invented from the rip's own tags or from a setting -- so
// "the track is over" is a decision this player makes, not something the file
// says.
//
// Cog lets the listener overrule that: with repeat-one set, a looping format
// plays for ever instead of fading out, which is the only way to actually sit
// and listen to a piece of game music the way it was meant to be heard. The
// decoders check the repeat setting directly (Playlist/PlaylistController.h,
// IsRepeatOneSet) and so do these, through the same setting key.
//
// The override exists for everything that is not a listener. A converter or a
// disk writer asking a decoder for a track wants the track, not an endless
// stream -- and it would get one, silently, because whoever ran it happened to
// leave repeat-one on.

#pragma once

#include <cstdint>

namespace xpcog {

class Settings;

enum class LoopPolicy : std::uint8_t {
    /// Follow the player: loop for ever when repeat-one is set. The default,
    /// because the ordinary caller is playback.
    Player,
    /// Never loop, whatever the player is set to. For callers that need a track
    /// to end -- conversion, writing a file, anything measuring a length.
    Never,
};

/// True when a looping track should play for ever rather than fade and stop.
///
/// Answered live rather than latched at open, because the listener can toggle
/// repeat-one in the middle of a track and expects it to take effect -- which is
/// what Cog does, re-checking on every read.
[[nodiscard]] bool loopsForever(const Settings* settings, LoopPolicy policy);

}  // namespace xpcog
