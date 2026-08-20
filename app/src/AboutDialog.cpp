#include "AboutDialog.hpp"

#include "Text.hpp"

#include "xpcog/core/Version.hpp"

#include <wx/button.h>
#include <wx/html/htmlwin.h>
#include <wx/notebook.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <array>
#include <string>

namespace xpcog::app {
namespace {

struct Component {
    const char* name;
    const char* licence;
    const char* purpose;
};

/// What is actually linked in. Kept here rather than generated, because a licence
/// list has to be right rather than convenient -- a library dropped from the
/// build should be removed deliberately, not vanish silently.
constexpr std::array kComponents = {
    Component{"wxWidgets 3.3", "wxWindows Licence", "user interface"},
    Component{"FLAC", "BSD-3-Clause", "FLAC decoding"},
    Component{"libvorbis / libogg", "BSD-3-Clause", "Ogg Vorbis decoding"},
    Component{"Opus / opusfile", "BSD-3-Clause", "Opus decoding"},
    Component{"minimp3", "CC0-1.0", "MP3 decoding"},
    Component{"WavPack", "BSD-3-Clause", "WavPack decoding"},
    Component{"FFmpeg", "LGPL-2.1", "AAC, ALAC, WMA and more"},
    Component{"TagLib", "LGPL-2.1 / MPL-1.1", "tag reading"},
    Component{"libsoxr", "LGPL-2.1", "sample-rate conversion"},
    Component{"SQLite", "public domain", "library database"},
    Component{"miniaudio", "public domain / MIT-0", "audio output"},
    Component{"hdcd_decode2", "BSD-2-Clause", "HDCD decoding"},
    Component{"NanoSVG", "zlib", "interface icons"},
};

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
    return compiler + " \xC2\xB7 " + std::string{wxVERSION_NUM_DOT_STRING};
}

[[nodiscard]] wxHtmlWindow* page(wxWindow* parent, const std::string& html) {
    auto* view = new wxHtmlWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                  wxHW_SCROLLBAR_AUTO | wxHW_NO_SELECTION);
    view->SetPage(toWx(html));
    return view;
}

}  // namespace

AboutDialog::AboutDialog(wxWindow* parent, const PluginRegistry& registry)
    : wxDialog(parent, wxID_ANY, "About XPCog", wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
    SetSize(FromDIP(wxSize(560, 460)));

    auto* title = new wxStaticText(this, wxID_ANY, "XPCog");
    wxFont titleFont = title->GetFont();
    titleFont.SetPointSize(titleFont.GetPointSize() + 10);
    titleFont.SetWeight(wxFONTWEIGHT_BOLD);
    title->SetFont(titleFont);

    auto* version = new wxStaticText(
        this, wxID_ANY,
        toWx("Version " + std::string{kVersionString} + " \xC2\xB7 " + buildInfo()));
    version->Enable(false);

    auto* tabs = new wxNotebook(this, wxID_ANY);

    tabs->AddPage(
        page(tabs,
             "<p>An audio player for Windows, macOS and Linux.</p>"
             "<p>Copyright \xC2\xA9 2026 the XPCog authors.<br>"
             "Copyright \xC2\xA9 2005\xE2\x80\x93""2026 Vincent Spader, Christopher "
             "Snowhill and the Cog authors.</p>"
             "<p>XPCog is free software, licensed under the "
             "<b>GNU General Public License, version 2 or later</b>. It comes with "
             "absolutely no warranty.</p>"
             "<p>Cog: cog.losno.co<br>"
             "Source: github.com/losnoco/XPCog</p>"),
        "About");

    // Read from the registry rather than written down: this says what *this*
    // build can play, which is the question someone opening the tab is asking.
    std::string formats = "<p>" + std::to_string(registry.decoderCount()) +
                          " decoders compiled in, claiming these extensions:</p>"
                          "<p><tt>";
    bool first = true;
    for (const std::string& extension : registry.allExtensions()) {
        if (!first) {
            formats += " \xC2\xB7 ";
        }
        formats += extension;
        first = false;
    }
    formats += "</tt></p>";
    tabs->AddPage(page(tabs, formats), "Formats");

    std::string licences =
        "<p>XPCog is built from the following third-party components. Which codec "
        "libraries a given build actually contains depends on how it was "
        "configured \xE2\x80\x94 the Formats tab lists what <i>this</i> build can "
        "play.</p><table cellpadding='4'>";
    for (const Component& component : kComponents) {
        licences += std::string{"<tr><td><b>"} + component.name + "</b></td><td>" +
                    component.licence + "</td><td>" + component.purpose + "</td></tr>";
    }
    licences += "</table>";
    tabs->AddPage(page(tabs, licences), "Licences");

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
