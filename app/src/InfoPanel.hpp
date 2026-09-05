// Everything known about one track.
//
// Port of Cog's InfoInspector (InfoWindowController + InfoInspector.xib), field
// for field and label for label. Cog's is a floating HUD panel it positions to
// the right of the main window by hand; this is a pane the frame shows and hides,
// which is the same thing with less arithmetic.
//
// The rule for *which* track is Cog's, from InfoWindowController's observer: the
// playlist selection when there is one, and the playing track otherwise. It is a
// good rule. Following the selection alone would blank the panel every time you
// clicked empty space, and following playback alone would make it useless for
// looking anything up.
//
// The panel holds no reference to the entry it drew. Playlist entries move when
// rows are removed or the playlist reloads, so it copies what it needs on the way
// through and keeps only strings.
//
// **It is one wxHtmlWindow, and the page is rebuilt from those strings.** What
// that replaced was twenty read-only wxTextCtrls in a wxFlexGridSizer inside a
// wxScrolled, each with a minimum height computed from its own line count, plus a
// custom control that drew the cover and a relayout that pinned the virtual width
// to the client width so nothing could be laid out where no scrollbar could reach
// it. All of that was arithmetic in service of a document, and wxHTML lays out
// documents: the table wraps in the width it is given, the cover is a row of that
// same document, and the scrolling is the one thing wxHtmlWindow was always going
// to do correctly.
//
// Three things follow from the switch, and they are the reason to read this far.
//
//   Selection. wxHtmlWindow selects across cells and copies with Ctrl+C -- Cmd+C
//   on macOS, which is what wxMOD_CONTROL means there -- so a path or a gain
//   block can still be lifted out of the panel, which is a large part of why an
//   info panel gets opened. It is not a text control: the caret and word-by-word
//   keyboard selection are gone, and a selection dragged across rows carries the
//   labels with it. A right-click menu offers Copy and Select All, because
//   nothing on screen otherwise says selecting is possible.
//
//   Wrapping. wxHTML breaks lines at whitespace and nowhere else (winpars.cpp
//   only ever splits on space, tab, CR and LF), so a long path -- which has no
//   spaces -- cannot wrap. It makes the page wider than the pane and a horizontal
//   scrollbar appears. The wxTextCtrl before it wrapped mid-word instead. This is
//   a real difference and it is deliberate: breaking a path for display means
//   either lying about it or copying it back out with a newline in the middle,
//   and this is the field people copy.
//
//   The cover goes through the memory filesystem, because that is how an image
//   reaches wxHTML. It is scaled here rather than by a WIDTH attribute so the
//   scaling is ours, and a second copy is registered under @2x, which is the name
//   wxHTML's IMG handler looks for first on a Retina screen.

#pragma once

#include "xpcog/core/library/PlaylistEntry.hpp"

#include <wx/html/htmlwin.h>
#include <wx/image.h>
#include <wx/string.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace xpcog {
class Library;
}

namespace xpcog::app {

/// The formatting Cog does in PlaylistEntry's derived accessors. Free functions
/// because they are the testable part -- and testable without a display at all,
/// since none of them touches a widget.
namespace info {

/// Cog's -trackText: "03", or "1.03" when the disc is known, or empty.
[[nodiscard]] std::string trackText(std::int32_t track, std::int32_t disc);

/// Cog's -lengthInfo, which unlike the playlist column keeps the fraction --
/// this is the panel you open when you care whether a gapless rip is 4:07.000.
[[nodiscard]] std::string lengthText(double seconds);

/// Cog's -gainInfo: every gain value that is present, one per line. Empty when
/// the file carries none, which is most files.
[[nodiscard]] std::string replayGainText(const ReplayGainInfo& gain);

/// Cog's -playCountInfo, with the count on the first line. `firstSeen` and
/// `lastPlayed` are Unix seconds; 0 means never set.
[[nodiscard]] std::string playCountText(std::int64_t count, std::int64_t firstSeen,
                                        std::int64_t lastPlayed);

/// One tag value as it goes into the page: the four characters that mean
/// something to a parser escaped, and newlines turned into breaks.
///
/// Worth having on its own and worth a test, because the failure is silent in
/// exactly the way this project keeps finding. A title of `<3` is a tag a parser
/// reads as the start of an element and drops, so the track appears to have no
/// title at all rather than to have an odd one -- and it is a *tag*, so no
/// fixture written in ASCII English will ever contain one.
[[nodiscard]] std::string escapeHtml(std::string_view text);

}  // namespace info

class InfoPanel : public wxHtmlWindow {
public:
    /// `library` may be null -- it is only consulted for album art and play
    /// counts, both of which live in the database rather than on the entry.
    InfoPanel(wxWindow* parent, const Library* library);

    ~InfoPanel() override;

    /// Draws `entry`, or clears the panel when it is null. Null is the ordinary
    /// case at startup and after the last row is removed, not an error.
    void showEntry(const PlaylistEntry* entry);

private:
    /// The fields, in Cog's order. The three groups are where Cog's own order
    /// already breaks: who made it, what it is, and what we know about it.
    enum Field {
        AlbumArtist,
        Artist,
        Composer,
        Album,
        Title,
        Track,
        Length,
        Date,
        Genre,
        Filename,

        SampleRate,
        Channels,
        Bitrate,
        BitsPerSample,
        Codec,
        Encoding,

        Cuesheet,
        ReplayGain,
        PlayCount,
        Comment,

        FieldCount,
    };

    void set(Field field, const std::string& value);

    /// The size, in DIP, the cover is worth drawing at in the width there is.
    ///
    /// Zero when there is no cover. Whichever constraint binds first: the height
    /// a cover is worth showing at, or the width the pane actually has -- the
    /// second is what makes it shrink in a narrow pane rather than force a
    /// horizontal scrollbar. Sizes are in DIP because that is what wxHTML wants:
    /// off macOS it multiplies image pixels by the DPI scale itself.
    [[nodiscard]] wxSize artSize();

    /// Puts the cover in the memory filesystem at `size`, under a name nothing
    /// else is using, and drops the one registered before it.
    void publishArt(wxSize size);

    /// Rebuilds the page and shows it, keeping the scroll position.
    ///
    /// A no-op when the HTML comes out identical, which is the common case:
    /// refreshInfo() runs on every selection change and on metadata arriving, and
    /// SetPage() would scroll a reader back to the top each time.
    void render();

    const Library* library_ = nullptr;

    std::array<std::string, FieldCount> values_;

    /// The cover at its own size. Kept rather than a scaled copy, so every
    /// resize scales once from the source instead of losing a little more detail
    /// each time the pane is dragged somewhere new.
    wxImage cover_;

    /// What is registered in the memory filesystem, and at what size. Empty when
    /// nothing is; `retina_` says whether the @2x copy went in beside it.
    wxString artFile_;
    wxSize   artSize_;
    bool     retina_ = false;

    wxString page_;
};

}  // namespace xpcog::app
