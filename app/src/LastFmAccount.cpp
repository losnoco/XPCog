#include "LastFmAccount.hpp"

#include "LastFmSecrets.hpp"

#include "xpcog/core/net/HttpClient.hpp"
#include "xpcog/core/scrobble/LastFmClient.hpp"

#include <wx/secretstore.h>
#include <wx/utils.h>

#include <chrono>
#include <string>
#include <thread>
#include <utility>

namespace xpcog::app {
namespace {

using namespace std::chrono_literals;

/// How long to keep asking whether the listener has granted access, and how
/// often.
///
/// Last.fm's request tokens are good for sixty minutes, so the ceiling here is
/// not the protocol's -- it is how long a dialog can plausibly sit saying
/// "waiting for your browser" before it is lying about being in progress. Three
/// seconds between polls is comfortably inside any rate limit for a call this
/// cheap, and is fast enough that granting access feels like it completed
/// rather than like it was noticed.
constexpr auto kPollInterval = 3s;
constexpr auto kPollTimeout  = 3min;

/// Cancellation is checked between short sleeps rather than by interrupting the
/// thread, so a listener who presses Cancel does not wait three seconds to see
/// it take effect.
constexpr auto kCancelGranularity = 100ms;

/// Stands in when this build has no HTTP client, so that `client()` can promise
/// to be non-null and no caller has to branch. Every call fails as a transport
/// error, which is the truth: there is no transport.
class NoTransport final : public IHttpClient {
public:
    HttpResponse post(std::string_view, const HttpParams&) override { return refuse(); }
    HttpResponse get(std::string_view, const HttpParams&) override { return refuse(); }

private:
    [[nodiscard]] static HttpResponse refuse() {
        return HttpResponse{0, {}, "this build has no HTTP support"};
    }
};

/// Sleeps up to `total`, returning false if cancellation was signalled.
[[nodiscard]] bool interruptibleSleep(const std::atomic<bool>& cancelled,
                                      std::chrono::milliseconds total) {
    auto remaining = total;
    while (remaining > 0ms) {
        if (cancelled.load()) {
            return false;
        }
        const auto slice = (remaining < kCancelGranularity) ? remaining
                                                            : kCancelGranularity;
        std::this_thread::sleep_for(slice);
        remaining -= slice;
    }
    return !cancelled.load();
}

}  // namespace

const wxString& LastFmAccount::serviceName() {
    // Stable for the life of the installation: changing it would orphan every
    // stored session and silently sign everybody out.
    static const wxString name = "XPCog/last.fm";
    return name;
}

LastFmAccount::LastFmAccount()
    : http_(makeCurlHttpClient()) {
    // Falls back to a transport that refuses everything, so `client()` is always
    // valid and nothing downstream needs an #ifdef or a null check. A build
    // without HTTP then behaves as a build that is permanently offline, which is
    // both true and a state the rest of this already handles.
    if (!http_) {
        http_ = std::make_unique<NoTransport>();
    }
    client_ = std::make_unique<LastFmClient>(*http_,
                                             std::string{secrets::kLastFmApiKey},
                                             std::string{secrets::kLastFmApiSecret});
}

LastFmAccount::~LastFmAccount() {
    cancelConnect();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool LastFmAccount::usable() const {
    return httpClientAvailable() && client_->configured();
}

wxString LastFmAccount::unavailableReason() const {
    if (!httpClientAvailable()) {
        return "This build was configured without HTTP support, so it cannot "
               "reach Last.fm.";
    }
    if (!client_->configured()) {
        return "This build carries no Last.fm API key. See "
               "app/src/LastFmSecrets.hpp.in for how to build with one.";
    }
    return {};
}

bool LastFmAccount::storeAvailable(wxString* why) {
#if wxUSE_SECRETSTORE
    wxSecretStore store = wxSecretStore::GetDefault();
    return store.IsOk(why);
#else
    if (why != nullptr) {
        // Reached when wx itself was built without the feature -- vcpkg's port
        // defaults to exactly that, which is why vcpkg.json asks for it by name.
        *why = "This build of wxWidgets has no secret store, so a Last.fm "
               "session could not be kept safely.";
    }
    return false;
#endif
}

Scrobbler::Session LastFmAccount::load() const {
#if wxUSE_SECRETSTORE
    wxSecretStore store = wxSecretStore::GetDefault();
    if (!store.IsOk()) {
        return {};
    }

    wxString      username;
    wxSecretValue secret;
    if (!store.Load(serviceName(), username, secret)) {
        return {};
    }

    // wxSecretString wipes itself on the way out of scope. The copy into
    // std::string below is the one that survives, and it survives because the
    // scrobbler needs it for every request -- but there is no reason for a
    // second copy to sit in a wxString until the next garbage moment.
    const wxSecretString key{secret};

    Scrobbler::Session session;
    session.key      = key.utf8_string();
    session.username = username.utf8_string();
    return session;
#else
    return {};
#endif
}

bool LastFmAccount::save(const Scrobbler::Session& session) {
#if wxUSE_SECRETSTORE
    wxSecretStore store = wxSecretStore::GetDefault();
    if (!store.IsOk()) {
        return false;
    }
    // One record holding both, which is the shape wxSecretStore already has and
    // the reason it is used rather than a keychain call plus a settings key:
    // Cog keeps the username in NSUserDefaults and the key in the Keychain, and
    // those two can disagree. These cannot.
    return store.Save(serviceName(), wxString::FromUTF8(session.username),
                      wxSecretValue{wxString::FromUTF8(session.key)});
#else
    (void)session;
    return false;
#endif
}

void LastFmAccount::forget() {
#if wxUSE_SECRETSTORE
    wxSecretStore store = wxSecretStore::GetDefault();
    if (store.IsOk()) {
        store.Delete(serviceName());
    }
#endif
}

void LastFmAccount::connect(std::function<void(std::function<void()>)> dispatch,
                            ConnectHandlers                            handlers) {
    if (!usable()) {
        const wxString reason = unavailableReason();
        dispatch([handlers, reason] {
            if (handlers.failed) {
                handlers.failed(reason);
            }
        });
        return;
    }

    if (connecting_.exchange(true)) {
        return;
    }
    cancelled_.store(false);

    // A previous attempt's thread has finished but may not have been joined.
    if (worker_.joinable()) {
        worker_.join();
    }

    worker_ = std::thread([this, dispatch, handlers] {
        const auto fail = [&dispatch, &handlers](const wxString& message) {
            dispatch([handlers, message] {
                if (handlers.failed) {
                    handlers.failed(message);
                }
            });
        };

        // Step 1: a request token.
        LastFmError error;
        const auto  token = client_->requestToken(&error);
        if (!token) {
            connecting_.store(false);
            fail(error.kind == LastFmError::Kind::Transport
                     ? wxString("Could not reach Last.fm. Check your "
                                "connection and try again.")
                     : wxString::FromUTF8(error.message));
            return;
        }

        // Step 2: the listener grants access in a browser. Opening it is a
        // toolkit call and belongs on the interface's thread.
        const std::string url = client_->authorizationUrl(*token);
        dispatch([handlers, url] {
            wxLaunchDefaultBrowser(wxString::FromUTF8(url));
            if (handlers.awaitingAuthorization) {
                handlers.awaitingAuthorization(wxString::FromUTF8(url));
            }
        });

        // Step 3: poll until it is granted, refused, or gives up.
        const auto deadline = std::chrono::steady_clock::now() + kPollTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!interruptibleSleep(cancelled_,
                                    std::chrono::duration_cast<std::chrono::milliseconds>(
                                        kPollInterval))) {
                connecting_.store(false);
                fail("Cancelled.");
                return;
            }

            LastFmError pollError;
            auto        session = client_->session(*token, &pollError);
            if (session) {
                Scrobbler::Session granted;
                granted.key      = session->key;
                granted.username = session->username;

                // Stored on the interface's thread: wx makes no thread-safety
                // promise about wxSecretStore, and this is the one write.
                dispatch([this, handlers, granted] {
                    if (!save(granted)) {
                        if (handlers.failed) {
                            handlers.failed(
                                "Connected to Last.fm, but the session could not "
                                "be saved, so it would be lost on restart.");
                        }
                        return;
                    }
                    if (handlers.connected) {
                        handlers.connected(granted);
                    }
                });
                connecting_.store(false);
                return;
            }

            // Error 14 is "not yet", which is the whole reason this polls.
            if (pollError.kind != LastFmError::Kind::NotAuthorized) {
                connecting_.store(false);
                fail(pollError.kind == LastFmError::Kind::Transport
                         ? wxString("Lost contact with Last.fm.")
                         : wxString::FromUTF8(pollError.message));
                return;
            }
        }

        connecting_.store(false);
        fail("Timed out waiting for authorisation in your browser.");
    });
}

void LastFmAccount::cancelConnect() { cancelled_.store(true); }

}  // namespace xpcog::app
