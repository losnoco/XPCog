#include "LyricsPanel.hpp"

#include "Text.hpp"

#include "xpcog/core/library/PlaylistEntry.hpp"
#include "xpcog/platform/AccentColour.hpp"

#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/translation.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace xpcog::app {
namespace {

/// WCAG relative luminance of an sRGB colour.
[[nodiscard]] double luminance(const wxColour& colour) {
    const auto channel = [](unsigned char value) {
        const double c = value / 255.0;
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(colour.Red()) + 0.7152 * channel(colour.Green()) +
           0.0722 * channel(colour.Blue());
}

[[nodiscard]] double contrast(const wxColour& a, const wxColour& b) {
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

/// The desktop's accent, or the toolkit's selection colour where there is none
/// -- SeekBar's rule, for SeekBar's reason. wxSYS_COLOUR_HIGHLIGHT outright is
/// what the sung line used to be drawn in, and on macOS that is
/// selectedTextBackgroundColor: a pastel made to sit *behind* text, which is
/// why the line was hard to read in front of it.
[[nodiscard]] wxColour accent() {
    if (const std::optional<platform::AccentRgb> rgb = platform::accentColour()) {
        return wxColour(rgb->red, rgb->green, rgb->blue);
    }
    return wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
}

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

constexpr const char* kFromLrclib  = wxTRANSLATE("From LRCLIB");
constexpr const char* kFromSidecar = wxTRANSLATE("From the .lrc file beside the track");

/// How often a followed track's position is read. The sung line changes every
/// few seconds; a tenth of one is late by less than anyone reading can notice,
/// and the work per tick is a binary search.
constexpr int kTickMs = 100;

/// Lines kept in view below the one being sung, so that what comes next is on
/// screen before it is needed rather than scrolled up from the bottom edge as
/// it starts.
constexpr std::size_t kLookAhead = 3;

/// What the pane draws: the body, the line under it naming where the words came
/// from (empty for the tag), and the timed words the body was made from, if
/// they were timed.
struct Drawn {
    wxString                    body;
    wxString                    source;
    std::optional<SyncedLyrics> synced;
};

/// What the redraw guard compares. Whether the words are timed is part of it:
/// a timed file with no breaks and no repeats reads the same either way, and
/// switching between the two must still redraw.
[[nodiscard]] std::string keyOf(const std::string& heading, const wxString& body,
                                bool timed) {
    return heading + (timed ? "\n\x01" : "\n") + toUtf8(body);
}

[[nodiscard]] Drawn plain(const std::string& text, wxString source) {
    Drawn drawn;
    drawn.body   = toWx(normaliseLyrics(text));
    drawn.source = std::move(source);
    return drawn;
}

[[nodiscard]] Drawn timed(SyncedLyrics lyrics, wxString source) {
    Drawn drawn;
    drawn.body   = toWx(syncedDisplayText(lyrics));
    drawn.source = std::move(source);
    drawn.synced = std::move(lyrics);
    return drawn;
}

/// Timed words as the pane should draw them: followed, or stripped to text.
[[nodiscard]] Drawn either(SyncedLyrics lyrics, wxString source, bool asTimed) {
    if (asTimed) {
        return timed(std::move(lyrics), std::move(source));
    }
    return plain(lyrics.plainText(), std::move(source));
}

/// What the file has to show, in the order the header comment gives; an empty
/// body when it has nothing.
[[nodiscard]] Drawn fromFile(const PlaylistEntry& entry, bool asTimed) {
    const std::string& tag = entry.unsyncedLyrics.str();
    if (auto lyrics = parseLrc(tag)) {
        return either(std::move(*lyrics), wxString{}, asTimed);
    }
    // Shown as text, the tag's words are as good as the file's and they are
    // the listener's own; only timing put the file ahead of them.
    if (!asTimed && !normaliseLyrics(tag).empty()) {
        return plain(tag, wxString{});
    }
    if (const auto sidecar = readSidecarLrc(entry.url)) {
        if (auto lyrics = parseLrc(*sidecar)) {
            return either(std::move(*lyrics), trUtf8(kFromSidecar), asTimed);
        }
    }
    return plain(tag, wxString{});
}

/// What the pane shows for an answer from the service.
[[nodiscard]] Drawn drawnOf(const LyricsLookup::Answer& answer, bool asTimed) {
    Drawn drawn;
    switch (answer.outcome) {
    case LyricsLookup::Outcome::Found: {
        auto lyrics = parseLrc(answer.synced);
        if (lyrics && asTimed) {
            return timed(std::move(*lyrics), trUtf8(kFromLrclib));
        }
        // The service derives a plain copy from a timed one, but an entry
        // uploaded with only the timed file is not guaranteed to carry it.
        if (normaliseLyrics(answer.lyrics).empty() && lyrics) {
            return plain(lyrics->plainText(), trUtf8(kFromLrclib));
        }
        return plain(answer.lyrics, trUtf8(kFromLrclib));
    }
    case LyricsLookup::Outcome::Instrumental:
        drawn.body = trUtf8(kInstrumental);
        return drawn;
    case LyricsLookup::Outcome::NotFound:
        drawn.body = trUtf8(kNotFound);
        return drawn;
    case LyricsLookup::Outcome::Failed:
        break;
    }
    switch (answer.error.kind) {
    case LyricsError::Kind::Transport:
        drawn.body = trUtf8(kUnreachable);
        break;
    case LyricsError::Kind::Transient:
        drawn.body = trUtf8(kBusy);
        break;
    default:
        drawn.body = wxString::Format(trUtf8(kRefused), answer.error.code);
        break;
    }
    return drawn;
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

std::string syncedDisplayText(const SyncedLyrics& lyrics) {
    // Every line, breaks included, and nothing trimmed from the ends: line N
    // of this text is line N of the file, which is the whole of what the
    // highlight needs to find it.
    std::string out;
    for (std::size_t i = 0; i < lyrics.lines.size(); ++i) {
        if (i > 0) {
            out.push_back('\n');
        }
        out += lyrics.lines[i].text;
    }
    return out;
}

wxColour readableOn(const wxColour& colour, const wxColour& background,
                    const wxColour& text) {
    constexpr double kReadable = 4.5;
    if (contrast(colour, background) >= kReadable) {
        return colour;
    }
    // Tenths are fine enough: the eye cannot tell adjacent steps apart, and the
    // loop ends at `text` itself, which is readable by construction.
    for (int step = 1; step <= 10; ++step) {
        const double   t     = step / 10.0;
        const auto     mix   = [t](unsigned char from, unsigned char to) {
            return static_cast<unsigned char>(std::lround(from + (to - from) * t));
        };
        const wxColour mixed(mix(colour.Red(), text.Red()), mix(colour.Green(), text.Green()),
                             mix(colour.Blue(), text.Blue()));
        if (contrast(mixed, background) >= kReadable) {
            return mixed;
        }
    }
    return text;
}

LyricsPanel::LyricsPanel(wxWindow* parent, std::function<double()> position)
    : wxPanel(parent, wxID_ANY), position_(std::move(position)), timer_(this) {
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
    //
    // wxTE_RICH2 so the sung line can be styled: Windows' plain edit control
    // has one style for all of its text. GTK and macOS ignore the flag.
    text_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                           wxDefaultSize,
                           wxTE_MULTILINE | wxTE_READONLY | wxTE_BESTWRAP | wxTE_RICH2);

    // Under the text rather than in the heading, and hidden rather than blank
    // when the words are the file's own: the heading names the track, and
    // "Artist -- Title -- LRCLIB" reads as a third name.
    source_ = new wxStaticText(this, wxID_ANY, wxEmptyString);
    source_->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    source_->Hide();

    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(heading_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    layout->Add(text_, 1, wxEXPAND | wxALL, FromDIP(8));
    layout->Add(source_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    SetSizer(layout);

    alive_ = std::make_shared<int>(0);

    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); });

    showEntry(nullptr, false);
}

LyricsPanel::~LyricsPanel() { timer_.Stop(); }

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

void LyricsPanel::showEntry(const PlaylistEntry* entry, bool playing) {
    playing_ = playing && entry != nullptr;

    std::string heading;
    // The body is a wxString rather than a std::string, because most of the
    // things it can hold come out of the catalogue. normaliseLyrics() stays on
    // std::string: it is the part with a test on it, and what a tagger wrote
    // is not language this program chose.
    Drawn drawn;

    // Set when the track on screen has to be asked about; the key the answer
    // must still match to be drawn.
    std::optional<LyricsQuery> ask;
    std::string                askKey;

    if (entry == nullptr) {
        drawn.body = trUtf8(kNoTrack);
    } else {
        heading = entry->artist.empty() ? entry->title()
                                        : entry->artist.str() + " \xE2\x80\x94 " + entry->title();
        drawn   = fromFile(*entry, timed_);
        if (drawn.body.IsEmpty()) {
            drawn.body = trUtf8(kNoLyrics);
            // The file has none. The service is asked only now -- a tag is the
            // listener's own and is never second-guessed -- and only about a
            // track it could know.
            if (lookup_ != nullptr) {
                if (auto query = queryFor(*entry)) {
                    askKey = LyricsLookup::keyOf(*query);
                    if (const auto known = lookup_->cached(*query)) {
                        drawn = drawnOf(*known, timed_);
                    } else {
                        drawn.body = trUtf8(kLooking);
                        ask        = std::move(query);
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
    // key, and must not ask again. So does the track on screen starting or
    // stopping, which changes whether it is followed and nothing else.
    std::string key = keyOf(heading, drawn.body, drawn.synced.has_value());
    if (key == shownKey_) {
        updateFollowing();
        return;
    }

    shownHeading_ = heading;
    heading_->SetLabel(toWx(heading));
    present(drawn.body, drawn.source, std::move(drawn.synced));

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

    Drawn drawn = drawnOf(answer, timed_);
    present(drawn.body, drawn.source, std::move(drawn.synced));
}

void LyricsPanel::present(const wxString& body, const wxString& source,
                          std::optional<SyncedLyrics> synced) {
    shownKey_ = keyOf(shownHeading_, body, synced.has_value());

    // Replacing the value drops every style, so whatever was marked as sung is
    // not any more.
    sung_ = SyncedLyrics::npos;
    synced_ = std::move(synced);
    lineStarts_.clear();
    lineLengths_.clear();
    if (synced_) {
        // Positions counted the way the control counts them: in wxString
        // characters, one for each line break. Counting here rather than
        // asking the control, because XYToPosition() means the wrapped visual
        // line on Windows and the logical one on GTK.
        long at = 0;
        for (const LrcLine& line : synced_->lines) {
            const long length = static_cast<long>(toWx(line.text).length());
            lineStarts_.push_back(at);
            lineLengths_.push_back(length);
            at += length + 1;
        }
    }

    text_->SetValue(body);
    // SetValue leaves the insertion point at the end on some platforms, and the
    // control scrolls to wherever that is. Asking for the top explicitly is the
    // difference between opening a song at its first line and at its last.
    text_->SetInsertionPoint(0);
    text_->ShowPosition(0);

    source_->SetLabel(source);
    source_->Show(!source.IsEmpty());

    // The heading is a single line whose text just changed length, and the
    // source line has just appeared or gone; without this the sizer keeps the
    // layout it computed for the previous track.
    Layout();

    updateFollowing();
}

void LyricsPanel::updateFollowing() {
    if (!playing_ || !synced_ || !position_) {
        timer_.Stop();
        if (sung_ != SyncedLyrics::npos) {
            styleLine(sung_, false);
            sung_ = SyncedLyrics::npos;
        }
        return;
    }
    if (!timer_.IsRunning()) {
        timer_.Start(kTickMs);
    }
    tick();
}

void LyricsPanel::tick() {
    // A pane docked out of sight keeps its timer -- MainFrame redraws it on the
    // way back in -- but has nothing to do meanwhile.
    if (!synced_ || !position_ || !IsShownOnScreen()) {
        return;
    }
    const std::size_t line = synced_->lineAt(position_());
    if (line == sung_) {
        return;
    }
    if (sung_ != SyncedLyrics::npos) {
        styleLine(sung_, false);
    }
    sung_ = line;
    if (line == SyncedLyrics::npos) {
        return;
    }
    styleLine(line, true);

    // Left alone while the reader has something selected: they are copying
    // it, and scrolling it away mid-drag is the pane working against them.
    long from = 0;
    long to   = 0;
    text_->GetSelection(&from, &to);
    if (from != to) {
        return;
    }
    // Ahead first, then the line itself: ShowPosition() scrolls as little as
    // it can, so this leaves the lines to come in view without ever pushing
    // the sung one out of it.
    const std::size_t ahead = std::min(line + kLookAhead, lineStarts_.size() - 1);
    text_->ShowPosition(lineStarts_[ahead]);
    text_->ShowPosition(lineStarts_[line]);
}

void LyricsPanel::styleLine(std::size_t index, bool sung) {
    if (index >= lineStarts_.size() || lineLengths_[index] == 0) {
        return;
    }
    // A whole font, never just a weight. wxTextAttr::GetFont() fills in what
    // an attribute leaves unset with fixed defaults -- ten points, and the
    // toolkit's generic face -- so a line styled bold by weight alone came out
    // small on macOS and in "Sans" rather than the desktop font on GTK, and
    // stayed that way after it was sung.
    wxFont font = text_->GetFont();
    font.SetWeight(sung ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);

    const wxColour text = text_->GetForegroundColour();
    wxTextAttr     attr;
    attr.SetFont(font);
    attr.SetTextColour(sung ? readableOn(accent(), text_->GetBackgroundColour(), text)
                            : text);
    const long start = lineStarts_[index];
    text_->SetStyle(start, start + lineLengths_[index], attr);
}

}  // namespace xpcog::app
