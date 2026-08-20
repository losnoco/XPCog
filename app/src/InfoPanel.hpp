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
// The values are read-only wxTextCtrls rather than static text, and that is not
// cosmetic: a path or a cuesheet you can select and copy is a large part of why
// an info panel gets opened, and wxStaticText cannot be selected.

#pragma once

#include "xpcog/core/library/PlaylistEntry.hpp"

#include <wx/image.h>
#include <wx/scrolwin.h>

#include <cstdint>
#include <string>
#include <vector>

class wxTextCtrl;

namespace xpcog {
class Library;
}

namespace xpcog::app {

/// The cover, drawn to fit whatever room it is given. Defined in the .cpp.
class ArtworkView;

/// The formatting Cog does in PlaylistEntry's derived accessors. Free functions
/// because they are the testable part -- and now testable without a display at
/// all, since none of them touches a widget.
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

}  // namespace info

class InfoPanel : public wxScrolled<wxPanel> {
public:
    /// `library` may be null -- it is only consulted for album art and play
    /// counts, both of which live in the database rather than on the entry.
    InfoPanel(wxWindow* parent, const Library* library);

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

    /// Tells the cover how much height to ask the sizer for.
    ///
    /// Called from showEntry and again on every resize, because the pane is
    /// dockable: it can be dragged from the right edge to the bottom, where it is
    /// a completely different shape. A fixed height alone -- which is what Cog's
    /// fixed-size HUD could get away with -- is wider than the pane the moment
    /// the pane is narrower than the cover.
    ///
    /// This only decides the height. Fitting the image inside the rectangle that
    /// results is ArtworkView's job, and it does that unconditionally, so a
    /// height this got slightly wrong is a little wasted space rather than a
    /// clipped cover.
    void updateArt();

    /// Lays out inside the client width rather than inside the virtual width.
    ///
    /// wxScrolled::Layout() sizes its sizer to GetVirtualSize(), so FitInside()
    /// -- which sets the virtual size from the sizer's minimum -- is what lets
    /// content be laid out past the right-hand edge. With horizontal scrolling
    /// deliberately off there is then no way to reach it: it is simply cut off.
    /// Pinning the virtual width to the client width makes the layout honest, and
    /// leaves only the height to scroll.
    void relayout();

    const Library* library_ = nullptr;
    ArtworkView*   art_     = nullptr;

    std::vector<wxTextCtrl*> values_;
};

}  // namespace xpcog::app
