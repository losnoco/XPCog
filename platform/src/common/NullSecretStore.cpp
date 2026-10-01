#include "xpcog/platform/SecretStore.hpp"

namespace xpcog::platform {
namespace {

/// What a platform with no store, or a build without one, gets.
///
/// Refusing rather than falling back to a file is the point. A Last.fm session
/// key or a remote-control token written to the settings would be readable by
/// anything that can read the settings, which on every one of these platforms is
/// a lower bar than the keychain -- and uicore/src/RemoteToken.hpp is explicit that
/// the server should decline to start rather than accept that trade.
class NullSecretStore final : public SecretStore {
public:
    bool isAvailable(std::string* why) const override {
        if (why != nullptr) {
            *why = "This build has no password store.";
        }
        return false;
    }

    bool save(const std::string&, const std::string&, const std::string&) override {
        return false;
    }

    bool load(const std::string&, std::string*, std::string*) const override {
        return false;
    }

    bool erase(const std::string&) override { return true; }
};

}  // namespace

// One definition, and the platforms that have a real store take it away.
//
// XPCOG_HAS_SECRET_STORE is set by platform/CMakeLists.txt wherever a real
// implementation is compiled in: always on Windows and macOS, and on Linux only
// when libsecret was found. Naming the *capability* rather than the library is
// what lets that be one condition instead of three.
#if !defined(XPCOG_HAS_SECRET_STORE)
std::unique_ptr<SecretStore> SecretStore::create() {
    return std::make_unique<NullSecretStore>();
}
#endif

std::unique_ptr<SecretStore> makeNullSecretStore() {
    return std::make_unique<NullSecretStore>();
}

}  // namespace xpcog::platform
