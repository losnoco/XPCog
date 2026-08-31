#include "RemoteToken.hpp"

#include "Text.hpp"

#include "xpcog/core/remote/Token.hpp"

#include <wx/secretstore.h>
#include <wx/translation.h>

namespace xpcog::app {
namespace {

constexpr const char* kUser = "token";

}  // namespace

wxString RemoteToken::serviceName() {
    // The domain, as LastFmAccount's does, so the entry is identifiable in
    // whatever the platform's credential manager is called.
    return "co.losno.XPCog/remote-api";
}

bool RemoteToken::storeAvailable(wxString* why) {
#if wxUSE_SECRETSTORE
    wxSecretStore store = wxSecretStore::GetDefault();
    return store.IsOk(why);
#else
    if (why != nullptr) {
        *why = _("This build of wxWidgets has no secret store, so the remote "
                 "control's access token could not be kept safely.");
    }
    return false;
#endif
}

std::string RemoteToken::load() {
#if wxUSE_SECRETSTORE
    wxSecretStore store = wxSecretStore::GetDefault();
    if (!store.IsOk()) {
        return {};
    }
    wxString      user;
    wxSecretValue value;
    if (!store.Load(serviceName(), user, value)) {
        return {};
    }
    return toUtf8(value.GetAsString());
#else
    return {};
#endif
}

bool RemoteToken::save(const std::string& token) {
#if wxUSE_SECRETSTORE
    wxSecretStore store = wxSecretStore::GetDefault();
    if (!store.IsOk()) {
        return false;
    }
    return store.Save(serviceName(), kUser, wxSecretValue{toWx(token)});
#else
    (void)token;
    return false;
#endif
}

std::string RemoteToken::ensure() {
    if (std::string existing = load(); !existing.empty()) {
        return existing;
    }
    // Empty when the system generator would not answer, and that is passed
    // straight back rather than substituted for: the server refuses to start
    // without a token, which is the right outcome.
    const std::string token = remote::generateRemoteToken();
    if (token.empty() || !save(token)) {
        return {};
    }
    return token;
}

std::string RemoteToken::regenerate() {
    const std::string token = remote::generateRemoteToken();
    if (token.empty() || !save(token)) {
        return {};
    }
    return token;
}

}  // namespace xpcog::app
