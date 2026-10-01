#include "xpcog/platform/SecretStore.hpp"

#include "WinString.hpp"

#include <windows.h>
// wincred.h after windows.h, which it depends on and does not include.
#include <wincred.h>

#include <memory>
#include <string>
#include <vector>

namespace xpcog::platform {

std::unique_ptr<SecretStore> makeNullSecretStore();

namespace {

/// The target name a record is filed under.
///
/// The service verbatim, which is what wxSecretStore's Windows implementation
/// uses too -- so a listener who logged in under the wx build is already logged
/// in here, the same property the libsecret schema gives on Linux.
[[nodiscard]] std::wstring targetFor(const std::string& service) {
    return toWide(service);
}

class WindowsSecretStore final : public SecretStore {
public:
    bool isAvailable(std::string* why) const override {
        // Credential Manager is part of the OS and cannot be absent. Saying so
        // rather than probing avoids a round trip on a question with one answer.
        static_cast<void>(why);
        return true;
    }

    bool save(const std::string& service, const std::string& user,
              const std::string& secret) override {
        std::wstring target   = targetFor(service);
        std::wstring userName = toWide(user);

        CREDENTIALW credential{};
        credential.Type        = CRED_TYPE_GENERIC;
        credential.TargetName  = target.data();
        credential.UserName    = userName.empty() ? nullptr : userName.data();
        credential.Persist     = CRED_PERSIST_LOCAL_MACHINE;
        // Bytes, not a wide string: the secret is UTF-8 and CredWriteW stores an
        // opaque blob. Encoding it as UTF-16 would round-trip through this
        // process and be unreadable to anything that did not.
        credential.CredentialBlobSize =
            static_cast<DWORD>(secret.size());
        credential.CredentialBlob = reinterpret_cast<LPBYTE>(
            const_cast<char*>(secret.data()));

        return CredWriteW(&credential, 0) != FALSE;
    }

    bool load(const std::string& service, std::string* user,
              std::string* secret) const override {
        PCREDENTIALW credential = nullptr;
        if (CredReadW(targetFor(service).c_str(), CRED_TYPE_GENERIC, 0, &credential) ==
            FALSE) {
            return false;
        }

        if (user != nullptr) {
            user->assign(credential->UserName != nullptr ? toUtf8(credential->UserName)
                                                         : std::string{});
        }
        if (secret != nullptr) {
            secret->assign(reinterpret_cast<const char*>(credential->CredentialBlob),
                           credential->CredentialBlobSize);
        }

        CredFree(credential);
        return true;
    }

    bool erase(const std::string& service) override {
        if (CredDeleteW(targetFor(service).c_str(), CRED_TYPE_GENERIC, 0) != FALSE) {
            return true;
        }
        // Nothing to delete is not a failure to delete: the record is gone
        // either way, which is what the caller asked for.
        return GetLastError() == ERROR_NOT_FOUND;
    }
};

}  // namespace

std::unique_ptr<SecretStore> SecretStore::create() {
    return std::make_unique<WindowsSecretStore>();
}

}  // namespace xpcog::platform
