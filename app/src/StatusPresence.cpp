#include "StatusPresence.hpp"

#include "AppIcon.hpp"
#include "Commands.hpp"
#include "MainFrame.hpp"
#include "Text.hpp"

#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/menu.h>
#include <wx/notifmsg.h>
#include <wx/translation.h>

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace xpcog::app {
namespace {

/// Ids for the two entries that exist only where there is a tray.
enum : int {
    kShowWindowId = FirstWidgetId + 80,
};

/// Long titles get cut rather than stretching a tooltip across the screen.
constexpr std::size_t kMaxTooltipRun = 60;

[[nodiscard]] std::string elide(const std::string& text) {
    if (text.size() <= kMaxTooltipRun) {
        return text;
    }
    // Cut on a byte boundary that is not mid-sequence: UTF-8 continuation bytes
    // are 10xxxxxx, so walking back off them lands on a character start. A
    // tooltip ending in half a codepoint renders as a replacement glyph.
    std::size_t cut = kMaxTooltipRun;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return text.substr(0, cut) + "\xE2\x80\xA6";
}

/// The application icon at every size a panel is plausibly going to want.
///
/// A set rather than one bitmap, and this is the fix for the way the icon looked
/// wrong before: handed a single image, a panel scales it to its own height, and
/// a 16px source on a 32px panel is the blur -- or, worse, a source larger than
/// the panel drawn at its own size and clipped by the panel's bounds. Given the
/// set, the panel picks.
///
/// Every size here is one applicationIconAt() has a real PNG for, so nothing in
/// this list is itself a scaled copy of something else.
[[nodiscard]] std::vector<platform::TrayImage> trayImages() {
    constexpr int kTraySizes[] = {16, 24, 32, 48, 64};

    std::vector<platform::TrayImage> images;
    for (const int size : kTraySizes) {
        const wxIcon icon = applicationIconAt(size);
        if (!icon.IsOk()) {
            continue;
        }
        wxBitmap bitmap;
        bitmap.CopyFromIcon(icon);
        const wxImage image = bitmap.ConvertToImage();
        if (!image.IsOk()) {
            continue;
        }

        platform::TrayImage out;
        out.width  = image.GetWidth();
        out.height = image.GetHeight();

        // wxImage keeps colour and alpha in two planes; the wire format wants
        // them interleaved, alpha first. Assembled a byte at a time rather than
        // through a uint32_t, which is what keeps this right on a little-endian
        // machine -- see TrayImage's note on why that particular mistake survives
        // being looked at.
        const unsigned char* rgb   = image.GetData();
        const unsigned char* alpha = image.HasAlpha() ? image.GetAlpha() : nullptr;
        const auto           pixels =
            static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height);
        out.argb.resize(pixels * 4);
        for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
            out.argb[(pixel * 4) + 0] =
                static_cast<std::byte>(alpha != nullptr ? alpha[pixel] : 0xFF);
            out.argb[(pixel * 4) + 1] = static_cast<std::byte>(rgb[(pixel * 3) + 0]);
            out.argb[(pixel * 4) + 2] = static_cast<std::byte>(rgb[(pixel * 3) + 1]);
            out.argb[(pixel * 4) + 3] = static_cast<std::byte>(rgb[(pixel * 3) + 2]);
        }
        images.push_back(std::move(out));
    }
    return images;
}

}  // namespace

void raiseWindow(wxTopLevelWindow* window) {
    if (window == nullptr) {
        return;
    }
    window->Iconize(false);
    window->Show();
    window->Raise();
    window->SetFocus();
}

// wxTBI_DOCK has to be asked for, and the header above claimed it was without
// anything passing it. On Cocoa wxTBI_DEFAULT_TYPE is wxTBI_CUSTOM_STATUSITEM,
// so the default ctor makes a *menu-bar* item -- a second permanent presence
// beside the Dock icon, which is exactly what the design note says not to build.
//
// It is guarded rather than passed everywhere because the three platforms do not
// agree on the signature: wxMSW and wxOSX take a wxTaskBarIconType, and
// wx/unix/taskbarx11.h declares `wxTaskBarIcon()` with no parameter at all.
#ifdef __WXOSX__
StatusPresence::StatusPresence(MainFrame* frame, platform::Dispatcher dispatch)
    : wxTaskBarIcon(wxTBI_DOCK), frame_(frame) {
#else
StatusPresence::StatusPresence(MainFrame* frame, platform::Dispatcher dispatch)
    : frame_(frame) {
#endif
    // Constructed on every platform, because the base class is a working
    // do-nothing and asking it is cheaper than an #ifdef around the question.
    tray_ = platform::TrayIcon::create(std::move(dispatch));

#ifndef __WXOSX__
    // StatusNotifierItem first, where a panel answers for it. It is preferred
    // rather than merely tried because it is the one that works on a Wayland
    // session, which is most of them -- and where both routes exist, it is also
    // the one whose icon the panel scales itself.
    if (tray_->isAvailable()) {
        trayActivated_ = tray_->activated.connect([this] { raiseWindow(frame_); });
        trayMenuActivated_ =
            tray_->menuItemActivated.connect([this](int id) { activateCommand(id); });
        tray_->setIcon(trayImages());
        hasTrayIcon_ = true;
    } else if (wxTaskBarIcon::IsAvailable()) {
        // wxTaskBarIcon::IsAvailable() answers for the session rather than the
        // platform: a Linux desktop with no notification area says no, and the
        // caller has to be able to tell that from "a tray that ignores updates".
        hasTrayIcon_ = SetIcon(applicationIconAt(16), "XPCog");
    }
#else
    // The Dock menu. Nothing to set on it, because the tile already carries the
    // bundle's icon and replacing it would lose the layered treatment macOS
    // composes from icons/xpcog.icon.
    hasTrayIcon_ = false;
#endif

    // Whichever route came up gets its tooltip and its menu, which is also the
    // first time the StatusNotifierItem one has a menu at all -- an item exported
    // without one appears in the panel and does nothing when clicked.
    refresh();

    Bind(wxEVT_TASKBAR_LEFT_DCLICK,
         [this](wxTaskBarIconEvent&) { raiseWindow(frame_); });
}

std::vector<platform::TrayMenuItem> StatusPresence::menuModel() const {
    // Id 0 is "no command", which is what the two track rows and the separators
    // are. Nothing can activate them, so nothing needs to know what they mean.
    constexpr int kNoCommand = 0;
    const auto    separator  = [] {
        return platform::TrayMenuItem{kNoCommand, {}, false, true};
    };

    std::vector<platform::TrayMenuItem> items;

    // The track, as two disabled rows at the top, exactly as Cog's dock menu
    // does. Absent rather than blank when there is nothing: an empty row reads as
    // a broken menu.
    if (!title_.empty()) {
        items.push_back({kNoCommand, elide(title_), false, false});
        if (!artist_.empty()) {
            items.push_back({kNoCommand, elide(artist_), false, false});
        }
        items.push_back(separator());
    }

    items.push_back({PlaybackPlayPause,
                     toUtf8(playing_ && !paused_ ? _("Pause") : _("Play")), true, false});
    items.push_back({PlaybackStop, toUtf8(_("Stop")), true, false});
    items.push_back(separator());
    items.push_back({PlaybackPrevious, toUtf8(_("Previous")), true, false});
    items.push_back({PlaybackNext, toUtf8(_("Next")), true, false});

#ifndef __WXOSX__
    // Only where there is a tray. AppKit appends Quit and the window list to a
    // Dock menu itself, and clicking the Dock icon already raises the window --
    // adding these there would produce a menu with two Quits.
    items.push_back(separator());
    items.push_back({kShowWindowId, toUtf8(_("Show XPCog")), true, false});
    items.push_back({FileQuit, toUtf8(_("Quit")), true, false});
#endif

    return items;
}

wxMenu* StatusPresence::CreatePopupMenu() {
    auto* menu = new wxMenu;

    for (const platform::TrayMenuItem& item : menuModel()) {
        if (item.separator) {
            menu->AppendSeparator();
            continue;
        }
        // wxID_ANY for the rows that carry no command: they are disabled, so the
        // id they are given is never seen again.
        wxMenuItem* appended =
            menu->Append(item.id != 0 ? item.id : wxID_ANY, toWx(item.label));
        if (!item.enabled) {
            appended->Enable(false);
        }
    }

    // The transport ids are the frame's, so the commands run there and pick up
    // the same EVT_UPDATE_UI handlers the menu bar uses. Only the show-window
    // entry belongs to this object.
    menu->Bind(wxEVT_MENU,
               [this](wxCommandEvent& event) { activateCommand(event.GetId()); });

    return menu;
}

void StatusPresence::activateCommand(int id) {
    if (id == kShowWindowId) {
        raiseWindow(frame_);
        return;
    }
    wxCommandEvent forwarded(wxEVT_MENU, id);
    frame_->GetEventHandler()->ProcessEvent(forwarded);
}

void StatusPresence::setNowPlaying(const std::string& title, const std::string& artist) {
    title_  = title;
    artist_ = artist;
    refresh();
}

void StatusPresence::setPlaybackState(bool playing, bool paused) {
    playing_ = playing;
    paused_  = paused;
    refresh();
}

void StatusPresence::clear() {
    title_.clear();
    artist_.clear();
    playing_ = false;
    paused_  = false;
    refresh();
}

std::string StatusPresence::tooltipBody() const {
    std::string body;
    if (!title_.empty()) {
        body = elide(title_);
        if (!artist_.empty()) {
            body += "\n" + elide(artist_);
        }
    }
    if (playing_ && paused_) {
        if (!body.empty()) {
            body += "\n";
        }
        body += toUtf8(_("(paused)"));
    }
    return body;
}

void StatusPresence::refresh() {
    if (!hasTrayIcon_) {
        return;
    }

    if (tray_->isAvailable()) {
        // The name is the bold line and the rest is the body, which is the split
        // StatusNotifierItem's tooltip already has -- where wx has one string and
        // the newlines below are all the structure there is.
        tray_->setToolTip("XPCog", tooltipBody());
        // Pushed on every change rather than built when the panel asks: the panel
        // does not call back into us to build a menu, it reads one we published,
        // and a menu published once shows the first track for the whole session.
        tray_->setMenu(menuModel());
        return;
    }

    wxString text = "XPCog";
    if (const std::string body = tooltipBody(); !body.empty()) {
        text += "\n" + toWx(body);
    }

    // The icon is passed again because wx has no tooltip-only setter; it is the
    // same bundle, so nothing is redrawn that was not already there.
    SetIcon(applicationIconAt(16), text);
}

bool StatusPresence::RemoveIcon() {
    // Both, unconditionally. Which one is up is this object's business, and the
    // frame calling this on the way out should not have to know.
    tray_->remove();
    hasTrayIcon_ = false;
    return wxTaskBarIcon::RemoveIcon();
}

void StatusPresence::notify(const std::string& title, const std::string& body,
                            const wxIcon& icon) {
    // wxNotificationMessage rather than a balloon: ShowBalloon() is MSW-only,
    // and this is the portable spelling that also reaches a Linux desktop's
    // notification daemon.
    wxNotificationMessage message(toWx(title), toWx(body), frame_);

    if (icon.IsOk()) {
        // Honoured in the MSW balloon path, which passes it straight to
        // ShowBalloon (wxWidgets/src/msw/notifmsg.cpp). Elsewhere it is a request
        // the platform may decline, which is why nothing branches on it.
        message.SetIcon(icon);
    }

#if defined(__WXMSW__)
    // Only when there is one to offer. UseTaskBarIcon() hands wx *our* icon to
    // hang the balloon on; handing it one that was never installed would leave
    // the balloon with nothing to come from, where passing nothing at all lets
    // wx make its own.
    if (hasTrayIcon_) {
        message.UseTaskBarIcon(this);
    }
#endif

    message.Show();
}

}  // namespace xpcog::app
