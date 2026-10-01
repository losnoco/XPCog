#include "xpcog/platform/SecretStore.hpp"

#include <libsecret/secret.h>

#include <memory>
#include <string>

namespace xpcog::platform {

std::unique_ptr<SecretStore> makeNullSecretStore();

namespace {

/// wxSecretStore's schema, reproduced exactly.
///
/// `org.freedesktop.Secret.Generic` with two string attributes, `service` and
/// `user`, is what wx/src/unix/secretstore.cpp writes -- so this reads and
/// writes the records the wx build already made. Anything else here would
/// silently lose a listener's Last.fm session the first time they opened the
/// other binary, with no error and nothing to look at.
const SecretSchema* schema() {
    // The reserved fields are suppressed rather than initialised, which is what
    // wx does at the same struct: they are not ours to set, and naming them
    // would break against a libsecret that adds one.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
    // Only the first three members are named. SecretSchema ends in eight
    // `reserved` fields that are not ours to set, and wx suppresses the
    // missing-initializer warning for the same reason -- here the aggregate is
    // just left short, which value-initialises them.
    static const SecretSchema s_schema = {
        "org.freedesktop.Secret.Generic",
        SECRET_SCHEMA_NONE,
        {
            {"service", SECRET_SCHEMA_ATTRIBUTE_STRING},
            {"user", SECRET_SCHEMA_ATTRIBUTE_STRING},
            {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING},
        },
    };
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    return &s_schema;
}

class LibSecretStore final : public SecretStore {
public:
    bool isAvailable(std::string* why) const override {
        GError*        error   = nullptr;
        SecretService* service = secret_service_get_sync(SECRET_SERVICE_NONE, nullptr, &error);
        if (service == nullptr) {
            if (why != nullptr) {
                *why = error != nullptr && error->message != nullptr
                           ? error->message
                           : "No keyring is available.";
            }
            g_clear_error(&error);
            return false;
        }
        g_object_unref(service);
        return true;
    }

    bool save(const std::string& service, const std::string& user,
              const std::string& secret) override {
        // Erased first, because a service whose user has changed would
        // otherwise leave the old record behind: the attributes include the
        // user, so a store under a new one does not replace the old one.
        erase(service);

        GError*    error = nullptr;
        const auto label = "XPCog: " + service;
        const gboolean ok =
            secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT, label.c_str(),
                                       secret.c_str(), nullptr, &error, "service",
                                       service.c_str(), "user", user.c_str(), nullptr);
        g_clear_error(&error);
        return ok != FALSE;
    }

    bool load(const std::string& service, std::string* user,
              std::string* secret) const override {
        // Searched rather than looked up, because the username is an
        // *attribute* and secret_password_lookup_sync answers with the password
        // alone. This is what wx does for the same reason.
        GError*         error = nullptr;
        GHashTable*     attributes =
            secret_attributes_build(schema(), "service", service.c_str(), nullptr);
        GList* found = secret_service_search_sync(
            nullptr, schema(), attributes,
            static_cast<SecretSearchFlags>(SECRET_SEARCH_UNLOCK | SECRET_SEARCH_LOAD_SECRETS),
            nullptr, &error);
        g_hash_table_unref(attributes);

        if (error != nullptr || found == nullptr) {
            g_clear_error(&error);
            return false;
        }

        auto* item = static_cast<SecretItem*>(found->data);
        bool  ok   = false;

        if (SecretValue* value = secret_item_get_secret(item); value != nullptr) {
            if (secret != nullptr) {
                gsize       length = 0;
                const gchar* bytes = secret_value_get(value, &length);
                secret->assign(bytes, length);
            }
            ok = true;
        }
        if (user != nullptr) {
            GHashTable* stored = secret_item_get_attributes(item);
            const auto* name =
                static_cast<const char*>(g_hash_table_lookup(stored, "user"));
            user->assign(name != nullptr ? name : "");
            g_hash_table_unref(stored);
        }

        g_list_free_full(found, g_object_unref);
        return ok;
    }

    bool erase(const std::string& service) override {
        GError* error = nullptr;
        // clear_sync answers false when nothing matched, which is not a failure
        // to erase -- the record is gone either way.
        secret_password_clear_sync(schema(), nullptr, &error, "service", service.c_str(),
                                   nullptr);
        const bool failed = error != nullptr;
        g_clear_error(&error);
        return !failed;
    }
};

}  // namespace

std::unique_ptr<SecretStore> SecretStore::create() {
    auto store = std::make_unique<LibSecretStore>();
    if (!store->isAvailable(nullptr)) {
        // A machine with no keyring running -- a bare session, a container --
        // gets the refusing one rather than an object that fails at every call.
        return makeNullSecretStore();
    }
    return store;
}

}  // namespace xpcog::platform
