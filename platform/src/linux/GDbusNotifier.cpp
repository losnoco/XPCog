#include "xpcog/platform/Notifier.hpp"

#include <gio/gio.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace xpcog::platform {

std::unique_ptr<Notifier> makeNullNotifier();

namespace {

constexpr const char* kService   = "org.freedesktop.Notifications";
constexpr const char* kPath      = "/org/freedesktop/Notifications";
constexpr const char* kInterface = "org.freedesktop.Notifications";

/// The cover, as the spec's `image-data` hint wants it.
///
/// `(iiibiiay)`: width, height, rowstride, has-alpha, bits per sample, channels,
/// and the pixels -- which are **RGBA**, not the ARGB TrayImage carries. Getting
/// that backwards produces a cover with its red and blue swapped, which looks
/// like a deliberate stylisation rather than a bug; SNI's pixmaps have the same
/// trap in the other direction and TrayIcon.hpp says so at length.
[[nodiscard]] GVariant* imageHint(const TrayImage& image) {
    const auto pixels = static_cast<std::size_t>(image.width) *
                        static_cast<std::size_t>(image.height);
    if (pixels == 0 || image.argb.size() < pixels * 4) {
        return nullptr;
    }

    std::vector<std::uint8_t> rgba;
    rgba.reserve(pixels * 4);
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(image.argb.data()) + (i * 4);
        rgba.push_back(p[1]);  // r
        rgba.push_back(p[2]);  // g
        rgba.push_back(p[3]);  // b
        rgba.push_back(p[0]);  // a
    }

    GVariant* bytes = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, rgba.data(),
                                                rgba.size(), sizeof(std::uint8_t));
    return g_variant_new("(iiibii@ay)", image.width, image.height, image.width * 4, TRUE,
                         8, 4, bytes);
}

class GDbusNotifier final : public Notifier {
public:
    explicit GDbusNotifier(Dispatcher dispatch) : dispatch_(std::move(dispatch)) {
        GError* error = nullptr;
        connection_   = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
        g_clear_error(&error);
    }

    ~GDbusNotifier() override {
        if (connection_ != nullptr) {
            g_object_unref(connection_);
        }
    }

    bool isAvailable() const override { return connection_ != nullptr; }

    void show(const Notification& notification) override {
        if (connection_ == nullptr) {
            return;
        }

        GVariantBuilder hints;
        g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
        if (notification.imagePath) {
            // Preferred over image-data when both are offered: the daemon reads
            // and scales the file itself, which is less to get wrong than a
            // hand-packed pixel array.
            g_variant_builder_add(
                &hints, "{sv}", "image-path",
                g_variant_new_string(notification.imagePath->c_str()));
        } else if (notification.image) {
            if (GVariant* data = imageHint(*notification.image); data != nullptr) {
                g_variant_builder_add(&hints, "{sv}", "image-data", data);
            }
        }
        // So a run of track changes replaces one notification rather than
        // stacking six. The spec's own mechanism for exactly this.
        g_variant_builder_add(&hints, "{sv}", "transient", g_variant_new_boolean(FALSE));

        const char* const actions[] = {nullptr};

        // Replaces the last one this process raised, which is what makes a
        // player's announcements a running commentary rather than a pile.
        // `^as` and not `as`: plain `as` in a g_variant_new format wants a
        // GVariantBuilder, and handing it a C array aborts the process with
        // "expected array GVariantBuilder but the built value has type
        // '(null)'". The caret is what takes a NULL-terminated char**.
        GVariant* args = g_variant_new(
            "(susss^asa{sv}i)", "XPCog", replaces_, "", notification.title.c_str(),
            notification.body.c_str(), actions, &hints, -1);

        // Fire and forget: there is nothing to wait for, and the only failure is
        // a daemon that is not running.
        g_dbus_connection_call(connection_, kService, kPath, kInterface, "Notify", args,
                               G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000,
                               nullptr, onReplied, this);
    }

private:
    static void onReplied(GObject* source, GAsyncResult* result, gpointer user) {
        GError*   error = nullptr;
        GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result,
                                                        &error);
        if (reply != nullptr) {
            auto* self = static_cast<GDbusNotifier*>(user);
            g_variant_get(reply, "(u)", &self->replaces_);
            g_variant_unref(reply);
        }
        g_clear_error(&error);
    }

    Dispatcher       dispatch_;
    GDBusConnection* connection_ = nullptr;
    /// The id the daemon gave the last notification, so the next one replaces
    /// it. Zero means "a new one", which is what the first call sends.
    guint32 replaces_ = 0;
};

}  // namespace

std::unique_ptr<Notifier> Notifier::create(Dispatcher dispatch) {
    auto notifier = std::make_unique<GDbusNotifier>(std::move(dispatch));
    if (!notifier->isAvailable()) {
        return makeNullNotifier();
    }
    return notifier;
}

}  // namespace xpcog::platform
