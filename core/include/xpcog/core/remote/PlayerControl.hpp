// What the remote control is allowed to ask the player to do.
//
// The server lives in core and the player does not: the transport is
// PlaybackController in app/, above the toolkit line, and the playlist, the undo
// stack and the library are all reached through the window. So core names an
// interface and the layer that has those things implements it -- app/ over
// PlaybackController and AppCommands, xpcog-cli over an AudioEngine and a
// SerialExecutor.
//
// Two rules hold this shape, and both are load-bearing:
//
// **No JSON here.** Methods speak std:: types and TrackId. nlohmann is linked
// PRIVATE to xpcog-core deliberately -- LastFmClient.hpp speaks std::string for
// the same reason -- and a public header naming it would end that. Serialisation
// happens in core/src/remote, on the HTTP thread, from the plain structs these
// return.
//
// **Every method runs on the interface thread, one at a time.** CallGate puts
// them there and waits. So an implementation may touch Playlist, UndoStack,
// Library and Settings unlocked, exactly as a menu handler does, and must not
// assume anything else about which thread it is on.
//
// This is the interface's first shape: a destructor and nothing else, so the
// server can hold a reference to something real before there is anything for it
// to ask. The transport, playlist, settings and DSP methods land with the routes
// that call them.

#pragma once

namespace xpcog::remote {

class IPlayerControl {
public:
    virtual ~IPlayerControl() = default;

    IPlayerControl()                                 = default;
    IPlayerControl(const IPlayerControl&)            = delete;
    IPlayerControl& operator=(const IPlayerControl&) = delete;
};

}  // namespace xpcog::remote
