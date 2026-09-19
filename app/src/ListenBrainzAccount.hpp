// The listener's ListenBrainz connection: where the user token is kept, and
// the one round trip that turns a pasted token into a connected account.
//
// The counterpart of LastFmAccount, and simpler in the same three ways the
// client is (see core's ListenBrainzClient.hpp): there is no application key to
// carry, no browser to open, and no polling. The listener copies the token from
// listenbrainz.org/settings, pastes it in, and `connect()` asks the server
// whose it is. What comes back is the username, and the two are stored
// together as one wxSecretStore record -- the same shape LastFmAccount uses and
// for the same reason: a (service, username, secret) triple is what the store
// holds, and one write cannot half-succeed.
//
// The token is a credential and never goes in the settings. The server's
// address does: it is configuration, it is not secret, and a listener running
// their own ListenBrainz or Maloja wants to see and edit it. So `listenBrainzUrl`
// lives in settings.def and is handed to the client here on every change.

#pragma once

#include "xpcog/core/scrobble/Scrobbler.hpp"

#include <wx/string.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace xpcog {
class IHttpClient;
class ListenBrainzClient;
}  // namespace xpcog

namespace xpcog::app {

class ListenBrainzAccount {
public:
    explicit ListenBrainzAccount(std::string apiRoot);
    ~ListenBrainzAccount();

    ListenBrainzAccount(const ListenBrainzAccount&)            = delete;
    ListenBrainzAccount& operator=(const ListenBrainzAccount&) = delete;

    /// Whether there is an HTTP client at all. There is no key to be missing,
    /// so this is the only way the account can be unusable.
    [[nodiscard]] bool usable() const;

    /// Why `usable()` is false, phrased for the preferences pane. Empty when it
    /// is true.
    [[nodiscard]] wxString unavailableReason() const;

    /// The stored token and its owner, or an empty session.
    [[nodiscard]] Scrobbler::Session load() const;

    /// Replaces the stored session. False when the store refused, which the
    /// caller must not treat as success.
    bool save(const Scrobbler::Session& session);

    /// Removes the stored session.
    void forget();

    /// Points the client at another server. Called with the setting's value
    /// at start and again whenever it changes.
    void setApiRoot(const std::string& apiRoot);

    /// The client, for the scrobbler. Borrowed; never null.
    [[nodiscard]] ListenBrainzClient& client() const { return *client_; }

    // --- connecting ------------------------------------------------------

    /// What a connection attempt reports back. **Both are called on the
    /// interface's thread**, marshalled through the dispatcher given to
    /// `connect()`, so a handler may touch widgets directly.
    struct ConnectHandlers {
        /// Validated and stored.
        std::function<void(const Scrobbler::Session&)> connected;
        /// The server did not know the token, could not be reached, or the
        /// store refused. `message` is for display.
        std::function<void(const wxString& message)> failed;
    };

    /// Asks the server who `token` belongs to, on a worker, and stores the
    /// answer. Does nothing when an attempt is already running.
    void connect(std::string token, std::function<void(std::function<void()>)> dispatch,
                 ConnectHandlers handlers);

    /// Whether an attempt is running.
    [[nodiscard]] bool connecting() const { return connecting_.load(); }

private:
    /// wxSecretStore's service name. Stable for the life of the installation:
    /// changing it would orphan every stored token.
    static const wxString& serviceName();

    std::unique_ptr<IHttpClient>        http_;
    std::unique_ptr<ListenBrainzClient> client_;

    std::thread       worker_;
    std::atomic<bool> connecting_{false};
};

}  // namespace xpcog::app
