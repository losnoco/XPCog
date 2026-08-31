// The remote control's access token, kept where a credential belongs.
//
// wxSecretStore, not settings.def, and for the reason LastFmAccount.hpp already
// gives about the Last.fm session key: settings are a registry key or a plist --
// readable text, backed up, synced -- and a token that grants control of the
// player and read access to anything it can decode is not something to leave
// there.
//
// The fallback that is deliberately absent: if there is no secret store, the
// server does not start and the preferences pane says why. Writing the token to
// settings instead would be the convenient thing and would quietly undo the
// reason for this file, so it is refused here rather than left for someone to
// add later as an improvement.

#pragma once

#include <wx/string.h>

#include <string>

namespace xpcog::app {

class RemoteToken {
public:
    /// Whether a token can be kept at all. `why` is filled in when not, for the
    /// preferences pane to show.
    [[nodiscard]] static bool storeAvailable(wxString* why = nullptr);

    /// The stored token, or empty when there is none.
    [[nodiscard]] static std::string load();

    /// Stores `token`. False when the store refused it.
    static bool save(const std::string& token);

    /// The stored token, generating and storing one if there is none.
    ///
    /// Empty when there is no store, or when the system's random generator would
    /// not answer -- neither of which is a case to paper over with something
    /// guessable.
    [[nodiscard]] static std::string ensure();

    /// Throws the current token away and makes a new one. Breaks every connected
    /// client, which is the point of the button that calls it.
    [[nodiscard]] static std::string regenerate();

private:
    [[nodiscard]] static wxString serviceName();
};

}  // namespace xpcog::app
