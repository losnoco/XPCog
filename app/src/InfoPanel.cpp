#include "InfoPanel.hpp"

#include "Text.hpp"

#include "xpcog/core/FilePath.hpp"
#include "xpcog/core/library/Library.hpp"

#include <wx/datetime.h>
#include <wx/dcbuffer.h>
#include <wx/mstream.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

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
        lines.push_back("Album Gain: " + decibels(*gain.albumGain));
    }
    if (gain.albumPeak) {
        lines.push_back("Album Peak: " + peak(*gain.albumPeak));
    }
    if (gain.trackGain) {
        lines.push_back("Track Gain: " + decibels(*gain.trackGain));
    }
    if (gain.trackPeak) {
        lines.push_back("Track Peak: " + peak(*gain.trackPeak));
    }
    if (gain.soundcheck && !gain.soundcheck->empty()) {
        lines.push_back("SoundCheck: " + *gain.soundcheck);
    }
    // Cog's condition exactly: a volume of 1.0 is no scaling and says nothing.
    if (gain.volume && *gain.volume != 1.0F) {
        lines.push_back("Volume Scale: " + fixed(static_cast<double>(*gain.volume), 2) +
                        "\xC3\x97");
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
        lines.push_back("First seen: " + stamp(firstSeen));
    }
    if (lastPlayed != 0) {
        lines.push_back("Last played: " + stamp(lastPlayed));
    }
    return join(lines);
}

}  // namespace info

namespace {

/// The height the cover art is scaled to.
constexpr int kArtHeight = 180;

/// Cog's labels, in Cog's order.
constexpr std::array<const char*, 20> kLabels = {
    "Album Artist", "Artist",  "Composer",       "Album",     "Title",
    "Track",        "Length",  "Date",           "Genre",     "Filename",
    "Sample Rate",  "Channels", "Bitrate",       "Bits",      "Codec",
    "Encoding",     "Cuesheet", "Replay Gain",   "Play Count", "Comment",
};

}  // namespace

/// The cover, drawn to fit.
///
/// This replaces a wxStaticBitmap, and the difference is the whole of the
/// artwork fix. wxStaticBitmap draws its bitmap at the bitmap's own size and
/// lets the window clip whatever does not fit, so getting a cover on screen
/// intact means computing exactly the right size for it in advance, every time,
/// from a client width that is itself still settling. Get that arithmetic wrong
/// by a pixel in the wrong direction and the result is not a slightly-too-big
/// cover, it is a cropped one.
///
/// Drawing it instead inverts the dependency. Whatever rectangle the sizer ends
/// up handing over, the cover is scaled into it with its aspect ratio kept and
/// centred in what is left. There is no size this can be given that crops
/// anything, which makes the height asked for outside a question of taste rather
/// than a correctness requirement.
class ArtworkView : public wxWindow {
public:
    ArtworkView(wxWindow* parent, int maxHeight)
        : wxWindow(parent, wxID_ANY), maxHeight_(maxHeight) {
        // wxAutoBufferedPaintDC needs this, and a cover that is rescaled on its
        // way to the screen is exactly the case where a flickering background
        // would show.
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &ArtworkView::onPaint, this);
    }

    /// Takes the cover at its own size. An invalid image clears it.
    ///
    /// The original is kept rather than a scaled copy, so every resize scales
    /// once from the source instead of losing a little more detail each time the
    /// pane is dragged somewhere new.
    void setImage(const wxImage& image) {
        source_   = image;
        scaled_   = wxBitmap{};
        scaledAt_ = wxSize{};
        Refresh();
    }

    [[nodiscard]] bool hasImage() const { return source_.IsOk(); }

    /// Asks the sizer for the height this cover wants, given `room` of width.
    ///
    /// The minimum width stays small on purpose: the cover must never be the
    /// thing that decides how narrow the pane is allowed to get.
    void fitTo(int room) {
        if (!source_.IsOk()) {
            SetMinSize(wxSize(0, 0));
            return;
        }
        const int width  = std::max(1, room);
        int       height = maxHeight_;
        if ((source_.GetWidth() * height) / source_.GetHeight() > width) {
            height = std::max(1, (source_.GetHeight() * width) / source_.GetWidth());
        }
        SetMinSize(wxSize(FromDIP(24), height));
    }

private:
    void onPaint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();

        const wxSize room = GetClientSize();
        if (!source_.IsOk() || room.GetWidth() <= 0 || room.GetHeight() <= 0) {
            return;
        }

        // Fit inside rather than fill: whichever ratio is smaller wins, so
        // neither edge runs over.
        int width = std::min(room.GetWidth(),
                             (source_.GetWidth() * room.GetHeight()) / source_.GetHeight());
        width     = std::max(1, width);
        int height = std::min(room.GetHeight(),
                              (source_.GetHeight() * width) / source_.GetWidth());
        height     = std::max(1, height);

        if (scaledAt_ != wxSize(width, height)) {
            scaledAt_ = wxSize(width, height);
            scaled_   = wxBitmap(source_.Scale(width, height, wxIMAGE_QUALITY_HIGH));
        }
        dc.DrawBitmap(scaled_, (room.GetWidth() - width) / 2,
                      (room.GetHeight() - height) / 2, true);
    }

    wxImage  source_;
    wxBitmap scaled_;
    wxSize   scaledAt_;
    int      maxHeight_;
};

InfoPanel::InfoPanel(wxWindow* parent, const Library* library) : library_(library) {
    Create(parent, wxID_ANY);
    // Vertical only, and that is the whole of the art-sizing fix.
    //
    // Turning horizontal scrolling on was the wrong answer to "the cover overruns
    // a narrow pane": it let the content stay wide and gave the reader a
    // scrollbar to chase it with, when what a cover should do in a narrower pane
    // is get smaller. With the horizontal rate at zero the virtual width is the
    // client width, so nothing can be wider than the pane -- which makes shrinking
    // the only thing updateArt() can do, rather than one of two options.
    //
    // The text fields wrap instead of extending, which is why they are multiline.
    SetScrollRate(0, FromDIP(8));

    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        event.Skip();
        updateArt();
    });

    auto* layout = new wxBoxSizer(wxVERTICAL);

    art_ = new ArtworkView(this, FromDIP(kArtHeight));
    art_->Hide();
    // wxEXPAND rather than a centring flag: the control takes the whole width of
    // the row and centres the cover inside itself. Centring the *control* would
    // mean its width had to be exactly right, which is the arithmetic this is
    // getting away from.
    layout->Add(art_, 0, wxEXPAND | wxALL, FromDIP(8));

    // Two columns: a bold caption and a selectable value. wxFlexGridSizer with
    // the value column growable is what a form layout is here.
    auto* form = new wxFlexGridSizer(2, FromDIP(4), FromDIP(8));
    form->AddGrowableCol(1, 1);

    values_.reserve(FieldCount);
    for (std::size_t field = 0; field < kLabels.size(); ++field) {
        auto* caption = new wxStaticText(this, wxID_ANY, wxString::FromAscii(kLabels[field]));
        wxFont bold = caption->GetFont();
        bold.SetWeight(wxFONTWEIGHT_BOLD);
        caption->SetFont(bold);

        // Read-only and borderless so it reads as a label, multiline so a
        // cuesheet or a replay-gain block is not one long line -- and selectable,
        // which is the reason it is a text control at all.
        auto* value = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                     wxDefaultSize,
                                     wxTE_READONLY | wxTE_MULTILINE | wxTE_NO_VSCROLL |
                                         wxBORDER_NONE);
        value->SetBackgroundColour(GetBackgroundColour());
        // Narrow, so the form can be. A text control's default best width is
        // around a hundred pixels, and twenty of them in a growable column set a
        // floor the pane cannot go below -- which with horizontal scrolling off
        // would clip rather than wrap.
        value->SetMinSize(wxSize(FromDIP(48), -1));

        form->Add(caption, 0, wxALIGN_TOP);
        form->Add(value, 1, wxEXPAND);
        values_.push_back(value);
    }

    layout->Add(form, 1, wxEXPAND | wxALL, FromDIP(8));
    SetSizer(layout);
}

void InfoPanel::set(Field field, const std::string& value) {
    const auto index = static_cast<std::size_t>(field);
    if (index >= values_.size()) {
        return;
    }
    values_[index]->SetValue(toWx(value));

    // One line per newline, so a block grows rather than scrolling inside a
    // one-line box. Nothing here is tall enough to want a scrollbar of its own.
    const int lines = 1 + static_cast<int>(std::count(value.begin(), value.end(), '\n'));
    values_[index]->SetMinSize(
        wxSize(-1, values_[index]->GetCharHeight() * lines + FromDIP(4)));
}

void InfoPanel::showEntry(const PlaylistEntry* entry) {
    if (entry == nullptr) {
        for (wxTextCtrl* value : values_) {
            value->ChangeValue(wxEmptyString);
        }
        art_->setImage(wxImage{});
        art_->Hide();
        relayout();
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

    set(Cuesheet, properties.cuesheet && !properties.cuesheet->empty() ? "yes" : "no");
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
    art_->setImage(cover);

    updateArt();
    relayout();
}

void InfoPanel::updateArt() {
    if (art_ == nullptr) {
        return;
    }
    if (!art_->hasImage()) {
        art_->Hide();
        return;
    }

    // Whichever constraint binds first: the height a cover is worth showing at,
    // or the width there actually is. The second is what makes it shrink in a
    // narrow pane rather than stay tall.
    //
    // The margin is not only tidiness -- it absorbs the vertical scrollbar
    // appearing and disappearing. Without it a cover sized to exactly the client
    // width can add enough height to need a scrollbar, which narrows the client,
    // which resizes the cover, which removes the scrollbar again.
    const int margin = FromDIP(16);
    const int room   = std::max(FromDIP(48), GetClientSize().GetWidth() - margin);

    const wxSize before = art_->GetMinSize();
    art_->fitTo(room);
    art_->Show();

    // A resize sends a stream of these, and only the ones that actually move the
    // cover are worth a relayout. That test is also what stops this looping:
    // relayout() can change the client width, which sends another size event
    // straight back here.
    if (art_->GetMinSize() != before) {
        relayout();
    }
}

void InfoPanel::relayout() {
    wxSizer* sizer = GetSizer();
    if (sizer == nullptr) {
        return;
    }
    const wxSize client = GetClientSize();
    // The width is the client width, deliberately, and never the sizer's
    // minimum: horizontal scrolling is off, so anything laid out wider than this
    // is unreachable rather than merely off-screen.
    SetVirtualSize(client.GetWidth(),
                   std::max(sizer->CalcMin().GetHeight(), client.GetHeight()));
    Layout();
}

}  // namespace xpcog::app
