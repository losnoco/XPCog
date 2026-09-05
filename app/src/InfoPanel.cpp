#include "InfoPanel.hpp"

#include "Text.hpp"

#include "xpcog/core/FilePath.hpp"
#include "xpcog/core/library/Library.hpp"

#include <wx/datetime.h>
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
namespace {

[[nodiscard]] std::string pad(long long value, int width) {
    std::string text = std::to_string(value);
    while (static_cast<int>(text.size()) < width) {
        text = "0" + text;
    }
    return text;
}

[[nodiscard]] std::string fixed(double value, int places) {
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "%.*f", places, value);
    return buffer;
}

[[nodiscard]] std::string join(const std::vector<std::string>& lines) {
    std::string joined;
    for (const std::string& line : lines) {
        if (!joined.empty()) {
            joined += '\n';
        }
        joined += line;
    }
    return joined;
}

}  // namespace

std::string trackText(std::int32_t track, std::int32_t disc) {
    if (track == 0) {
        return {};
    }
    if (disc == 0) {
        return pad(track, 2);
    }
    return std::to_string(disc) + "." + pad(track, 2);
}

std::string lengthText(double seconds) {
    if (seconds <= 0.0) {
        // A live stream has no length, and "0:00.000" would be a claim rather
        // than an absence.
        return {};
    }
    const auto milliseconds = static_cast<long long>((seconds * 1000.0) + 0.5);
    return std::to_string(milliseconds / 60000) + ":" +
           pad((milliseconds / 1000) % 60, 2) + "." + pad(milliseconds % 1000, 3);
}

std::string replayGainText(const ReplayGainInfo& gain) {
    std::vector<std::string> lines;
    const auto decibels = [](float value) {
        // The sign is always shown: +0.00 dB and -0.00 dB are different claims
        // from "no gain tag", and this panel exists to tell them apart.
        return (value < 0.0F ? "" : "+") + fixed(static_cast<double>(value), 2) + " dB";
    };
    const auto peak = [](float value) { return fixed(static_cast<double>(value), 6); };

    if (gain.albumGain) {
        lines.push_back(toUtf8(wxString::Format(_("Album Gain: %s"),
                                               toWx(decibels(*gain.albumGain)))));
    }
    if (gain.albumPeak) {
        lines.push_back(toUtf8(wxString::Format(_("Album Peak: %s"),
                                               toWx(peak(*gain.albumPeak)))));
    }
    if (gain.trackGain) {
        lines.push_back(toUtf8(wxString::Format(_("Track Gain: %s"),
                                               toWx(decibels(*gain.trackGain)))));
    }
    if (gain.trackPeak) {
        lines.push_back(toUtf8(wxString::Format(_("Track Peak: %s"),
                                               toWx(peak(*gain.trackPeak)))));
    }
    if (gain.soundcheck && !gain.soundcheck->empty()) {
        lines.push_back(toUtf8(wxString::Format(_("SoundCheck: %s"),
                                               toWx(*gain.soundcheck))));
    }
    // Cog's condition exactly: a volume of 1.0 is no scaling and says nothing.
    if (gain.volume && *gain.volume != 1.0F) {
        lines.push_back(toUtf8(wxString::Format(
            trUtf8("Volume Scale: %s\xC3\x97"),
            toWx(fixed(static_cast<double>(*gain.volume), 2)))));
    }
    return join(lines);
}

std::string playCountText(std::int64_t count, std::int64_t firstSeen,
                          std::int64_t lastPlayed) {
    const auto stamp = [](std::int64_t unixSeconds) {
        return toUtf8(wxDateTime(static_cast<time_t>(unixSeconds))
                          .Format(wxDefaultDateTimeFormat));
    };

    std::vector<std::string> lines;
    lines.push_back(std::to_string(count));
    if (firstSeen != 0) {
        lines.push_back(toUtf8(
            wxString::Format(_("First seen: %s"), toWx(stamp(firstSeen)))));
    }
    if (lastPlayed != 0) {
        lines.push_back(toUtf8(
            wxString::Format(_("Last played: %s"), toWx(stamp(lastPlayed)))));
    }
    return join(lines);
}

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

/// The step the cover's size moves in, in DIP.
///
/// Dragging the pane narrower changes the width a pixel at a time, and every
/// change that reaches the cover costs two rescales, two PNG encodes and a
/// reparse. Rounding the room down to a step means at most one of those per eight
/// pixels of drag, and the cover is never more than a step smaller than it could
/// be -- which is not visible and would not be worth seeing if it were.
constexpr int kArtStep = 8;

/// Cog's labels, in Cog's order.
///
/// Marked rather than translated: this is a file-scope table, so the lookup
/// happens where the page is built. Several of them are also playlist column
/// headings -- Title, Artist, Album -- and share a msgid with those on purpose:
/// one word, one translation, whichever surface shows it.
constexpr std::array<const char*, 20> kLabels = {
    wxTRANSLATE("Album Artist"), wxTRANSLATE("Artist"),
    wxTRANSLATE("Composer"),     wxTRANSLATE("Album"),
    wxTRANSLATE("Title"),        wxTRANSLATE("Track"),
    wxTRANSLATE("Length"),       wxTRANSLATE("Date"),
    wxTRANSLATE("Genre"),        wxTRANSLATE("Filename"),
    wxTRANSLATE("Sample Rate"),  wxTRANSLATE("Channels"),
    wxTRANSLATE("Bitrate"),      wxTRANSLATE("Bits"),
    wxTRANSLATE("Codec"),        wxTRANSLATE("Encoding"),
    wxTRANSLATE("Cuesheet"),     wxTRANSLATE("Replay Gain"),
    wxTRANSLATE("Play Count"),   wxTRANSLATE("Comment"),
};

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
    static_assert(kLabels.size() == static_cast<std::size_t>(FieldCount),
                  "every field needs a label, and in the same order");

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
        if (artSize() != artSize_) {
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

void InfoPanel::set(Field field, const std::string& value) {
    const auto index = static_cast<std::size_t>(field);
    if (index < values_.size()) {
        values_[index] = value;
    }
}

void InfoPanel::showEntry(const PlaylistEntry* entry) {
    if (entry == nullptr) {
        values_.fill(std::string{});
        cover_ = wxImage{};
        render();
        return;
    }

    set(AlbumArtist, entry->albumArtist);
    set(Artist, entry->artist);
    set(Composer, entry->composer);
    set(Album, entry->album);
    set(Title, entry->title());
    set(Track, info::trackText(entry->track, entry->disc));
    set(Length, info::lengthText(entry->duration()));
    // Cog binds `date`, not the year, and falls back to nothing rather than
    // inventing a January the first.
    set(Date, entry->date.empty() && entry->year != 0 ? std::to_string(entry->year)
                                                      : entry->date);
    set(Genre, entry->genre);

    // Cog shows the last path component here. The whole path instead, because
    // this panel is resizable where Cog's fixed HUD was not, and because a path
    // you can select and copy is a large part of why an info panel gets opened.
    const auto local = entry->url.localPath();
    set(Filename, local ? pathToUtf8(*local) : entry->url.toString());

    const TrackProperties& properties = entry->properties;
    set(SampleRate, properties.format.sampleRate > 0.0
                        ? std::to_string(static_cast<long long>(properties.format.sampleRate)) +
                              " Hz"
                        : std::string{});
    set(Channels, properties.format.channels > 0
                      ? std::to_string(properties.format.channels)
                      : std::string{});
    set(Bitrate, properties.bitrateKbps > 0
                     ? std::to_string(properties.bitrateKbps) + " kbps"
                     : std::string{});
    set(BitsPerSample, properties.format.bitsPerSample > 0
                           ? std::to_string(properties.format.bitsPerSample)
                           : std::string{});
    set(Codec, properties.codec);
    set(Encoding, properties.encoding);

    set(Cuesheet, toUtf8(properties.cuesheet && !properties.cuesheet->empty()
                             ? _("yes")
                             : _("no")));
    set(ReplayGain, info::replayGainText(properties.replayGain));

    // The count on the entry is what the playlist carries; the dates only exist
    // in the database, so a build without one shows the count alone.
    std::int64_t firstSeen  = 0;
    std::int64_t lastPlayed = 0;
    if (library_ != nullptr) {
        if (const auto record =
                library_->playCount(entry->artist, entry->album, entry->title());
            record.has_value()) {
            firstSeen  = record->firstSeen;
            lastPlayed = record->lastPlayed;
        }
    }
    set(PlayCount, info::playCountText(entry->playCount, firstSeen, lastPlayed));

    set(Comment, entry->comment);

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
    room = std::max(kArtStep, (room / kArtStep) * kArtStep);

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

void InfoPanel::render() {
    if (const wxSize wanted = artSize(); wanted != artSize_) {
        publishArt(wanted);
    }

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
        // The label goes through the same escape as the value: it is translated,
        // and a translator's ampersand would otherwise be an entity.
        const wxString label =
            toWx(info::escapeHtml(toUtf8(trUtf8(kLabels[field]))));
        wxString value = toWx(info::escapeHtml(values_[field]));
        if (value.IsEmpty()) {
            // An empty cell is a row of no height, and a field that is simply
            // absent should read as blank rather than as missing.
            value = "&nbsp;";
        }
        html += "<tr><td valign=\"top\" align=\"right\"><b>" + label +
                "</b></td><td valign=\"top\">" + value + "</td></tr>";
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
