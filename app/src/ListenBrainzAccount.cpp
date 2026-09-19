#include "ListenBrainzAccount.hpp"

#include "xpcog/core/net/HttpClient.hpp"
#include "xpcog/core/scrobble/ListenBrainzClient.hpp"

#include <wx/secretstore.h>
#include <wx/translation.h>

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

}  // namespace

const wxString& ListenBrainzAccount::serviceName() {
    static const wxString name = "XPCog/ListenBrainz";
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

wxString ListenBrainzAccount::unavailableReason() const {
    if (!httpClientAvailable()) {
        return _("This build was configured without HTTP support, so it cannot "
                 "reach ListenBrainz.");
    }
    return {};
}

Scrobbler::Session ListenBrainzAccount::load() const {
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
    const wxSecretString token{secret};

    Scrobbler::Session session;
    session.key      = token.utf8_string();
    session.username = username.utf8_string();
    return session;
#else
    return {};
#endif
}

bool ListenBrainzAccount::save(const Scrobbler::Session& session) {
#if wxUSE_SECRETSTORE
    wxSecretStore store = wxSecretStore::GetDefault();
    if (!store.IsOk()) {
        return false;
    }
    return store.Save(serviceName(), wxString::FromUTF8(session.username),
                      wxSecretValue{wxString::FromUTF8(session.key)});
#else
    (void)session;
    return false;
#endif
}

void ListenBrainzAccount::forget() {
#if wxUSE_SECRETSTORE
    wxSecretStore store = wxSecretStore::GetDefault();
    if (store.IsOk()) {
        store.Delete(serviceName());
    }
#endif
}

void ListenBrainzAccount::setApiRoot(const std::string& apiRoot) {
    client_->setApiRoot(apiRoot);
}

void ListenBrainzAccount::connect(std::string                                token,
                                  std::function<void(std::function<void()>)> dispatch,
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
    if (token.empty()) {
        dispatch([handlers] {
            if (handlers.failed) {
                handlers.failed(_("Paste your user token first."));
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
            wxString message;
            switch (error.kind) {
            case ScrobbleError::Kind::Transport:
                message = _("Could not reach ListenBrainz. Check your connection "
                            "and the server address, and try again.");
                break;
            case ScrobbleError::Kind::SessionInvalid:
                message = _("ListenBrainz does not know that token. Copy it again "
                            "from your settings page.");
                break;
            default:
                message = wxString::FromUTF8(error.message);
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

        // Stored on the interface's thread: wx makes no thread-safety promise
        // about wxSecretStore, and this is the one write.
        dispatch([this, handlers, granted] {
            if (!save(granted)) {
                if (handlers.failed) {
                    handlers.failed(_("Connected to ListenBrainz, but the token could "
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
