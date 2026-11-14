// The tag editor: rows for the promoted fields, the file's other tags below
// them where the format has room, and a cover section where it has pictures.
//
// All the deciding lives in app::TagEditorModel -- which rows, what notes,
// what a Save hands back -- so this file is only GTK widgets and the plumbing
// that keeps them and the model answering each other. That is also why it
// never asks what format the file is: the probe said, once, into caps.

#pragma once

#include "Session.hpp"

#include <gtk/gtk.h>

#include <vector>

namespace xpcog::gtk {

/// Opens the editor for the files behind `ids`. Does nothing when none of
/// them is a file a writer claims; the window gates its menu item on the
/// same question, so this is the guard rather than the UX.
void showTagEditorDialog(GtkWindow* parent, app::Session& session,
                         const std::vector<TrackId>& ids);

}  // namespace xpcog::gtk
