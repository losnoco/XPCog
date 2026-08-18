// Everything known about one track.
//
// Port of Cog's InfoInspector (InfoWindowController + InfoInspector.xib), field
// for field and label for label. Cog's is a floating HUD panel it positions to
// the right of the main window by hand; this is a dock, for the reason the
// equaliser is one -- a dock is a window with the window management already
// done, it remembers where it was, and it can still be torn off by anyone who
// wants it floating.
//
// The rule for *which* track is Cog's, from InfoWindowController's observer: the
// playlist selection when there is one, and the playing track otherwise. It is a
// good rule. Following the selection alone would blank the panel every time you
// clicked empty space, and following playback alone would make it useless for
// looking anything up.
//
// The panel holds no reference to the entry it drew. Playlist entries move when
// rows are removed or the playlist reloads, so it copies what it needs on the
// way through and keeps only strings.

#pragma once

#include "xpcog/core/library/PlaylistEntry.hpp"

#include <QList>
#include <QString>
#include <QWidget>

class QLabel;

namespace xpcog {
class Library;
}

namespace xpcog::app {

/// The formatting Cog does in PlaylistEntry's derived accessors. Free functions
/// because they are the testable part: this binary's test harness has no
/// QApplication and so cannot build a widget, but it can check that a disc-and-
/// track pair reads "1.03" and that replay gain comes out in Cog's wording.
namespace info {

/// Cog's -trackText: "03", or "1.03" when the disc is known, or empty.
[[nodiscard]] QString trackText(std::int32_t track, std::int32_t disc);

/// Cog's -lengthInfo, which unlike the playlist column keeps the fraction --
/// this is the panel you open when you care whether a gapless rip is 4:07.000.
[[nodiscard]] QString lengthText(double seconds);

/// Cog's -gainInfo: every gain value that is present, one per line. Empty when
/// the file carries none, which is most files.
[[nodiscard]] QString replayGainText(const ReplayGainInfo& gain);

/// Cog's -playCountInfo, with the count on the first line. `firstSeen` and
/// `lastPlayed` are Unix seconds; 0 means never set.
[[nodiscard]] QString playCountText(std::int64_t count, std::int64_t firstSeen,
                                    std::int64_t lastPlayed);

}  // namespace info

class InfoPanel : public QWidget {
    Q_OBJECT

public:
    /// `library` may be null -- it is only consulted for album art and play
    /// counts, both of which live in the database rather than on the entry.
    explicit InfoPanel(const Library* library, QWidget* parent = nullptr);

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

    void set(Field field, const QString& value);

    const Library*  library_ = nullptr;
    QLabel*         art_     = nullptr;
    QList<QLabel*>  values_;
};

}  // namespace xpcog::app
