// What this build is, and what it is made of.
//
// Three tabs rather than one wall of text: what the program is, what this
// particular build can play, and what it is built from. The middle one is read
// from the registry rather than written down, so it is true of the binary in
// front of you rather than of the source tree it came from.

#pragma once

#include "xpcog/core/PluginRegistry.hpp"

#include <wx/dialog.h>

namespace xpcog::app {

class AboutDialog : public wxDialog {
public:
    AboutDialog(wxWindow* parent, const PluginRegistry& registry);
};

}  // namespace xpcog::app
