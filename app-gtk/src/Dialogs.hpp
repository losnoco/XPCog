// The small dialogs: Open URL, the playlist save picker, the shortcuts, and a
// plain warning.
//
// Each is a function that presents something and calls back, because that is
// how GTK 4 dialogs work: nothing blocks, and a callback runs on the main
// context when the listener has answered. The deciding is the session's --
// which file to write -- and the wording is
// the wx frontend's under the same msgids.

#pragma once

#include "Session.hpp"

#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <vector>

namespace xpcog::gtk {

/// Asks for an address, with the history under it; `open` gets a URL that
/// parsed. The history setting is updated on the way.
void showOpenUrlDialog(GtkWindow* parent, Settings& settings, std::function<void(const Url&)> open);

/// Asks where to save the playlist, then writes it through the session. With
/// `selection` non-empty, those tracks; otherwise what is on screen.
void showSavePlaylistDialog(GtkWindow* parent, app::Session& session, std::vector<TrackId> selection);


/// One sentence with an OK button.
void showWarning(GtkWidget* parent, const std::string& heading, const std::string& body);

/// The keyboard shortcuts, one section per menu of the command table, each
/// row read from the accelerator GTK actually has for its action -- so the
/// dialog cannot list a key that does not work.
void showShortcutsDialog(GtkWidget* parent);

}  // namespace xpcog::gtk
