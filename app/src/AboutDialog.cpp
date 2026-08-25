#include "AboutDialog.hpp"

#include "Text.hpp"

#include "xpcog/core/Version.hpp"

#include <wx/button.h>
#include <wx/html/htmlwin.h>
#include <wx/notebook.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/translation.h>

#include <array>
#include <map>
#include <span>
#include <string>
#include <string_view>

namespace xpcog::app {
namespace {

struct Component {
    const char* name;
    const char* licence;
    /// Translated: this is the one column that is prose. The other two are a
    /// proper noun and an SPDX identifier, and translating either would make the
    /// row harder to check against the library it names, not easier.
    const char* purpose;
};

/// What is actually linked in. Kept here rather than generated, because a licence
/// list has to be right rather than convenient -- a library dropped from the
/// build should be removed deliberately, not vanish silently.
///
/// Split in two because the two halves answer different questions. The first is
/// what the player is built out of and is in every build; the second is what
/// decodes something, and which of those rows a given build actually contains
/// depends on how it was configured -- which is what the Formats tab reports.
constexpr std::array kApplicationComponents = {
    Component{"wxWidgets", "wxWindows Licence", wxTRANSLATE("user interface")},
    Component{"SQLite", "public domain", wxTRANSLATE("library database")},
    Component{"miniaudio", "public domain / MIT-0", wxTRANSLATE("audio output")},
    Component{"zlib", "zlib licence", wxTRANSLATE("decompression, throughout")},
    Component{"libcurl", "curl licence", wxTRANSLATE("HTTP and internet radio")},
    Component{"nlohmann/json", "MIT", wxTRANSLATE("reading Last.fm's replies")},
    Component{"libsoxr", "LGPL-2.1", wxTRANSLATE("sample-rate conversion")},
    Component{"Rubber Band", "GPL-2.0", wxTRANSLATE("pitch and tempo")},
    Component{"Signalsmith Stretch", "MIT", wxTRANSLATE("pitch and tempo")},
    Component{"FreeSurround", "GPL-2.0", wxTRANSLATE("upmixing stereo to surround")},
    Component{"dsd2pcm", "BSD", wxTRANSLATE("DSD to PCM conversion")},
    Component{"hdcd_decode2", "BSD-2-Clause", wxTRANSLATE("HDCD decoding")},
    Component{"LPC extrapolation", "ISC-style", wxTRANSLATE("gapless edges")},
    Component{"NanoSVG", "zlib", wxTRANSLATE("drawing the interface icons")},
    Component{"Lucide", "ISC", wxTRANSLATE("the interface icons themselves")},
    Component{"sentry-native", "MIT", wxTRANSLATE("opt-in crash reporting")},
};

constexpr std::array kCodecComponents = {
    Component{"FLAC", "BSD-3-Clause", wxTRANSLATE("FLAC")},
    Component{"libogg / libvorbis", "BSD-3-Clause", wxTRANSLATE("Ogg Vorbis")},
    Component{"Opus / opusfile", "BSD-3-Clause", wxTRANSLATE("Opus")},
    Component{"minimp3", "CC0-1.0", wxTRANSLATE("MP3")},
    Component{"WavPack", "BSD-3-Clause", wxTRANSLATE("WavPack")},
    Component{"libmpcdec", "BSD-3-Clause", wxTRANSLATE("Musepack")},
    Component{"FFmpeg", "LGPL-2.1", wxTRANSLATE("AAC, ALAC, WMA and more")},
    Component{"TagLib", "LGPL-2.1 / MPL-1.1", wxTRANSLATE("tag reading")},
    Component{"libopenmpt", "BSD-3-Clause", wxTRANSLATE("tracker modules")},
    Component{"Game Music Emu", "LGPL-2.1", wxTRANSLATE("console chiptunes")},
    Component{"libarchive", "BSD-2-Clause", wxTRANSLATE("archives, and the SC-55 ROMs")},
    Component{"vgmstream", "ISC", wxTRANSLATE("game streaming formats")},
    Component{"libsidplayfp", "GPL-2.0", wxTRANSLATE("Commodore 64 SID")},
    Component{"AdPlug", "LGPL-2.1", wxTRANSLATE("AdLib and OPL2 formats")},
    Component{"libbinio", "LGPL-2.1", wxTRANSLATE("AdPlug's file reading")},
    Component{"libvgm", "GPL-2.0", wxTRANSLATE("VGM, S98, DRO and GYM")},
    Component{"Hively replayer", "BSD-3-Clause", wxTRANSLATE("AHX and Hively modules")},
    Component{"libjaytrax", "GPL-3.0", wxTRANSLATE("Syntrax modules")},
    Component{"SpessaSynth Core", "Apache-2.0", wxTRANSLATE("SoundFont synthesis")},
    Component{"Nuked OPL3", "GPL-2.0", wxTRANSLATE("OPL3 synthesis")},
    Component{"Nuked SC-55", "MAME licence", wxTRANSLATE("Roland SC-55 emulation")},
    Component{"psflib", "GPL-2.0", wxTRANSLATE("the PSF container")},
    Component{"HighlyExperimental", "GPL-2.0", wxTRANSLATE("PSF and PSF2 (PlayStation)")},
    Component{"HighlyQuixotic", "GPL-2.0", wxTRANSLATE("QSF (Capcom QSound)")},
    Component{"HighlyTheoretical", "GPL-3.0", wxTRANSLATE("DSF and SSF (Sega)")},
    Component{"lazyusf2", "GPL-2.0", wxTRANSLATE("USF (Nintendo 64)")},
    Component{"mGBA", "MPL-2.0", wxTRANSLATE("GSF (Game Boy Advance)")},
    Component{"snes9x", "Snes9x licence", wxTRANSLATE("SNSF (Super Nintendo)")},
    Component{"melonDS", "GPL-3.0", wxTRANSLATE("2SF (Nintendo DS)")},
    Component{"SSEQPlayer", "GPL-2.0", wxTRANSLATE("NCSF (Nintendo DS)")},
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
             wxString("<p>") + _("An audio player for Windows, macOS and Linux.") +
                 "</p><p>" +
                 trUtf8("Copyright \xC2\xA9 2026 the XPCog authors.") + "<br>" +
                 trUtf8("Copyright \xC2\xA9 2005\xE2\x80\x93""2026 Vincent Spader, "
                   "Christopher Snowhill and the Cog authors.") +
                 "</p><p>" +
                 _("XPCog is free software, licensed under the <b>GNU General Public "
                   "License, version 2 or later</b>. It comes with absolutely no "
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
    licences += componentRows(_("The player"), kApplicationComponents);
    licences += componentRows(_("Decoding and tags"), kCodecComponents);
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
