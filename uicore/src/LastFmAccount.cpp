#include "LastFmAccount.hpp"

#include "Translations.hpp"

#include "xpcog/platform/OpenUrl.hpp"
#include "xpcog/platform/SecretStore.hpp"

#include "LastFmSecrets.hpp"

#include "xpcog/core/net/HttpClient.hpp"
#include "xpcog/core/scrobble/LastFmClient.hpp"


#include <chrono>
#include <string>
#include <thread>
#include <memory>
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
    HttpResponse postJson(std::string_view, std::string_view, const HttpHeaders&) override {
        return refuse();
    }
    HttpResponse get(std::string_view, const HttpParams&, const HttpHeaders&) override {
        return refuse();
    }

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

namespace {

/// One store for the process. Opening a keyring is a round trip; this is asked
/// once per preferences pane and once at startup.
platform::SecretStore& store() {
    static const std::unique_ptr<platform::SecretStore> instance =
        platform::SecretStore::create();
    return *instance;
}

}  // namespace

const std::string& LastFmAccount::serviceName() {
    // Stable for the life of the installation: changing it would orphan every
    // stored session and silently sign everybody out.
    static const std::string name = "XPCog/last.fm";
    return name;
}

const std::string& LastFmAccount::apiServiceName() {
    // Same rule: stable, and named so the listener can find it beside the
    // session in whatever the platform calls its credential manager.
    static const std::string name = "XPCog/last.fm API key";
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
    // Read here rather than lazily: the scrobbler is built right after this
    // and starts using the client, so the pair has to be in place before that.
    // The session is read at the same moment by MainFrame, so this is not a
    // new trip to the store, only a second record from the same one.
    own_ = loadApiCredentials();
    applyCredentials();
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

std::string LastFmAccount::unavailableReason() const {
    if (!httpClientAvailable()) {
        return tr("This build was configured without HTTP support, so it cannot "
                 "reach Last.fm.");
    }
    if (!client_->configured()) {
        return tr("This build carries no Last.fm API key. Enter your own below "
                  "to scrobble.");
    }
    return {};
}

bool LastFmAccount::storeAvailable(std::string* why) {
    return store().isAvailable(why);
}

Scrobbler::Session LastFmAccount::load() const {
    Scrobbler::Session session;
    if (!store().load(serviceName(), &session.username, &session.key)) {
        return {};
    }
    return session;
}

bool LastFmAccount::save(const Scrobbler::Session& session) {
    // One record holding both, which is the shape the store already has and the
    // reason it is used rather than a keychain call plus a settings key: Cog
    // keeps the username in NSUserDefaults and the key in the Keychain, and
    // those two can disagree. These cannot.
    return store().save(serviceName(), session.username, session.key);
}

void LastFmAccount::forget() { static_cast<void>(store().erase(serviceName())); }

bool LastFmAccount::hasBuiltInCredentials() {
    return !secrets::kLastFmApiKey.empty() && !secrets::kLastFmApiSecret.empty();
}

LastFmAccount::ApiCredentials LastFmAccount::loadApiCredentials() {
    ApiCredentials credentials;
    if (!store().load(apiServiceName(), &credentials.key, &credentials.secret)) {
        return {};
    }
    // A record with a half missing is treated as absent rather than handed to
    // the client to fail every request with: it cannot have been written by
    // setApiCredentials(), which refuses such a pair.
    return credentials.complete() ? credentials : ApiCredentials{};
}

LastFmAccount::ApplyResult LastFmAccount::setApiCredentials(ApiCredentials credentials) {
    if (!credentials.empty() && !credentials.complete()) {
        return ApplyResult::Incomplete;
    }

    if (!store().isAvailable()) {
        return ApplyResult::StoreRefused;
    }
    if (credentials.empty()) {
        // erase() answers true for a record that was not there, which is the
        // outcome asked for; only a store that could not remove one refuses.
        if (!store().erase(apiServiceName())) {
            return ApplyResult::StoreRefused;
        }
    } else if (!store().save(apiServiceName(), credentials.key, credentials.secret)) {
        return ApplyResult::StoreRefused;
    }

    const std::string before = client_->apiKey();
    own_                     = std::move(credentials);
    applyCredentials();

    if (client_->apiKey() == before) {
        return ApplyResult::Applied;
    }
    forget();
    return ApplyResult::AppliedAndDisconnected;
}

void LastFmAccount::applyCredentials() {
    if (own_.complete()) {
        client_->setCredentials(own_.key, own_.secret);
    } else {
        client_->setCredentials(std::string{secrets::kLastFmApiKey},
                                std::string{secrets::kLastFmApiSecret});
    }
}

void LastFmAccount::connect(std::function<void(std::function<void()>)> dispatch,
                            ConnectHandlers                            handlers) {
    if (!usable()) {
        const std::string reason = unavailableReason();
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
        const auto fail = [&dispatch, &handlers](const std::string& message) {
            dispatch([handlers, message] {
                if (handlers.failed) {
                    handlers.failed(message);
                }
            });
        };

        // Step 1: a request token.
        ScrobbleError error;
        const auto  token = client_->requestToken(&error);
        if (!token) {
            connecting_.store(false);
            fail(error.kind == ScrobbleError::Kind::Transport
                     ? std::string(tr("Could not reach Last.fm. Check your "
                                  "connection and try again."))
                     : std::string(error.message));
            return;
        }

        // Step 2: the listener grants access in a browser. Opening it is a
        // toolkit call and belongs on the interface's thread.
        const std::string url = client_->authorizationUrl(*token);
        dispatch([handlers, url] {
            platform::openInBrowser(url);
            if (handlers.awaitingAuthorization) {
                handlers.awaitingAuthorization(std::string(url));
            }
        });

        // Step 3: poll until it is granted, refused, or gives up.
        const auto deadline = std::chrono::steady_clock::now() + kPollTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!interruptibleSleep(cancelled_,
                                    std::chrono::duration_cast<std::chrono::milliseconds>(
                                        kPollInterval))) {
                connecting_.store(false);
                fail(tr("Cancelled."));
                return;
            }

            ScrobbleError pollError;
            auto        session = client_->session(*token, &pollError);
            if (session) {
                Scrobbler::Session granted;
                granted.key      = session->key;
                granted.username = session->username;

                // Stored on the interface's thread. The store makes no
                // thread-safety promise -- neither libsecret's nor the
                // keychain's -- and this is the one write.
                dispatch([this, handlers, granted] {
                    if (!save(granted)) {
                        if (handlers.failed) {
                            handlers.failed(
                                tr("Connected to Last.fm, but the session could not "
                                  "be saved, so it would be lost on restart."));
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
            if (pollError.kind != ScrobbleError::Kind::NotAuthorized) {
                connecting_.store(false);
                fail(pollError.kind == ScrobbleError::Kind::Transport
                         ? std::string(tr("Lost contact with Last.fm."))
                         : std::string(pollError.message));
                return;
            }
        }

        connecting_.store(false);
        fail(tr("Timed out waiting for authorisation in your browser."));
    });
}

void LastFmAccount::cancelConnect() { cancelled_.store(true); }

}  // namespace xpcog::app
