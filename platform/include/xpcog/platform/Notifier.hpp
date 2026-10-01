// Telling the desktop what just started playing.
//
// This was wxNotificationMessage, reached from app/src/StatusPresence.cpp. It
// moves here for the reason SecretStore did: a second frontend wants the same
// thing, and "announce the track" is not a widget.
//
// What it deliberately is *not* is a general notification API. One title, one
// body, one optional image, and no actions, no replace-in-place, no urgency:
// that is the whole of what a player needs, and every extra field would be one
// more thing that behaves differently on three platforms.

#pragma once

#include "xpcog/platform/Dispatcher.hpp"
#include "xpcog/platform/TrayIcon.hpp"

#include <memory>
#include <optional>
#include <string>

namespace xpcog::platform {

struct Notification {
    std::string title;
    std::string body;

    /// The cover, where the platform can show one. TrayImage's layout, for the
    /// reason it has one: straight ARGB, alpha first, so nothing downstream has
    /// to guess at a byte order.
    std::optional<TrayImage> image;

    /// A file holding the cover, as an alternative to decoding it.
    ///
    /// Cog does this -- it writes the art out because UNNotificationAttachment
    /// takes a URL (PlaybackEventController.m:190-200) -- and it is the useful
    /// half here for a different reason: artwork is stored as the bytes the
    /// file carried, so handing over a path costs nothing where handing over
    /// pixels would mean linking an image decoder into a layer that has no
    /// other use for one.
    std::optional<std::string> imagePath;
};

class Notifier {
public:
    /// Never null. A platform with no implementation gets one that reports
    /// itself unavailable and shows nothing.
    [[nodiscard]] static std::unique_ptr<Notifier> create(Dispatcher dispatch);

    Notifier()          = default;
    virtual ~Notifier() = default;

    Notifier(const Notifier&)            = delete;
    Notifier& operator=(const Notifier&) = delete;

    [[nodiscard]] virtual bool isAvailable() const = 0;

    /// Shows one. Fire and forget: there is nothing to wait for and nothing
    /// useful to do about a failure, which on every platform means a daemon
    /// that is not running.
    virtual void show(const Notification& notification) = 0;
};

}  // namespace xpcog::platform
