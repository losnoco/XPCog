#include "StatusPresence.hpp"

#include "AppIcon.hpp"
#include "Commands.hpp"
#include "MainFrame.hpp"
#include "Text.hpp"

#include <wx/menu.h>
#include <wx/notifmsg.h>

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

StatusPresence::StatusPresence(MainFrame* frame) : frame_(frame) {
#ifndef __WXOSX__
    // wxTaskBarIcon::IsAvailable() answers for the session rather than the
    // platform: a Linux desktop with no notification area says no, and the
    // caller has to be able to tell that from "a tray that ignores updates".
    if (wxTaskBarIcon::IsAvailable()) {
        hasTrayIcon_ = SetIcon(applicationIconAt(16), "XPCog");
    }
#else
    // The Dock menu. Constructed by the wxTBI_DOCK base below; nothing to set,
    // because the tile already carries the bundle's icon and replacing it would
    // lose the layered treatment macOS composes from icons/xpcog.icon.
    hasTrayIcon_ = false;
#endif

    Bind(wxEVT_TASKBAR_LEFT_DCLICK,
         [this](wxTaskBarIconEvent&) { raiseWindow(frame_); });
}

wxMenu* StatusPresence::CreatePopupMenu() {
    auto* menu = new wxMenu;

    // The track, as two disabled rows at the top, exactly as Cog's dock menu
    // does. Absent rather than blank when there is nothing: an empty row reads as
    // a broken menu.
    if (!title_.empty()) {
        menu->Append(wxID_ANY, toWx(elide(title_)))->Enable(false);
        if (!artist_.empty()) {
            menu->Append(wxID_ANY, toWx(elide(artist_)))->Enable(false);
        }
        menu->AppendSeparator();
    }

    menu->Append(PlaybackPlayPause, playing_ && !paused_ ? "Pause" : "Play");
    menu->Append(PlaybackStop, "Stop");
    menu->AppendSeparator();
    menu->Append(PlaybackPrevious, "Previous");
    menu->Append(PlaybackNext, "Next");

#ifndef __WXOSX__
    // Only where there is a tray. AppKit appends Quit and the window list to a
    // Dock menu itself, and clicking the Dock icon already raises the window --
    // adding these there would produce a menu with two Quits.
    menu->AppendSeparator();
    menu->Append(kShowWindowId, "Show XPCog");
    menu->Append(FileQuit, "Quit");
#endif

    // The transport ids are the frame's, so the commands run there and pick up
    // the same EVT_UPDATE_UI handlers the menu bar uses. Only the show-window
    // entry belongs to this object.
    menu->Bind(wxEVT_MENU, [this](wxCommandEvent& event) {
        if (event.GetId() == kShowWindowId) {
            raiseWindow(frame_);
            return;
        }
        wxCommandEvent forwarded(wxEVT_MENU, event.GetId());
        frame_->GetEventHandler()->ProcessEvent(forwarded);
    });

    return menu;
}

void StatusPresence::setNowPlaying(const std::string& title, const std::string& artist) {
    title_  = title;
    artist_ = artist;
    refreshTooltip();
}

void StatusPresence::setPlaybackState(bool playing, bool paused) {
    playing_ = playing;
    paused_  = paused;
    refreshTooltip();
}

void StatusPresence::clear() {
    title_.clear();
    artist_.clear();
    playing_ = false;
    paused_  = false;
    refreshTooltip();
}

void StatusPresence::refreshTooltip() {
    if (!hasTrayIcon_) {
        return;
    }

    std::string text = "XPCog";
    if (!title_.empty()) {
        text += "\n" + elide(title_);
        if (!artist_.empty()) {
            text += "\n" + elide(artist_);
        }
    }
    if (playing_) {
        text += paused_ ? "\n(paused)" : "";
    }

    // The icon is passed again because wx has no tooltip-only setter; it is the
    // same bundle, so nothing is redrawn that was not already there.
    SetIcon(applicationIconAt(16), toWx(text));
}

void StatusPresence::notify(const std::string& title, const std::string& body) {
    if (!hasTrayIcon_) {
        return;
    }
    // wxNotificationMessage rather than a balloon: ShowBalloon() is MSW-only,
    // and this is the portable spelling that also reaches a Linux desktop's
    // notification daemon.
    wxNotificationMessage message(toWx(title), toWx(body), frame_);
#if defined(__WXMSW__)
    message.UseTaskBarIcon(this);
#endif
    message.Show();
}

}  // namespace xpcog::app
