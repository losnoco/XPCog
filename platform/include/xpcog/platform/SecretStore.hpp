// The platform's own password store.
//
// This was wxSecretStore, reached from app/src/LastFmAccount.cpp and
// app/src/RemoteToken.cpp. It moves here because a second frontend needs the
// same records, and because a Last.fm session key is not a thing to keep two
// copies of: log in on one binary and the other should already be logged in.
//
// **The unit is a service, holding a user and a secret together**, which is
// wxSecretStore's shape and is kept deliberately. LastFmAccount.hpp explains
// why it matters there: the username and the session key are one record because
// a key without the name it belongs to cannot be used, and storing them apart
// invites a pair that has drifted.
//
// The Linux implementation reproduces wx's libsecret schema exactly --
// `org.freedesktop.Secret.Generic` with string attributes `service` and `user`
// -- so a listener who logged in under the wx build is already logged in here.
// That is not a detail: getting it wrong loses somebody's session silently and
// there is no way to tell them why.

#pragma once

#include <memory>
#include <string>

namespace xpcog::platform {

class SecretStore {
public:
    /// Never null. A build or a machine with no store gets one that reports
    /// itself unavailable and refuses every write, which is the behaviour
    /// RemoteToken.hpp already depends on: the server declines to start rather
    /// than keeping a token somewhere it should not be.
    [[nodiscard]] static std::unique_ptr<SecretStore> create();

    SecretStore()          = default;
    virtual ~SecretStore() = default;

    SecretStore(const SecretStore&)            = delete;
    SecretStore& operator=(const SecretStore&) = delete;

    /// Whether anything can be stored. `why` takes a sentence for the interface
    /// to show when it cannot.
    [[nodiscard]] virtual bool isAvailable(std::string* why = nullptr) const = 0;

    /// Replaces whatever `service` held.
    virtual bool save(const std::string& service, const std::string& user,
                      const std::string& secret) = 0;

    /// Reads both halves back. Either pointer may be null.
    [[nodiscard]] virtual bool load(const std::string& service, std::string* user,
                                    std::string* secret) const = 0;

    /// Forgets `service`. Returns true when there is nothing left, including
    /// when there was nothing to begin with.
    virtual bool erase(const std::string& service) = 0;
};

}  // namespace xpcog::platform
