#include "OpenUrlDialog.hpp"

#include "Text.hpp"

#include "xpcog/core/Url.hpp"

#include <wx/button.h>
#include <wx/combobox.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <algorithm>
#include <cctype>

namespace xpcog::app {
namespace {

/// Cog's kMaximumURLs.
constexpr std::size_t kMaxHistory = 15;

[[nodiscard]] std::string trim(std::string_view text) {
    const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    std::size_t begin = 0;
    while (begin < text.size() && space(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && space(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string{text.substr(begin, end - begin)};
}

}  // namespace

std::vector<std::string> urlHistoryFrom(const std::string& stored) {
    std::vector<std::string> entries;
    std::size_t              start = 0;
    for (;;) {
        const std::size_t newline = stored.find('\n', start);
        const std::string line =
            trim(newline == std::string::npos ? std::string_view{stored}.substr(start)
                                              : std::string_view{stored}.substr(
                                                    start, newline - start));
        if (!line.empty()) {
            entries.push_back(line);
        }
        if (newline == std::string::npos) {
            break;
        }
        start = newline + 1;
    }
    return entries;
}

std::vector<std::string> urlHistoryWith(std::vector<std::string> history,
                                        const std::string&       url) {
    history.erase(std::remove(history.begin(), history.end(), url), history.end());
    history.push_back(url);
    while (history.size() > kMaxHistory) {
        history.erase(history.begin());
    }
    return history;
}

std::string joinUrlHistory(const std::vector<std::string>& history) {
    std::string joined;
    for (const std::string& entry : history) {
        if (!joined.empty()) {
            joined += '\n';
        }
        joined += entry;
    }
    return joined;
}

OpenUrlDialog::OpenUrlDialog(wxWindow* parent, Settings& settings)
    : wxDialog(parent, wxID_ANY, "Open URL"), settings_(settings) {
    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(new wxStaticText(this, wxID_ANY, "Address of a stream or file:"), 0,
                wxALL, FromDIP(8));

    wxArrayString history;
    for (const std::string& entry : urlHistoryFrom(settings_.UrlHistory())) {
        history.Add(toWx(entry));
    }

    // wxCB_DROPDOWN is editable, and wx offers no inline completion to fight --
    // which the Qt version had to turn off explicitly, because Qt's completion
    // fought typing a new address sharing a prefix with an old one.
    input_ = new wxComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                            FromDIP(wxSize(420, -1)), history, wxCB_DROPDOWN);
    if (!history.IsEmpty()) {
        // Newest is last, so that is what should be showing.
        input_->SetSelection(static_cast<int>(history.GetCount()) - 1);
    }
    layout->Add(input_, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));

    if (wxSizer* buttons = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
        buttons != nullptr) {
        if (auto* ok = dynamic_cast<wxButton*>(FindWindow(wxID_OK)); ok != nullptr) {
            ok->SetLabel("Open");
        }
        layout->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(8));
    }

    SetSizerAndFit(layout);

    // Bound rather than letting wxID_OK close the dialog by itself, so a bad
    // address can be refused without the dialog disappearing first.
    Bind(wxEVT_BUTTON, &OpenUrlDialog::onOk, this, wxID_OK);

    input_->SetFocus();
}

std::string OpenUrlDialog::url() const { return trim(toUtf8(input_->GetValue())); }

void OpenUrlDialog::onOk(wxCommandEvent& event) {
    const std::string text = url();
    if (text.empty()) {
        return;
    }

    // Validated against the same parser that will be asked to open it. A more
    // permissive check would accept a bare path as a relative URL, so a mistyped
    // address would be taken here and fail silently later with nothing to point
    // at.
    if (!Url::parse(text).has_value()) {
        wxMessageBox(
            toWx("\xE2\x80\x9C" + text +
                 "\xE2\x80\x9D is not an address XPCog can open. It needs a scheme, "
                 "such as https:// or file://."),
            "Invalid URL", wxOK | wxICON_WARNING, this);
        return;
    }

    settings_.setUrlHistory(
        joinUrlHistory(urlHistoryWith(urlHistoryFrom(settings_.UrlHistory()), text)));

    event.Skip();  // lets the dialog close with wxID_OK
}

}  // namespace xpcog::app
