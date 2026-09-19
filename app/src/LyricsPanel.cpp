#include "LyricsPanel.hpp"

#include "Text.hpp"

#include "xpcog/core/library/PlaylistEntry.hpp"

#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/translation.h>

#include <optional>
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
constexpr const char* kNoLyrics = wxTRANSLATE("This file carries no lyrics.");

constexpr const char* kNoTrack = wxTRANSLATE("Nothing selected or playing.");

// The online states. Each starts by restating that the file has none, because
// that is still true and still the first thing a reader wants to know: what
// follows is what was done about it.
constexpr const char* kLooking =
    wxTRANSLATE("This file carries no lyrics. Asking LRCLIB...");
constexpr const char* kNotFound =
    wxTRANSLATE("This file carries no lyrics, and LRCLIB has none for it.");
constexpr const char* kInstrumental =
    wxTRANSLATE("This file carries no lyrics. LRCLIB lists it as an instrumental.");
constexpr const char* kUnreachable =
    wxTRANSLATE("This file carries no lyrics. LRCLIB could not be reached.");
constexpr const char* kBusy =
    wxTRANSLATE("This file carries no lyrics. LRCLIB is busy; try again in a moment.");
constexpr const char* kRefused =
    wxTRANSLATE("This file carries no lyrics. LRCLIB could not answer (HTTP %d).");

/// What the pane shows for an answer, and whether the words are the service's.
[[nodiscard]] wxString bodyOf(const LyricsLookup::Answer& answer, bool& fromService) {
    fromService = false;
    switch (answer.outcome) {
    case LyricsLookup::Outcome::Found:
        fromService = true;
        return toWx(normaliseLyrics(answer.lyrics));
    case LyricsLookup::Outcome::Instrumental:
        return trUtf8(kInstrumental);
    case LyricsLookup::Outcome::NotFound:
        return trUtf8(kNotFound);
    case LyricsLookup::Outcome::Failed:
        break;
    }
    switch (answer.error.kind) {
    case LyricsError::Kind::Transport:
        return trUtf8(kUnreachable);
    case LyricsError::Kind::Transient:
        return trUtf8(kBusy);
    default:
        return wxString::Format(trUtf8(kRefused), answer.error.code);
    }
}

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

    // Under the text rather than in the heading, and hidden rather than blank
    // when the words are the file's own: the heading names the track, and
    // "Artist -- Title -- LRCLIB" reads as a third name.
    source_ = new wxStaticText(this, wxID_ANY, _("From LRCLIB"));
    source_->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    source_->Hide();

    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(heading_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    layout->Add(text_, 1, wxEXPAND | wxALL, FromDIP(8));
    layout->Add(source_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    SetSizer(layout);

    alive_ = std::make_shared<int>(0);

    showEntry(nullptr);
}

void LyricsPanel::setLookup(LyricsLookup* lookup) {
    lookup_ = lookup;
    // Whatever was being waited for is being waited for from a lookup that
    // may no longer be the one asked; the redraw the caller does next asks
    // again, and the old answer, if it comes, finds no key to match.
    awaiting_.clear();
}

std::optional<LyricsQuery> LyricsPanel::queryFor(const PlaylistEntry& entry) {
    // No artist means nothing to ask: the service matches on both, and a title
    // alone -- "Track 3", or a stream's ICY name -- would match anything or
    // nothing. The service's own rule, applied here so the request is not
    // made only to be refused. rawTitle rather than title(), which falls back
    // to the file name: "01 - Something.flac" is not a question worth a
    // round trip and a week in the cache as "not found".
    if (entry.artist.empty() || entry.rawTitle.empty()) {
        return std::nullopt;
    }
    LyricsQuery query;
    query.title  = entry.rawTitle;
    query.artist = entry.artist.str();
    query.album  = entry.album.str();
    // Unknown lengths -- live streams -- go as zero, which the client leaves
    // out of the request rather than sending as a number that cannot match.
    query.duration = entry.duration();
    return query;
}

void LyricsPanel::showEntry(const PlaylistEntry* entry) {
    std::string heading;
    // A wxString rather than a std::string, because most of the things it can
    // hold come out of the catalogue. normaliseLyrics() stays on std::string:
    // it is the part with a test on it, and what a tagger wrote is not
    // language this program chose.
    wxString body;
    bool     fromService = false;

    // Set when the track on screen has to be asked about; the key the answer
    // must still match to be drawn.
    std::optional<LyricsQuery> ask;
    std::string                askKey;

    if (entry == nullptr) {
        body = trUtf8(kNoTrack);
    } else {
        heading = entry->artist.empty() ? entry->title()
                                        : entry->artist.str() + " \xE2\x80\x94 " + entry->title();
        body    = toWx(normaliseLyrics(entry->unsyncedLyrics));
        if (body.IsEmpty()) {
            body = trUtf8(kNoLyrics);
            // The file has none. The service is asked only now -- a tag is the
            // listener's own and is never second-guessed -- and only about a
            // track it could know.
            if (lookup_ != nullptr) {
                if (auto query = queryFor(*entry)) {
                    askKey = LyricsLookup::keyOf(*query);
                    if (const auto known = lookup_->cached(*query)) {
                        body = bodyOf(*known, fromService);
                    } else {
                        body = trUtf8(kLooking);
                        ask  = std::move(query);
                    }
                }
            }
        }
    }

    // The guard the class comment explains: replacing a wxTextCtrl's value scrolls
    // it back to the top, and this is called on every selection and track change.
    // Keyed on both halves, because the same lyrics under a different heading is a
    // different track -- two rips of one song, or a file appearing twice. A
    // redraw of a track still being asked about lands here too, with the same
    // key, and must not ask again.
    std::string key = heading + '\n' + toUtf8(body);
    if (key == shownKey_) {
        return;
    }

    shownHeading_ = heading;
    heading_->SetLabel(toWx(heading));
    present(body, fromService);

    // Answered, or being answered, for a different track than before: the key
    // an arriving answer has to match is this one now, or none.
    awaiting_ = ask ? askKey : std::string{};
    if (!ask) {
        return;
    }

    // The lookup deduplicates a query already in flight and remembers every
    // answer, so this is cheap to call for a track just scrolled past and
    // back to. The handler is dispatched on this thread; the weak pointer is
    // for the one dispatched after the window has gone.
    std::weak_ptr<int> guard = alive_;
    lookup_->lookup(*ask, [this, guard, askKey](const LyricsLookup::Answer& answer) {
        if (guard.expired()) {
            return;
        }
        showAnswer(askKey, answer);
    });
}

void LyricsPanel::showAnswer(const std::string& key, const LyricsLookup::Answer& answer) {
    // An answer for a track the listener has moved off. It is in the lookup's
    // cache for when they come back; drawing it now would put one song's
    // words under another's name.
    if (key != awaiting_) {
        return;
    }
    awaiting_.clear();

    bool           fromService = false;
    const wxString body        = bodyOf(answer, fromService);
    present(body, fromService);
}

void LyricsPanel::present(const wxString& body, bool fromService) {
    shownKey_ = shownHeading_ + '\n' + toUtf8(body);

    text_->SetValue(body);
    // SetValue leaves the insertion point at the end on some platforms, and the
    // control scrolls to wherever that is. Asking for the top explicitly is the
    // difference between opening a song at its first line and at its last.
    text_->SetInsertionPoint(0);
    text_->ShowPosition(0);

    source_->Show(fromService);

    // The heading is a single line whose text just changed length, and the
    // source line has just appeared or gone; without this the sizer keeps the
    // layout it computed for the previous track.
    Layout();
}

}  // namespace xpcog::app
