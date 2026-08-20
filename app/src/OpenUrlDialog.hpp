// Open a stream by typing its address.
//
// Port of Cog's OpenURLPanel (ThirdParty/OpenURLPanel), which is an editable
// combo box over the fifteen most recently used URLs. The history is the point
// rather than a flourish: a radio station is a URL nobody remembers and everyone
// returns to, and without it the feature is "paste it again every time".
//
// A plain modal dialog rather than Cog's window-modal sheet, because a sheet is
// an AppKit shape with no cross-platform equivalent.

#pragma once

#include "xpcog/core/Settings.hpp"

#include <wx/dialog.h>

#include <string>
#include <vector>

class wxComboBox;

namespace xpcog::app {

/// Splits the stored history newest-last. Free functions rather than members so
/// they can be tested without a display -- which is the whole of what the old
/// suite's test_openurldialog covered.
[[nodiscard]] std::vector<std::string> urlHistoryFrom(const std::string& stored);

/// Folds `url` into `history` and returns the result, newest last and capped.
/// A repeat moves to the end rather than being added twice, so re-opening a
/// station keeps it to hand instead of filling the list with itself.
[[nodiscard]] std::vector<std::string> urlHistoryWith(std::vector<std::string> history,
                                                      const std::string&       url);

/// Joins for storage. The inverse of urlHistoryFrom().
[[nodiscard]] std::string joinUrlHistory(const std::vector<std::string>& history);

class OpenUrlDialog : public wxDialog {
public:
    OpenUrlDialog(wxWindow* parent, Settings& settings);

    /// The address entered, trimmed. Only meaningful after ShowModal() returns
    /// wxID_OK, at which point it is guaranteed to parse.
    [[nodiscard]] std::string url() const;

private:
    void onOk(wxCommandEvent& event);

    Settings&   settings_;
    wxComboBox* input_ = nullptr;
};

}  // namespace xpcog::app
