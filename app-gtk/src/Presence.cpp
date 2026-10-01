#include "Presence.hpp"

#include "xpcog/core/FilePath.hpp"
#include "xpcog/platform/SettingsStore.hpp"

#include <gtk/gtk.h>

#include <cstdint>
#include <filesystem>

namespace xpcog::gtk {

namespace {

/// The application icon at every size the resource holds, as the wire format
/// the tray wants: one pixel at a time, alpha first, straight alpha.
///
/// GdkTexture downloads as premultiplied BGRA in native order, so each pixel is
/// unpremultiplied and re-ordered by hand -- a byte at a time rather than
/// through a uint32_t, which is what keeps this right on a little-endian
/// machine; TrayIcon.hpp says why that particular mistake survives being
/// looked at.
std::vector<platform::TrayImage> trayImages() {
    constexpr int kSizes[] = {16, 24, 32, 48, 64};

    std::vector<platform::TrayImage> images;
    for (const int size : kSizes) {
        const std::string path = "/co/losno/XPCog/icons/hicolor/" + std::to_string(size) + "x" +
                                 std::to_string(size) + "/apps/co.losno.XPCog.png";
        GErrorPtr  error;
        GdkTexture* texture = gdk_texture_new_from_resource(path.c_str());
        if (texture == nullptr) {
            continue;
        }
        auto owned = GObjectPtr<GdkTexture>::adopt(texture);

        platform::TrayImage out;
        out.width  = gdk_texture_get_width(texture);
        out.height = gdk_texture_get_height(texture);
        const auto pixels = static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height);
        std::vector<std::uint8_t> premultiplied(pixels * 4);
        gdk_texture_download(texture, premultiplied.data(), static_cast<gsize>(out.width) * 4);

        out.argb.resize(pixels * 4);
        for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
            const std::uint8_t* in = premultiplied.data() + pixel * 4;
            // CAIRO_FORMAT_ARGB32 in native order on a little-endian machine:
            // B, G, R, A in memory.
            const std::uint8_t a = in[3];
            const auto unpremultiply = [a](std::uint8_t channel) -> std::uint8_t {
                return a == 0 ? 0 : static_cast<std::uint8_t>((channel * 255 + a / 2) / a);
            };
            std::byte* target = out.argb.data() + pixel * 4;
            target[0]         = static_cast<std::byte>(a);
            target[1]         = static_cast<std::byte>(unpremultiply(in[2]));
            target[2]         = static_cast<std::byte>(unpremultiply(in[1]));
            target[3]         = static_cast<std::byte>(unpremultiply(in[0]));
        }
        images.push_back(std::move(out));
    }
    return images;
}

}  // namespace

Presence::Presence(Dispatcher dispatch) {
    tray_ = platform::TrayIcon::create(dispatch);
    if (tray_->isAvailable()) {
        subscriptions_.push_back(tray_->activated.connect([this] { showRequested.publish(); }));
        subscriptions_.push_back(tray_->menuItemActivated.connect([this](int id) {
            if (id == app::kTrayShowWindowId) {
                showRequested.publish();
            } else {
                commandActivated.publish(id);
            }
        }));
        tray_->setIcon(trayImages());
        hasTray_ = true;
    }
    notifier_ = platform::Notifier::create(std::move(dispatch));
    refresh();
}

Presence::~Presence() = default;

void Presence::setNowPlaying(const std::string& title, const std::string& artist) {
    state_.title  = title;
    state_.artist = artist;
    refresh();
}

void Presence::setPlaybackState(bool playing, bool paused) {
    state_.playing = playing;
    state_.paused  = paused;
    refresh();
}

void Presence::clear() {
    state_ = app::TrayState{};
    refresh();
}

void Presence::refresh() {
    if (!hasTray_) {
        return;
    }
    // The name is the bold line and the rest is the body, which is the split
    // StatusNotifierItem's tooltip already has. The menu is pushed on every
    // change rather than built when the panel asks: the panel does not call
    // back to build a menu, it reads one we published, and a menu published
    // once shows the first track for the whole session.
    tray_->setToolTip("XPCog", app::trayTooltipBody(state_));
    tray_->setMenu(app::trayMenuModel(state_, /*withWindowItems=*/true));
}

void Presence::notify(const std::string& title, const std::string& body,
                      const std::shared_ptr<const std::vector<std::byte>>& cover) {
    if (!notifier_ || !notifier_->isAvailable()) {
        return;
    }
    platform::Notification notification;
    notification.title = title;
    notification.body  = body;
    if (cover && !cover->empty()) {
        // The bytes the file carried, written out for the daemon to read: Cog
        // does the same because UNNotificationAttachment takes a URL.
        const std::filesystem::path file =
            pathFromUtf8(platform::cacheDirectory()) / "notification-cover";
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
        if (g_file_set_contents(file.string().c_str(), reinterpret_cast<const char*>(cover->data()),
                                static_cast<gssize>(cover->size()), nullptr)) {
            notification.imagePath = file.string();
        }
    }
    notifier_->show(notification);
}

void Presence::remove() {
    if (tray_) {
        tray_->remove();
    }
    hasTray_ = false;
}

}  // namespace xpcog::gtk
