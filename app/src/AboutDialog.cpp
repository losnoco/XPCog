#include "AboutDialog.hpp"

#include "Credits.hpp"
#include "Text.hpp"

#include "xpcog/core/Version.hpp"

#include <wx/button.h>
#include <wx/html/htmlwin.h>
#include <wx/notebook.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/translation.h>

#include <map>
#include <span>
#include <string>
#include <string_view>

namespace xpcog::app {
namespace {

[[nodiscard]] std::string buildInfo() {
#if defined(__clang__)
    const std::string compiler =
        "Clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__);
#elif defined(_MSC_VER)
    const std::string compiler = "MSVC " + std::to_string(_MSC_VER);
#elif defined(__GNUC__)
    const std::string compiler =
        "GCC " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#else
    const std::string compiler = "unknown compiler";
#endif
    return compiler + " \xC2\xB7 wxWidgets " + std::string{wxVERSION_NUM_DOT_STRING};
}

[[nodiscard]] wxHtmlWindow* page(wxWindow* parent, const wxString& html) {
    auto* view = new wxHtmlWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                  wxHW_SCROLLBAR_AUTO | wxHW_NO_SELECTION);
    view->SetPage(html);
    return view;
}

/// The separator between extensions.
///
/// A wxString built once through FromUTF8, not a `const char*` appended to one:
/// see the rule at the top of Text.hpp. `formats += " \xC2\xB7 "` compiles and
/// puts two mangled characters on screen.
[[nodiscard]] const wxString& separator() {
    static const wxString middot = wxString::FromUTF8(" \xC2\xB7 ");
    return middot;
}

/// What this build can play, grouped by the decoder that claims it.
///
/// Read from the registry rather than written down: this says what *this* build
/// does, which is the question someone opening the tab is asking. It used to be
/// one flat run of extensions, which answered "can it play a .gym" and nothing
/// else -- not which of the thirty-odd decoders would take it, and not why two
/// builds with the same extension list behave differently.
///
/// The order is the registry's own, which after freeze() is descending priority:
/// a file is offered to these rows top to bottom. That makes the one genuinely
/// confusing case legible instead of hidden -- an extension claimed by more than
/// one decoder goes to whichever appears first, which is how FFmpeg ends up
/// below the dedicated decoders rather than swallowing everything.
[[nodiscard]] wxString formatsPage(const PluginRegistry& registry) {
    // Counted first, so a shared extension can be marked in every row that
    // claims it rather than only in the ones after the first.
    std::map<std::string_view, int> claims;
    for (const DecoderDescriptor& decoder : registry.decoders()) {
        for (const std::string_view extension : decoder.extensions) {
            ++claims[extension];
        }
    }

    wxString html =
        "<p>" +
        wxString::Format(
            wxPLURAL("%zu decoder is compiled in. A file goes to the first row "
                     "below that claims its extension:",
                     "%zu decoders are compiled in. A file goes to the first row "
                     "below that claims its extension:",
                     static_cast<unsigned>(registry.decoderCount())),
            registry.decoderCount()) +
        "</p><table cellpadding='4'>";

    bool anyShared = false;
    for (const DecoderDescriptor& decoder : registry.decoders()) {
        wxString extensions;
        for (const std::string_view extension : decoder.extensions) {
            if (!extensions.IsEmpty()) {
                extensions += separator();
            }
            extensions += toWx(extension);
            if (claims[extension] > 1) {
                extensions += "*";
                anyShared = true;
            }
        }
        // A decoder claiming no extension at all is not a fault: silence:// and
        // the HLS decoder are chosen by scheme and by MIME type. Saying so beats
        // an empty cell that reads as a bug.
        if (extensions.IsEmpty()) {
            extensions = "<i>" + _("chosen by scheme or MIME type") + "</i>";
        }

        // The descriptor's own name, untranslated: it is the identifier the
        // codec registers under and the one a bug report should quote, which is
        // the same reason the Advanced pane shows setting idents verbatim.
        html += "<tr><td valign='top'><b>" + toWx(decoder.name) +
                "</b></td><td><tt>" + extensions + "</tt></td></tr>";
    }
    html += "</table>";

    if (anyShared) {
        html += "<p>" +
                trUtf8("An extension marked * is claimed by more than one "
                       "decoder. The first row that claims it wins "
                       "\xE2\x80\x94 which is what keeps FFmpeg, deliberately "
                       "registered below the rest, from taking files a "
                       "dedicated decoder handles better.") +
                "</p>";
    }
    return html;
}

/// One table of components, with its heading.
[[nodiscard]] wxString componentRows(const wxString&               heading,
                                     std::span<const Component>    components) {
    wxString rows = "<p><b>" + heading + "</b></p><table cellpadding='4'>";
    for (const Component& component : components) {
        rows += wxString("<tr><td><b>") + wxString::FromAscii(component.name) +
                "</b></td><td>" + wxString::FromAscii(component.licence) +
                "</td><td>" + trUtf8(component.purpose) +
                "</td></tr>";
    }
    return rows + "</table>";
}

}  // namespace

AboutDialog::AboutDialog(wxWindow* parent, const PluginRegistry& registry)
    : wxDialog(parent, wxID_ANY, _("About XPCog"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
    SetSize(FromDIP(wxSize(560, 460)));

    auto* title = new wxStaticText(this, wxID_ANY, "XPCog");
    wxFont titleFont = title->GetFont();
    titleFont.SetPointSize(titleFont.GetPointSize() + 10);
    titleFont.SetWeight(wxFONTWEIGHT_BOLD);
    title->SetFont(titleFont);

    // The toolkit's version is part of the build line rather than a row in the
    // table below: the table says what is linked, and this says which of it.
    auto* version = new wxStaticText(
        this, wxID_ANY,
        wxString::Format(trUtf8("Version %s \xC2\xB7 %s"),
                         toWx(kVersionString),
                         toWx(buildInfo())));
    version->Enable(false);

    auto* tabs = new wxNotebook(this, wxID_ANY);

    tabs->AddPage(
        page(tabs,
             wxString("<p>") + _("An audio player for Windows and Linux.") +
                 "</p><p>" +
                 trUtf8("Copyright \xC2\xA9 2026 the XPCog authors.") + "<br>" +
                 trUtf8("Copyright \xC2\xA9 2005\xE2\x80\x93""2026 Vincent Spader, "
                   "Christopher Snowhill and the Cog authors.") +
                 "</p><p>" +
                 _("XPCog is free software, licensed under the <b>GNU General Public "
                   "License, version 3 or later</b>. It comes with absolutely no "
                   "warranty.") +
                 "</p><p>" + _("Cog:") + " cog.losno.co<br>" + _("Source:") +
                 " github.com/losnoco/XPCog</p>"),
        _("About"));

    tabs->AddPage(page(tabs, formatsPage(registry)), _("Formats"));

    wxString licences =
        "<p>" +
        trUtf8("XPCog is built from the following third-party components. Which decoder "
               "libraries a given build actually contains depends on how it was "
               "configured \xE2\x80\x94 the Formats tab lists what <i>this</i> build "
          "can play. Each library's own licence text ships with its sources.") +
        "</p>";
    // The shared table in uicore/src/Credits.cpp, with this frontend's own
    // toolkit rows; the GTK About credits the same list.
    licences += componentRows(_("The player"), playerComponents());
    licences += componentRows(_("The interface"), wxComponents());
    licences += componentRows(_("Decoding and tags"), codecComponents());
    licences += componentRows(_("Data"), dataComponents());
    tabs->AddPage(page(tabs, licences), _("Licences"));

    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    layout->Add(version, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    layout->Add(tabs, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    if (wxSizer* buttons = CreateStdDialogButtonSizer(wxCLOSE); buttons != nullptr) {
        layout->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(12));
    }
    SetSizer(layout);

    // wxID_CLOSE does not end a modal dialog on its own the way wxID_CANCEL does.
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CLOSE); }, wxID_CLOSE);
}

}  // namespace xpcog::app
