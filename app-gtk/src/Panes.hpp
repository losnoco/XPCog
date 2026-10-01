// The panes: Info, Lyrics, the equaliser, pitch & tempo, and the strip that
// holds the bottom ones.
//
// Each is the drawing half of a thing whose deciding half is uicore's --
// info::describe() for the fields, LyricsPresenter for the words, the
// settings and the preset library for the curve. What a pane does when a
// setting changes under it is publish the key, exactly as the wx panels do,
// so the window can hand it to the session and the engine re-reads the chain
// mid-track.
//
// ToolsStrip is what replaces the wxAUI docks for the bottom panes: a row of
// sections, each with a header naming it and a button closing it, each shown
// or hidden by its View action. It is a place rather than a dock -- nothing
// here floats, tabs or moves -- and docs/GTKPORT.md records that as the
// decision it is.

#pragma once

#include "Glib.hpp"
#include "LyricsText.hpp"

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Signal.hpp"
#include "xpcog/core/library/PlaylistEntry.hpp"

#include <adwaita.h>

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <map>
#include <string>
#include <vector>

namespace xpcog {
class Library;
class EqualizerPresetLibrary;
}  // namespace xpcog

namespace xpcog::gtk {

/// Cog's info HUD as rows: the cover on top, then label and value per field
/// the entry has something to say for.
class InfoPane {
public:
    explicit InfoPane(const Library* library);
    [[nodiscard]] GtkWidget* widget() const { return root_; }

    /// Redraws for `entry`, or the empty state for null.
    void showEntry(const PlaylistEntry* entry);

    /// Shows the cover on its own, as large as the window allows. Nothing
    /// when there is no cover.
    void showArtwork();

private:
    const Library* library_;
    GtkWidget*     root_    = nullptr;
    GtkWidget*     coverButton_ = nullptr;
    GtkWidget*     picture_ = nullptr;
    GObjectPtr<GdkTexture> cover_;
    std::string            coverTitle_;
    std::vector<Connection> connections_;
    GtkWidget*     grid_    = nullptr;
    GtkWidget*     empty_   = nullptr;
    std::vector<GtkWidget*> rows_;
};

/// The words, or the sentence saying why not, with the heading naming the
/// track and a line saying where the words came from when not the tag. Timed
/// words are followed while the track on screen is the one playing: the sung
/// line marked in the accent and kept in view, and left alone while the
/// reader has text selected.
class LyricsPane {
public:
    /// `position` reports how far into the playing track the listener has
    /// got, in seconds; asked only while a timed file is being followed.
    explicit LyricsPane(std::function<double()> position);
    [[nodiscard]] GtkWidget* widget() const { return root_; }

    /// `playing` says whether `entry` is the track being played, which is
    /// what decides whether timed lyrics are followed or only shown.
    void showEntry(const PlaylistEntry* entry, bool playing);
    void setLookup(LyricsLookup* lookup);
    /// `lyricsSynced`; takes effect on the next showEntry().
    void setTimed(bool timed) { presenter_.setTimed(timed); }

private:
    void present(const app::LyricsText& text);
    void updateFollowing();
    void tick();
    void styleLine(std::size_t index, bool sung);
    void applyAccent();

    GtkWidget*        root_    = nullptr;
    GtkWidget*        heading_ = nullptr;
    GtkWidget*        timed_   = nullptr;
    GtkWidget*        text_    = nullptr;
    GtkWidget*        source_  = nullptr;
    GtkTextTag*       sungTag_ = nullptr;  ///< Owned by the buffer's tag table.
    GtkTextMark*      scrollMark_ = nullptr;

    std::function<double()>     position_;
    Timeout                     timer_;
    std::optional<SyncedLyrics> synced_;
    std::size_t                 sung_    = SyncedLyrics::npos;
    bool                        playing_ = false;

    std::vector<Connection> connections_;
    app::LyricsPresenter    presenter_;
};

/// The preamp and 31 bands, the preset picker, and the two switches.
class EqualizerPane {
public:
    explicit EqualizerPane(Settings& settings);
    [[nodiscard]] GtkWidget* widget() const { return root_; }

    /// Re-reads the sliders, the picker and the switches from the settings,
    /// for when something else wrote the curve -- genre tracking, the remote.
    void refresh();

    /// A setting under this pane changed; carries the key.
    Signal<std::string> settingChanged;

private:
    void addBand(GtkWidget* row, const std::string& caption, const std::string& key);
    void selectPreset(int index);
    void markCustom();
    void enableIfSilent();
    void flatten();
    void publishCurve();
    [[nodiscard]] int customIndex() const;

    Settings&                     settings_;
    const EqualizerPresetLibrary& presets_;

    GtkWidget*               root_       = nullptr;
    GtkWidget*               enabled_    = nullptr;
    GtkWidget*               presetDrop_ = nullptr;
    GtkWidget*               trackGenre_ = nullptr;
    std::vector<GtkWidget*>  scales_;
    std::vector<GtkWidget*>  readouts_;
    std::vector<std::string> keys_;

    /// Set while this pane writes its own widgets, so their handlers know the
    /// change is not the listener's.
    bool syncing_ = false;

    std::vector<Connection> connections_;
};

/// Two scales through SpeedCurve, the lock, and a button to the pane that
/// chooses the engine.
class SpeedPane {
public:
    explicit SpeedPane(Settings& settings);
    [[nodiscard]] GtkWidget* widget() const { return root_; }

    void refresh();

    Signal<std::string> settingChanged;
    /// The "Settings..." button: open Preferences on Pitch & Tempo.
    Signal<> settingsRequested;

private:
    void write(const char* key, double ratio);

    Settings& settings_;

    GtkWidget* root_       = nullptr;
    GtkWidget* pitchRow_   = nullptr;
    GtkWidget* pitch_      = nullptr;
    GtkWidget* pitchValue_ = nullptr;
    GtkWidget* tempo_      = nullptr;
    GtkWidget* tempoValue_ = nullptr;
    GtkWidget* lock_       = nullptr;
    GtkWidget* note_       = nullptr;

    bool syncing_ = false;

    std::vector<Connection> connections_;
};

/// The strip under the playlist: named sections side by side, each shown or
/// hidden on its own, and the strip itself gone when none is shown.
class ToolsStrip {
public:
    ToolsStrip();
    [[nodiscard]] GtkWidget* widget() const { return root_; }

    /// Adds a section holding `content`, under `name`, with `title` in its
    /// header. Hidden until shown.
    void addSection(const std::string& name, const std::string& title, GtkWidget* content);

    void setShown(const std::string& name, bool shown);
    [[nodiscard]] bool shown(const std::string& name) const;
    [[nodiscard]] bool anyShown() const;

    /// The header's close button was pressed for `name`; the window flips the
    /// action and calls setShown.
    Signal<std::string> closeRequested;

private:
    struct Section {
        GtkWidget* revealer = nullptr;
    };

    GtkWidget*                     root_ = nullptr;  // the scroller
    GtkWidget*                     row_  = nullptr;  // the sections, side by side
    std::map<std::string, Section> sections_;
    std::vector<Connection>        connections_;
};

}  // namespace xpcog::gtk
