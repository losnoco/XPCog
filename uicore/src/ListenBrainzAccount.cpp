#include "ListenBrainzAccount.hpp"

#include "Translations.hpp"

#include "xpcog/platform/SecretStore.hpp"

#include "xpcog/core/net/HttpClient.hpp"
#include "xpcog/core/scrobble/ListenBrainzClient.hpp"

#include <memory>
#include <utility>

namespace xpcog::app {
namespace {

/// Stands in when this build has no HTTP client, so that `client()` can promise
/// to be non-null. Every call fails as a transport error, which is the truth.
/// The same stub LastFmAccount keeps, and kept separately rather than shared
/// because it is six lines and the two files have nothing else in common.
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

/// One store for the process, as LastFmAccount keeps one: opening a keyring is
/// a round trip, and this is asked at startup and per preferences pane.
platform::SecretStore& store() {
    static const std::unique_ptr<platform::SecretStore> instance =
        platform::SecretStore::create();
    return *instance;
}

}  // namespace

const std::string& ListenBrainzAccount::serviceName() {
    static const std::string name = "XPCog/ListenBrainz";
    return name;
}

ListenBrainzAccount::ListenBrainzAccount(std::string apiRoot)
    : http_(makeCurlHttpClient()) {
    if (!http_) {
        http_ = std::make_unique<NoTransport>();
    }
    client_ = std::make_unique<ListenBrainzClient>(*http_, std::move(apiRoot));
}

ListenBrainzAccount::~ListenBrainzAccount() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool ListenBrainzAccount::usable() const { return httpClientAvailable(); }

std::string ListenBrainzAccount::unavailableReason() const {
    if (!httpClientAvailable()) {
        return tr("This build was configured without HTTP support, so it cannot "
                  "reach ListenBrainz.");
    }
    return {};
}

Scrobbler::Session ListenBrainzAccount::load() const {
    Scrobbler::Session session;
    if (!store().load(serviceName(), &session.username, &session.key)) {
        return {};
    }
    return session;
}

bool ListenBrainzAccount::save(const Scrobbler::Session& session) {
    return store().save(serviceName(), session.username, session.key);
}

void ListenBrainzAccount::forget() { static_cast<void>(store().erase(serviceName())); }

void ListenBrainzAccount::setApiRoot(const std::string& apiRoot) {
    client_->setApiRoot(apiRoot);
}

void ListenBrainzAccount::connect(std::string                                token,
                                  std::function<void(std::function<void()>)> dispatch,
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
    if (token.empty()) {
        dispatch([handlers] {
            if (handlers.failed) {
                handlers.failed(tr("Paste your user token first."));
            }
        });
        return;
    }

    if (connecting_.exchange(true)) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }

    // One request, so a thread is arguably more than it needs -- but that one
    // request has a twenty-second timeout, and the pane is not going to
    // freeze for it.
    worker_ = std::thread([this, token = std::move(token), dispatch, handlers] {
        ScrobbleError error;
        const auto    username = client_->validateToken(token, &error);
        connecting_.store(false);

        if (!username) {
            std::string message;
            switch (error.kind) {
            case ScrobbleError::Kind::Transport:
                message = tr("Could not reach ListenBrainz. Check your connection "
                             "and the server address, and try again.");
                break;
            case ScrobbleError::Kind::SessionInvalid:
                message = tr("ListenBrainz does not know that token. Copy it again "
                             "from your settings page.");
                break;
            default:
                message = error.message;
                break;
            }
            dispatch([handlers, message] {
                if (handlers.failed) {
                    handlers.failed(message);
                }
            });
            return;
        }

        Scrobbler::Session granted;
        granted.key      = token;
        granted.username = *username;

        // Stored on the interface's thread: the store makes no thread-safety
        // promise, and this is the one write.
        dispatch([this, handlers, granted] {
            if (!save(granted)) {
                if (handlers.failed) {
                    handlers.failed(tr("Connected to ListenBrainz, but the token could "
                                       "not be saved, so it would be lost on restart."));
                }
                return;
            }
            if (handlers.connected) {
                handlers.connected(granted);
            }
        });
    });
}

}  // namespace xpcog::app
