// The player's presence off the window: the tray icon and the desktop's
// notifications. The counterpart of app/src/StatusPresence.hpp, over the same
// two platform seams -- platform::TrayIcon, which is a StatusNotifierItem, and
// platform::Notifier, which is org.freedesktop.Notifications -- with no wx
// fallback behind either, because GTK 4 has no XEmbed tray and no
// notification of its own worth preferring.
//
// What the tray shows is uicore's TrayMenu: the same rows the wx tray
// publishes, so a panel shows one thing whichever binary is running.

#pragma once

#include "Glib.hpp"
#include "TrayMenu.hpp"

#include "xpcog/core/Dispatcher.hpp"
#include "xpcog/core/Signal.hpp"
#include "xpcog/platform/Notifier.hpp"
#include "xpcog/platform/TrayIcon.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace xpcog::gtk {

class Presence {
public:
    explicit Presence(Dispatcher dispatch);
    ~Presence();

    Presence(const Presence&)            = delete;
    Presence& operator=(const Presence&) = delete;

    /// Whether a tray icon is on screen, which is what decides whether closing
    /// the window may hide it there.
    [[nodiscard]] bool hasTray() const { return hasTray_; }

    void setNowPlaying(const std::string& title, const std::string& artist);
    void setPlaybackState(bool playing, bool paused);
    void clear();

    /// A notification, with the cover when there is one. The cover is written
    /// to the cache directory and handed over as a path, which is what the
    /// notification daemon reads and what keeps an image decoder out of
    /// platform/.
    void notify(const std::string& title, const std::string& body,
                const std::shared_ptr<const std::vector<std::byte>>& cover);

    /// Takes the icon off the panel. On the way out, before the window goes.
    void remove();

    /// The icon was clicked, or its menu's Show row chosen.
    Signal<> showRequested;
    /// A menu row carrying a CommandId.
    Signal<int> commandActivated;

private:
    void refresh();

    std::unique_ptr<platform::TrayIcon>  tray_;
    std::unique_ptr<platform::Notifier>  notifier_;
    bool                                 hasTray_ = false;
    app::TrayState                       state_;
    std::vector<Subscription>            subscriptions_;
};

}  // namespace xpcog::gtk
