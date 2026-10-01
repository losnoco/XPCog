#include "LyricsPanel.hpp"

#include "Text.hpp"

#include "xpcog/platform/AccentColour.hpp"

#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <utility>

namespace xpcog::app {
namespace {

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

[[nodiscard]] Rgb rgbOf(const wxColour& colour) {
    return {colour.Red(), colour.Green(), colour.Blue()};
}

/// How often a followed track's position is read. The sung line changes every
/// few seconds; a tenth of one is late by less than anyone reading can notice,
/// and the work per tick is a binary search.
constexpr int kTickMs = 100;

/// Lines kept in view below the one being sung, so that what comes next is on
/// screen before it is needed rather than scrolled up from the bottom edge as
/// it starts.
constexpr std::size_t kLookAhead = 3;

}  // namespace

wxColour readableOn(const wxColour& colour, const wxColour& background, const wxColour& text) {
    const Rgb out = readableOn(rgbOf(colour), rgbOf(background), rgbOf(text));
    return wxColour(out.red, out.green, out.blue);
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

    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); });

    showEntry(nullptr, false);
}

LyricsPanel::~LyricsPanel() { timer_.Stop(); }

void LyricsPanel::setLookup(LyricsLookup* lookup) { presenter_.setLookup(lookup); }

void LyricsPanel::showEntry(const PlaylistEntry* entry, bool playing) {
    playing_ = playing && entry != nullptr;
    // What to say is the presenter's; this is the drawing. The answer that
    // arrives later is dispatched on this thread, and the presenter has
    // already checked it is still for the track on screen. Nothing to draw
    // still means the track may have started or stopped playing, which
    // changes whether it is followed and nothing else.
    std::optional<LyricsText> text =
        presenter_.show(entry, [this](const LyricsText& answered) { present(answered); });
    if (text) {
        present(*text);
    } else {
        updateFollowing();
    }
}

void LyricsPanel::present(const LyricsText& text) {
    heading_->SetLabel(toWx(text.heading));

    // Replacing the value drops every style, so whatever was marked as sung is
    // not any more.
    sung_   = SyncedLyrics::npos;
    synced_ = text.synced;
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

    text_->SetValue(toWx(text.body));
    // SetValue leaves the insertion point at the end on some platforms, and the
    // control scrolls to wherever that is. Asking for the top explicitly is the
    // difference between opening a song at its first line and at its last.
    text_->SetInsertionPoint(0);
    text_->ShowPosition(0);

    source_->SetLabel(toWx(text.source));
    source_->Show(!text.source.empty());

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
