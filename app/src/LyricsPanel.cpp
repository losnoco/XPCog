#include "LyricsPanel.hpp"

#include "Text.hpp"

#include "xpcog/core/library/PlaylistEntry.hpp"

#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <utility>

namespace xpcog::app {
namespace {

/// Shown instead of an empty box when the file carries no lyrics.
///
/// A divergence from Cog, which leaves its window blank. A blank pane is
/// ambiguous in a way a blank *window* is not: the window was opened deliberately
/// and can only be about the one thing, while a pane sits in the layout all the
/// time and an empty one reads as "still loading" or "broken" rather than as an
/// answer. Saying it costs one line and removes the question.
constexpr const char* kNoLyrics = "This file carries no lyrics.";

constexpr const char* kNoTrack = "Nothing selected or playing.";

}  // namespace

std::string normaliseLyrics(std::string text) {
    // CRLF and lone CR both become LF. Windows taggers write CRLF, some write a
    // bare CR, and a `\r` that reaches a GTK text control is drawn as a box
    // rather than as a line break -- so this is a correctness fix on Linux and a
    // tidying one elsewhere.
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                continue;  // the LF of a CRLF pair carries the break
            }
            out.push_back('\n');
            continue;
        }
        out.push_back(text[i]);
    }

    // Trailing blank lines are common -- a tagger padding the field, or a lyrics
    // site's copy-paste -- and in a scrolling control they are indistinguishable
    // from the song having more to say.
    const auto end = out.find_last_not_of(" \t\n");
    if (end == std::string::npos) {
        return {};
    }
    out.erase(end + 1);

    // Leading blank lines push the first line out of view for no reason.
    const auto begin = out.find_first_not_of(" \t\n");
    if (begin != std::string::npos && begin > 0) {
        out.erase(0, begin);
    }
    return out;
}

LyricsPanel::LyricsPanel(wxWindow* parent) : wxPanel(parent, wxID_ANY) {
    // Which track these belong to. Cog's window needs no such line -- it is a
    // window you opened about the track you were looking at -- but a pane that
    // follows the selection changes underneath you as you arrow down the
    // playlist, and lyrics are the one kind of text where recognising the song
    // from its content is exactly what you cannot rely on.
    heading_ = new wxStaticText(this, wxID_ANY, wxEmptyString);
    heading_->SetForegroundColour(
        wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));

    // A text control rather than a wxStaticText, for InfoPanel's reason: lyrics
    // are there to be read, and often to be copied. Read-only, so it cannot be
    // edited into disagreeing with the file -- nothing here writes tags.
    //
    // No wxHSCROLL, which is what makes it wrap; with it, a long line would run
    // off the side of a pane the reader has deliberately made narrow.
    text_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                           wxDefaultSize,
                           wxTE_MULTILINE | wxTE_READONLY | wxTE_BESTWRAP);

    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(heading_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    layout->Add(text_, 1, wxEXPAND | wxALL, FromDIP(8));
    SetSizer(layout);

    showEntry(nullptr);
}

void LyricsPanel::showEntry(const PlaylistEntry* entry) {
    std::string heading;
    std::string body;

    if (entry == nullptr) {
        body = kNoTrack;
    } else {
        heading = entry->artist.empty() ? entry->title()
                                        : entry->artist + " \xE2\x80\x94 " + entry->title();
        body    = normaliseLyrics(entry->unsyncedLyrics);
        if (body.empty()) {
            body = kNoLyrics;
        }
    }

    // The guard the class comment explains: replacing a wxTextCtrl's value scrolls
    // it back to the top, and this is called on every selection and track change.
    // Keyed on both halves, because the same lyrics under a different heading is a
    // different track -- two rips of one song, or a file appearing twice.
    std::string key = heading + '\n' + body;
    if (key == shownKey_) {
        return;
    }
    shownKey_ = std::move(key);

    heading_->SetLabel(toWx(heading));
    text_->SetValue(toWx(body));
    // SetValue leaves the insertion point at the end on some platforms, and the
    // control scrolls to wherever that is. Asking for the top explicitly is the
    // difference between opening a song at its first line and at its last.
    text_->SetInsertionPoint(0);
    text_->ShowPosition(0);

    // The heading is a single line whose text just changed length; without this
    // the sizer keeps the width it computed for the previous track.
    Layout();
}

}  // namespace xpcog::app
