// The listener's Last.fm connection: where the session key is kept, which API
// key it was opened under, and the three-step dance that obtains one.
//
// This is the app-layer half of scrobbling. Core does the protocol and the
// queue and knows nothing about credentials -- it is handed a session key as a
// string. Everything about *storing* that string, and about the browser trip
// that produces it, is here, because both need a toolkit and core links none.
//
// **Two credentials, not one.** The session key is the listener's; the API key
// and shared secret are the application's, and identify *which program* is
// submitting. A build normally carries that pair baked in (LastFmSecrets.hpp.in
// says how, and why a clean checkout has none). But a listener who built from
// source, or whose build shipped without one, can apply for a pair of their own
// at https://www.last.fm/api/account/create and enter it in the pane. A pair
// entered that way wins over the built-in one, is kept in the same secret store
// as the session, and can be removed to go back. The shared secret is what
// signs every request, so it gets the same treatment as the session key rather
// than a settings key: anyone holding it can submit as that application.
//
// A session belongs to the key that opened it. Change the key and the session
// stops working, so `setApiCredentials()` discards it and says so, and the
// listener connects again under the new one.
//
// **Storage is wxSecretStore**, which is the platform's own facility on each of
// the three: Credential Manager on Windows, the Keychain on macOS, the Secret
// Service (libsecret) on Linux. Cog uses the Keychain directly through a
// `KeychainHelper` of its own; wx already wraps all three, so the alternative
// here was writing that helper three times.
//
// Two things fall out of using it that are better than the obvious arrangement:
//
//   * **The username and the key are stored together.** wxSecretStore's unit is
//     a (service, username, secret) triple, which is exactly the shape of what
//     Last.fm returns. Cog splits them -- the key in the Keychain, the username
//     in NSUserDefaults -- and the two can disagree if one write succeeds and
//     the other does not. Here there is one record and one write.
//   * **The key is never in the settings.** Settings are a registry key or a
//     plist: readable text, backed up, synced, and not where a credential goes.
//     `settings.def` therefore has the on/off switch and nothing else.
//
// The store can be absent -- a wx built without `wxUSE_SECRETSTORE`, or a Linux
// session with no Secret Service running. `available()` answers that, and the
// preferences pane greys itself rather than pretending.
//
// **The authentication flow is the desktop one**, not Cog's. See
// LastFmClient.hpp for why that matters; what it means *here* is that this class
// drives a browser trip and then polls, rather than posting a password from a
// text field. There is no field for a password anywhere in this program, which
// is the point.

#pragma once

#include "xpcog/core/scrobble/Scrobbler.hpp"

#include <wx/string.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace xpcog {
class IHttpClient;
class LastFmClient;
}  // namespace xpcog

namespace xpcog::app {

class LastFmAccount {
public:
    LastFmAccount();
    ~LastFmAccount();

    LastFmAccount(const LastFmAccount&)            = delete;
    LastFmAccount& operator=(const LastFmAccount&) = delete;

    /// Whether an API key is in use *and* there is an HTTP client. False means
    /// scrobbling cannot work here until the listener supplies a key -- or at
    /// all, in a build without HTTP.
    [[nodiscard]] bool usable() const;

    /// Why `usable()` is false, phrased for the preferences pane. Empty when it
    /// is true.
    [[nodiscard]] wxString unavailableReason() const;

    /// Whether the platform's secret store can be reached. Separate from
    /// `usable()` because the failure is the listener's environment rather than
    /// the build, and says something different.
    [[nodiscard]] static bool storeAvailable(wxString* why = nullptr);

    /// The stored session, or an empty one.
    [[nodiscard]] Scrobbler::Session load() const;

    /// Replaces the stored session. Returns false when the store refused, which
    /// the caller must not treat as success: a session that was not written is
    /// one the listener will have to grant again next launch, and telling them
    /// they are connected would be a lie with a delay on it.
    bool save(const Scrobbler::Session& session);

    /// Removes the stored session.
    void forget();

    // --- the application's own credentials -------------------------------

    /// An API key and the shared secret that signs for it. Both or neither:
    /// one without the other cannot make a request.
    struct ApiCredentials {
        std::string key;
        std::string secret;

        [[nodiscard]] bool empty() const noexcept { return key.empty() && secret.empty(); }
        [[nodiscard]] bool complete() const noexcept {
            return !key.empty() && !secret.empty();
        }
    };

    /// Whether this build was configured with a key of its own.
    [[nodiscard]] static bool hasBuiltInCredentials();

    /// The listener's own pair, from the secret store, or an empty one.
    [[nodiscard]] static ApiCredentials loadApiCredentials();

    /// Whether the pair in use is the listener's rather than the build's.
    [[nodiscard]] bool usingOwnCredentials() const { return !own_.empty(); }

    /// Stores `credentials` as the listener's own and puts them into use, or,
    /// given an empty pair, removes the stored one and goes back to the
    /// built-in key. A pair with only one half filled in is refused.
    ///
    /// Returns the outcome rather than a bool because the caller has two things
    /// to say: whether the store took it, and whether the session was discarded
    /// with it. It is when the key in use changed -- a session opened under one
    /// key is not valid for another, so keeping it would mean every scrobble
    /// failing with error 9 until the scrobbler noticed and threw it away
    /// anyway. The caller clears the scrobbler's copy and the listener
    /// connects again.
    enum class ApplyResult : std::uint8_t {
        Applied,
        AppliedAndDisconnected,
        Incomplete,
        StoreRefused,
    };
    ApplyResult setApiCredentials(ApiCredentials credentials);

    /// The client, for the pane. Borrowed; never null.
    [[nodiscard]] LastFmClient& client() const { return *client_; }

    // --- the connect flow ------------------------------------------------

    /// What a connection attempt reports back. **Every one of these is called on
    /// the interface's thread**, marshalled through the dispatcher given to
    /// `connect()`, so a handler may touch widgets directly.
    struct ConnectHandlers {
        /// The browser has been opened at `url` and the listener has to grant
        /// access there. `url` is passed so the pane can offer it again as a
        /// link -- opening a browser can silently fail, and a listener staring
        /// at "waiting for authorisation" with no browser open has no way
        /// forward otherwise.
        std::function<void(const wxString& url)> awaitingAuthorization;

        /// Granted, exchanged, and stored.
        std::function<void(const Scrobbler::Session&)> connected;

        /// Gave up: refused, timed out, or cancelled. `message` is for display.
        std::function<void(const wxString& message)> failed;
    };

    /// Starts the flow. `dispatch` marshals onto the interface's thread.
    ///
    /// Does nothing when an attempt is already running -- the pane disables its
    /// button, and this is the second line of defence rather than the first.
    void connect(std::function<void(std::function<void()>)> dispatch,
                 ConnectHandlers                            handlers);

    /// Abandons an attempt in progress. Safe to call when none is.
    ///
    /// The worker notices at its next poll rather than being killed, so this
    /// returns immediately and the `failed` handler may still fire once.
    void cancelConnect();

    /// Whether an attempt is running.
    [[nodiscard]] bool connecting() const { return connecting_.load(); }

private:
    /// wxSecretStore's service name. Includes the domain so it is identifiable
    /// in Credential Manager and Keychain Access, where the listener may well go
    /// looking for it.
    static const wxString& serviceName();

    /// The record holding the listener's own API key and secret. A second
    /// service rather than a second field: wxSecretStore's unit is one
    /// (service, username, secret), and the key goes in the username slot, so
    /// the two halves are one record and cannot disagree.
    static const wxString& apiServiceName();

    /// Hands the client whichever pair applies: the listener's own when there
    /// is one, the build's otherwise.
    void applyCredentials();

    std::unique_ptr<IHttpClient>  http_;
    std::unique_ptr<LastFmClient> client_;
    ApiCredentials                own_;

    std::thread       worker_;
    std::atomic<bool> connecting_{false};
    std::atomic<bool> cancelled_{false};
};

}  // namespace xpcog::app
