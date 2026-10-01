#include "RemoteToken.hpp"

#include "Translations.hpp"

#include "xpcog/platform/SecretStore.hpp"

#include "xpcog/core/remote/Token.hpp"

namespace xpcog::app {

std::string RemoteToken::serviceName() {
    // The domain, as LastFmAccount's does, so the entry is identifiable in
    // whatever the platform's credential manager is called.
    return "co.losno.XPCog/remote-api";
}

namespace {

/// One store for the process. Opening a keyring is a D-Bus round trip on Linux
/// and a keychain unlock elsewhere, and this is asked several times per
/// preferences pane.
platform::SecretStore& store() {
    static const std::unique_ptr<platform::SecretStore> instance =
        platform::SecretStore::create();
    return *instance;
}

/// The user half of the record. The token has no user, but the store's unit is
/// a service holding a user and a secret together, so it gets a fixed one.
constexpr const char* kUser = "xpcog";

}  // namespace

bool RemoteToken::storeAvailable(std::string* why) {
    return store().isAvailable(why);
}

std::string RemoteToken::load() {
    std::string secret;
    if (!store().load(serviceName(), nullptr, &secret)) {
        return {};
    }
    return secret;
}

bool RemoteToken::save(const std::string& token) {
    return store().save(serviceName(), kUser, token);
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
