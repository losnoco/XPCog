// The notification area, on the desktops that still have one worth talking to.
//
// This is a seam that exists because wxWidgets' does not reach far enough. The
// application already has wxTaskBarIcon, it works on Windows, and on Linux it is
// GtkStatusIcon -- which speaks the XEmbed system-tray protocol and nothing else.
// XEmbed is X11: it reparents a real X window into the panel's. So under a
// Wayland session wxTaskBarIcon::IsAvailable() answers false before it looks at
// anything, and there is no argument to be had with it, because the transport it
// would use does not exist on that display server.
//
// What replaced XEmbed is StatusNotifierItem: the application exports a D-Bus
// object describing an icon, a tooltip and a menu, and the panel draws it. No
// window is shared, so the display server is irrelevant, and one implementation
// covers GNOME (through the AppIndicator extension), KDE, and every panel that
// grew SNI support -- which is all of the ones still maintained.
//
// So this is the third Linux integration written from a specification rather than
// ported from Cog, after MPRIS and for the same reason: Cog is macOS, macOS has a
// Dock, and there is nothing here to translate. The references are the
// StatusNotifierItem specification and com.canonical.dbusmenu, which is a
// separate interface and the part that is easy to skip -- an SNI item with no
// menu object appears in the panel and does nothing when clicked, which reads as
// a broken icon rather than as an unimplemented interface.
//
// --- What this deliberately is not ----------------------------------------
//
// Not a replacement for wxTaskBarIcon. Windows and macOS get the base class,
// which reports itself unavailable, and the application falls back to wx exactly
// as before. This is the Linux escape hatch, not a second tray abstraction to
// keep in step with the first, and the shape is chosen so the application asks
// one question -- isAvailable() -- rather than carrying a platform branch.
//
// Not a menu framework either. The menu is a flat list of rows pushed whole
// whenever it changes, because that is the entire shape of the tray menu the
// player has: five transport commands, two window commands, and two disabled
// rows naming the track. Submenus, checkmarks and radio groups are all
// expressible in dbusmenu and none of them are here, because adding them
// untested would be guessing at what a panel does with them.
//
// --- Threading -------------------------------------------------------------
//
// The same rule as MediaIntegration, for the same reason: a method call arrives
// on whichever thread pumps the bus, and a subscriber must not have to know that.
// Implementations reach a signal only through publishOnUiThread().
//
// In practice, on the one platform that implements this, the hop is a formality
// -- a GTK application's main loop is a GMainLoop on the default main context, so
// GDBus delivers on the UI thread already. It is still routed through the
// dispatcher, because "an implementation never publishes directly" is a rule
// worth having exactly one of.

#pragma once

#include "xpcog/core/Signal.hpp"
#include "xpcog/platform/Dispatcher.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace xpcog::platform {

/// One rendering of the icon, as raw pixels.
///
/// Pixels rather than a themed icon name, which is the other thing SNI accepts.
/// A name means an icon installed into the user's icon theme, and XPCog is a
/// program that may well be running out of a build directory -- an icon that
/// appears only after `make install` is an icon that is missing exactly when
/// somebody is testing this.
struct TrayImage {
    int width  = 0;
    int height = 0;

    /// `width * height * 4` bytes, one pixel at a time, alpha first and then red,
    /// green, blue. Straight alpha, not premultiplied.
    ///
    /// That byte order is the wire format, and it is worth stating rather than
    /// leaving to the implementation: SNI's pixmaps are ARGB32 in *network* byte
    /// order, so a little-endian machine writing a `uint32_t` gets BGRA and an
    /// icon whose red and blue are swapped. It looks deliberate enough that it
    /// survives review.
    std::vector<std::byte> argb;
};

/// One row of the tray's menu.
struct TrayMenuItem {
    /// The application's own command id, handed back through `menuItemActivated`
    /// and otherwise untouched -- this layer assigns the ids the protocol needs
    /// and keeps the mapping to itself.
    int id = 0;

    std::string label;
    bool        enabled = true;

    /// A rule rather than a row; `id` and `label` are ignored.
    bool separator = false;
};

class TrayIcon {
public:
    /// The implementation for this platform, or a do-nothing one where there is
    /// none. Never null, so callers have no branch to forget -- they ask
    /// isAvailable() instead, which is a question the do-nothing one answers
    /// correctly.
    ///
    /// `dispatch` must remain safe to call from any thread for as long as the
    /// returned object lives.
    [[nodiscard]] static std::unique_ptr<TrayIcon> create(Dispatcher dispatch);

    explicit TrayIcon(Dispatcher dispatch) : dispatch_(std::move(dispatch)) {}

    TrayIcon(const TrayIcon&)            = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    virtual ~TrayIcon() = default;

    /// Whether there is a panel that will actually show this.
    ///
    /// Answered for the session rather than the platform, and answered
    /// *synchronously at construction*, because the caller needs it before it can
    /// decide whether hiding its window is safe. On Linux it means a
    /// StatusNotifierItem host is on the session bus; it can go false later if
    /// that host exits, and true later if one arrives.
    [[nodiscard]] virtual bool isAvailable() const { return false; }

    /// The icon, at every size that is available.
    ///
    /// A set rather than one image so the panel picks instead of scaling: this is
    /// the whole reason the icon looked soft, or oversized, or clipped on a panel
    /// that was handed a single bitmap and had its own idea of the right height.
    virtual void setIcon(const std::vector<TrayImage>& sizes) { (void)sizes; }

    /// Hover text. `title` is the bold first line, `body` the rest; either may be
    /// empty, and a panel is free to show neither.
    virtual void setToolTip(const std::string& title, const std::string& body) {
        (void)title;
        (void)body;
    }

    /// The whole menu, replacing whatever was there.
    ///
    /// Whole rather than incremental because the menu is small and its contents
    /// change on every track: the diff would be larger than the thing being
    /// diffed, and a panel is told to re-read rather than handed the change.
    virtual void setMenu(const std::vector<TrayMenuItem>& items) { (void)items; }

    /// Take the icon down now, rather than when this object is destroyed.
    ///
    /// The window closing and the process exiting are not the same moment, and an
    /// icon left behind for the gap is one a listener can click to reach a window
    /// that is being torn down.
    virtual void remove() {}

    // --- Commands from the panel, all delivered on the UI thread ------------

    /// The icon was clicked. Conventionally: show the window.
    Signal<> activated;

    /// A menu row was chosen, carrying the `id` that row was given.
    Signal<int> menuItemActivated;

protected:
    /// Publishes `signal` on the user interface's thread. The only way a subclass
    /// should ever reach a signal above.
    template <typename... Args>
    void publishOnUiThread(Signal<Args...>& signal, Args... args) const {
        dispatch_([&signal, args...]() { signal.publish(args...); });
    }

private:
    Dispatcher dispatch_;
};

}  // namespace xpcog::platform
