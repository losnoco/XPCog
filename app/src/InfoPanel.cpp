#include "InfoPanel.hpp"

#include "Text.hpp"

#include "xpcog/core/FilePath.hpp"
#include "xpcog/core/library/Library.hpp"

#include <wx/datetime.h>
#include <wx/dcclient.h>
#include <wx/filesys.h>
#include <wx/fs_mem.h>
#include <wx/menu.h>
#include <wx/mstream.h>
#include <wx/settings.h>
#include <wx/translation.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace xpcog::app {

namespace info {

std::string escapeHtml(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char character : text) {
        switch (character) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\r':
                // The LF of a CRLF pair carries the break; a bare CR is dropped
                // rather than drawn, which is what a tagger that writes them
                // means by it.
                break;
            case '\n':
                out += "<br>";
                break;
            default:
                out += character;
                break;
        }
    }
    return out;
}

}  // namespace info

namespace {

/// The height, in DIP, the cover art is drawn at when there is room for it.
constexpr int kArtHeight = 180;

/// The margin the page is drawn inside, in DIP.
constexpr int kBorder = 8;

/// The step the cover's size and the wrapping width move in, in DIP.
///
/// Dragging the pane narrower changes the width a pixel at a time, and every
/// change that reaches the cover costs two rescales, two PNG encodes and a
/// reparse. Rounding the room down to a step means at most one of those per eight
/// pixels of drag, and the cover is never more than a step smaller than it could
/// be -- which is not visible and would not be worth seeing if it were. The
/// width a path is wrapped to is rounded the same way and for the same reason.
constexpr int kStep = 8;

/// Cog's labels, in Cog's order, from the shared table.
const std::array<const char*, info::FieldCount>& kLabels = info::fieldLabels();

/// The memory filesystem is global and registered once.
///
/// wxWidgets has no way to ask whether a handler is already installed and adding
/// a second one is not an error, only waste, so the flag is the check.
void ensureMemoryFilesystem() {
    static const bool once = [] {
        wxFileSystem::AddHandler(new wxMemoryFSHandler);
        return true;
    }();
    (void)once;
}

/// The name wxHTML's IMG handler tries first on a Retina screen, which is the
/// base name with `@2x` before the extension (src/html/m_image.cpp).
[[nodiscard]] wxString hidpiName(const wxString& name) {
    return name.BeforeLast('.') + "@2x." + name.AfterLast('.');
}

/// A colour as `#rrggbb`, which is all wxHTML's parser reads.
[[nodiscard]] wxString htmlColour(wxSystemColour which) {
    return wxSystemSettings::GetColour(which).GetAsString(wxC2S_HTML_SYNTAX);
}

}  // namespace

InfoPanel::InfoPanel(wxWindow* parent, const Library* library)
    : wxHtmlWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                   wxHW_SCROLLBAR_AUTO),
      library_(library) {
    ensureMemoryFilesystem();

    // wxHTML's own defaults are a web browser's -- a serif face at a size of its
    // own choosing, which in a dock beside native controls looks like a page that
    // failed to load its stylesheet rather than like part of the application.
    // Only the size is given: an empty face means wxNORMAL_FONT's, which is the
    // system's, and is what the rest of the window is drawn in.
    SetStandardFonts(GetFont().GetPointSize());
    SetBorders(FromDIP(kBorder));

    // The cover is the only thing in the page whose size depends on the pane's,
    // and it is sized here rather than by a WIDTH attribute so that what wxHTML
    // scales is an image already close to its final size.
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        event.Skip();
        // Two things in the page are measured against the pane rather than laid
        // out by wxHTML: the cover's size, and the width a path is wrapped to.
        if (artSize() != artSize_ || valueWidth() != valueWidth_) {
            render();
        }
    });

    // Every colour in the page was read when it was built, so without this a
    // switch to dark mode leaves black text on a dark background -- the same
    // reason the toolbar restrokes its icons.
    Bind(wxEVT_SYS_COLOUR_CHANGED, [this](wxSysColourChangedEvent& event) {
        event.Skip();
        render();
    });

    // Nothing on screen says a page can be selected, and this is a panel people
    // open to copy a path out of. Ctrl+C -- Cmd+C on macOS -- already works;
    // wxHtmlWindow's own table handles wxID_COPY, so only Select All is bound.
    Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent&) {
        wxMenu menu;
        menu.Append(wxID_COPY, _("&Copy") + "\tCtrl+C");
        menu.Append(wxID_SELECTALL, _("Select &All"));
        menu.Enable(wxID_COPY, !SelectionToText().IsEmpty());
        PopupMenu(&menu);
    });
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { SelectAll(); }, wxID_SELECTALL);

    render();
}

InfoPanel::~InfoPanel() {
    // The memory filesystem outlives the panel and is shared with everything
    // else in the process, so what this put in it has to come back out.
    publishArt(wxSize(0, 0));
}

void InfoPanel::showEntry(const PlaylistEntry* entry) {
    if (entry == nullptr) {
        values_.fill(std::string{});
        cover_ = wxImage{};
        render();
        return;
    }

    // The text is the shared table's; what this panel adds is the page.
    values_ = info::describe(*entry, library_);

    wxImage cover;
    if (library_ != nullptr && !entry->artHash.empty()) {
        // Shared rather than copied: the same cover is wanted by the info
        // panel and the now-playing display, and it is only being read from.
        const auto bytes = library_->sharedArtwork(entry->artHash);
        if (bytes && !bytes->empty()) {
            wxMemoryInputStream stream(bytes->data(), bytes->size());
            wxImage             image;
            // wxBITMAP_TYPE_ANY: the artwork is whatever the file carried, which
            // is usually JPEG and sometimes PNG.
            if (image.LoadFile(stream, wxBITMAP_TYPE_ANY) && image.GetHeight() > 0 &&
                image.GetWidth() > 0) {
                cover = image;
            }
        }
    }
    cover_ = cover;

    render();
}

wxSize InfoPanel::artSize() {
    if (!cover_.IsOk() || cover_.GetWidth() <= 0 || cover_.GetHeight() <= 0) {
        return {0, 0};
    }

    // What is left of the pane once the page's own margins and a vertical
    // scrollbar are taken out.
    //
    // Measured from the window rather than from the client area, and with the
    // scrollbar subtracted whether or not one is there, because the client area
    // is downstream of the answer: a cover sized to the full client width can add
    // the height that summons a scrollbar, which narrows the client, which
    // shrinks the cover, which dismisses the scrollbar. The outer width does not
    // move, so nothing here can oscillate.
    const int scrollbar = std::max(0, wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, this));
    int       room =
        ToDIP(std::max(0, GetSize().GetWidth() - scrollbar)) - (2 * kBorder);
    room = std::max(kStep, (room / kStep) * kStep);

    int height = kArtHeight;
    if ((cover_.GetWidth() * height) / cover_.GetHeight() > room) {
        height = std::max(1, (cover_.GetHeight() * room) / cover_.GetWidth());
    }
    return {std::max(1, (cover_.GetWidth() * height) / cover_.GetHeight()), height};
}

void InfoPanel::publishArt(wxSize size) {
    if (!artFile_.IsEmpty()) {
        wxMemoryFSHandler::RemoveFile(artFile_);
        if (retina_) {
            wxMemoryFSHandler::RemoveFile(hidpiName(artFile_));
        }
        artFile_.clear();
    }
    artSize_ = size;
    retina_  = false;
    if (!cover_.IsOk() || size.GetWidth() <= 0 || size.GetHeight() <= 0) {
        return;
    }

    // A name nothing else is using, and a new one every time: wxHTML holds the
    // image it parsed, and reusing a name would make "did this page change?" a
    // question about the filesystem's contents rather than about the page.
    static unsigned serial = 0;
    const wxString  name   = wxString::Format("xpcog-info-%u.png", ++serial);

    wxImage scaled =
        cover_.Scale(size.GetWidth(), size.GetHeight(), wxIMAGE_QUALITY_HIGH);
    wxMemoryFSHandler::AddFile(name, scaled, wxBITMAP_TYPE_PNG);
    artFile_ = name;

    // Only where wxHTML will go looking for it, which is a Retina screen: on
    // anything else the second encode is pure waste, and this runs on every step
    // of a drag.
    if (GetContentScaleFactor() > 1.0) {
        wxImage doubled = cover_.Scale(size.GetWidth() * 2, size.GetHeight() * 2,
                                       wxIMAGE_QUALITY_HIGH);
        wxMemoryFSHandler::AddFile(hidpiName(name), doubled, wxBITMAP_TYPE_PNG);
        retina_ = true;
    }
}

int InfoPanel::labelWidth() {
    wxClientDC dc(this);
    wxFont     bold = GetFont();
    bold.SetWeight(wxFONTWEIGHT_BOLD);
    dc.SetFont(bold);

    int widest = 0;
    for (const char* label : kLabels) {
        widest = std::max(widest, dc.GetTextExtent(trUtf8(label)).GetWidth());
    }
    return widest;
}

int InfoPanel::valueWidth() {
    // An estimate, and it only has to be a close one. wxHTML decides the real
    // column widths after this has already chosen where the path breaks, so
    // being a little under means a line wraps a little early -- where being over
    // would mean the horizontal scrollbar this exists to avoid.
    const int scrollbar = std::max(0, wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, this));
    const int padding   = FromDIP(kStep);  // cellpadding, both cells, both sides
    const int width     = GetSize().GetWidth() - scrollbar - (2 * FromDIP(kBorder)) -
                      padding - labelWidth();
    return std::max(FromDIP(48), (width / kStep) * kStep);
}

wxString InfoPanel::wrapValue(const std::string& value, int width, wxDC& dc) {
    wxString out;
    int      line = 0;
    for (const std::string& piece : info::breakPieces(value)) {
        const int pieceWidth = dc.GetTextExtent(toWx(piece)).GetWidth();
        if (line > 0 && line + pieceWidth > width) {
            out += "<br>";
            line = 0;
        }
        out += toWx(info::escapeHtml(piece));

        // A value that carries its own newlines -- a gain block, a play count --
        // starts its line again where they are, or every line after the first
        // would be measured as though it followed the ones before it.
        if (const std::size_t last = piece.find_last_of('\n');
            last == std::string::npos) {
            line += pieceWidth;
        } else {
            line = dc.GetTextExtent(toWx(piece.substr(last + 1))).GetWidth();
        }
    }
    return out;
}

void InfoPanel::render() {
    if (const wxSize wanted = artSize(); wanted != artSize_) {
        publishArt(wanted);
    }
    valueWidth_ = valueWidth();

    wxClientDC dc(this);
    dc.SetFont(GetFont());

    // wxHtmlWindow paints the background itself from wxSYS_COLOUR_WINDOW, but
    // nothing sets the text colour, and its default is black: on a dark theme
    // that is the whole panel unreadable.
    wxString html = "<html><body bgcolor=\"" + htmlColour(wxSYS_COLOUR_WINDOW) +
                    "\" text=\"" + htmlColour(wxSYS_COLOUR_WINDOWTEXT) + "\">";

    if (!artFile_.IsEmpty()) {
        html += "<p align=\"center\"><img src=\"memory:" + artFile_ + "\"></p>";
    }

    // The value column takes what the labels leave, which is what `width=100%`
    // on the table means to wxHTML's table layout.
    html += "<table width=\"100%\" cellpadding=\"2\" cellspacing=\"0\">";
    for (std::size_t field = 0; field < kLabels.size(); ++field) {
        // A field the file says nothing about takes no row. Cog's HUD is a fixed
        // form and shows all twenty labels whatever is behind them; a dock is not
        // fixed, and twelve empty rows are twelve rows of nothing between the
        // reader and the two they came to read.
        if (values_[field].empty()) {
            continue;
        }
        // The label goes through the same escape as the value: it is translated,
        // and a translator's ampersand would otherwise be an entity.
        const wxString label =
            toWx(info::escapeHtml(toUtf8(trUtf8(kLabels[field]))));
        html += "<tr><td valign=\"top\" align=\"right\"><b>" + label +
                "</b></td><td valign=\"top\">" +
                wrapValue(values_[field], valueWidth_, dc) + "</td></tr>";
    }
    html += "</table></body></html>";

    if (html == page_) {
        return;
    }
    page_ = html;

    // SetPage() scrolls back to the top, and this is called on every selection
    // change and every piece of metadata that arrives during a scan.
    const int scroll = GetViewStart().y;
    SetPage(html);
    Scroll(0, scroll);
}

}  // namespace xpcog::app
