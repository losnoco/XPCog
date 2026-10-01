#pragma once

// The panes that are controls rather than drawings: Info, Lyrics, the
// equaliser and the speed controls, and the strip the last two sit in. Each
// is the GTK frontend's pane of the same name (app-gtk/src/Panes.hpp) in
// WinUI's controls -- same rules, same settings keys, same wording -- so where
// one is changed the other is the place to look.
//
// The painted ones -- spectrum, oscilloscope, waveform, the SC-55 -- are not
// here; they wait on the choice of what draws them.

#include "WinRT.hpp"

#include "LyricsText.hpp"

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Signal.hpp"
#include "xpcog/core/library/PlaylistEntry.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace xpcog {
class Library;
class LyricsLookup;
class EqualizerPresetLibrary;
}  // namespace xpcog

namespace xpcog::winui {

/// The selected or playing track's tags, file details and cover.
class InfoPane {
public:
    InfoPane(const Library* library, std::function<mux::XamlRoot()> xamlRoot);

    [[nodiscard]] mux::UIElement element() const { return root_; }

    void showEntry(const PlaylistEntry* entry);

private:
    winrt::fire_and_forget showArtwork();

    const Library*                  library_;
    std::function<mux::XamlRoot()>  xamlRoot_;
    mux::Controls::ScrollViewer     root_{nullptr};
    mux::Controls::Button           coverButton_{nullptr};
    mux::Controls::Image            cover_{nullptr};
    mux::Media::Imaging::BitmapImage coverImage_{nullptr};
    std::string                     coverTitle_;
    mux::Controls::Grid             grid_{nullptr};
    mux::Controls::TextBlock        empty_{nullptr};
    std::vector<mux::Controls::TextBlock> labels_;
    std::vector<mux::Controls::TextBlock> values_;
    bool dialogOpen_ = false;
};

/// The track's lyrics -- embedded, a sidecar file, or LRCLIB -- and, when they
/// are timed and the track is the one playing, the line being sung.
class LyricsPane {
public:
    explicit LyricsPane(std::function<double()> position);

    [[nodiscard]] mux::UIElement element() const { return root_; }

    void showEntry(const PlaylistEntry* entry, bool playing);
    void setLookup(LyricsLookup* lookup) { presenter_.setLookup(lookup); }
    void setTimed(bool timed);

    /// The Timed toggle was clicked; the window owns the setting.
    std::function<void()> timedToggled;

private:
    void present(const app::LyricsText& text);
    void updateFollowing();
    void tick();
    void styleLine(std::size_t index, bool sung);

    mux::Controls::Grid         root_{nullptr};
    mux::Controls::TextBlock    heading_{nullptr};
    mux::Controls::Primitives::ToggleButton timed_{nullptr};
    mux::Controls::ScrollViewer scroller_{nullptr};
    mux::Controls::StackPanel   lines_{nullptr};
    mux::Controls::TextBlock    source_{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{nullptr};

    std::function<double()>     position_;
    std::optional<SyncedLyrics> synced_;
    std::size_t                 sung_    = SyncedLyrics::npos;
    bool                        playing_ = false;
    bool                        settingTimed_ = false;
    app::LyricsPresenter        presenter_;
};

/// The 31-band graphic equaliser, its preamp, its presets.
class EqualizerPane {
public:
    explicit EqualizerPane(Settings& settings);

    [[nodiscard]] mux::UIElement element() const { return root_; }

    /// Re-reads every value from the settings: after a preset, and after the
    /// session applied a genre's preset on its own.
    void refresh();

    Signal<std::string> settingChanged;

private:
    void addBand(const mux::Controls::StackPanel& row, const std::string& caption,
                 const std::string& key);
    void selectPreset(int index);
    void markCustom();
    void enableIfSilent();
    void flatten();
    void publishCurve();
    [[nodiscard]] int customIndex() const;

    Settings&                     settings_;
    const EqualizerPresetLibrary& presets_;
    mux::Controls::StackPanel     root_{nullptr};
    mux::Controls::CheckBox       enabled_{nullptr};
    mux::Controls::ComboBox       preset_{nullptr};
    mux::Controls::CheckBox       trackGenre_{nullptr};
    std::vector<mux::Controls::Slider>    scales_;
    std::vector<mux::Controls::TextBlock> readouts_;
    std::vector<std::string>              keys_;
    bool syncing_ = false;
};

/// Pitch and tempo, and whether they move together.
class SpeedPane {
public:
    explicit SpeedPane(Settings& settings);

    [[nodiscard]] mux::UIElement element() const { return root_; }

    void refresh();

    Signal<std::string> settingChanged;

private:
    void write(const char* key, double ratio);

    Settings& settings_;
    mux::Controls::StackPanel root_{nullptr};
    mux::Controls::Grid       pitchRow_{nullptr};
    mux::Controls::Slider     pitch_{nullptr};
    mux::Controls::TextBlock  pitchValue_{nullptr};
    mux::Controls::Slider     tempo_{nullptr};
    mux::Controls::TextBlock  tempoValue_{nullptr};
    mux::Controls::CheckBox   lock_{nullptr};
    mux::Controls::TextBlock  note_{nullptr};
    bool syncing_ = false;
};

/// The bottom strip: tools side by side, each a card with a heading and a
/// close button, shown and hidden from the View menu.
class ToolsStrip {
public:
    ToolsStrip();

    [[nodiscard]] mux::UIElement element() const { return root_; }

    /// How a section's content meets a card smaller than it wants.
    enum class Scroll {
        None,      ///< it scales to fit: the analysers, the SC-55
        Vertical,  ///< it reflows to the card's width and scrolls down
        Both,      ///< it has a natural width of its own: the equaliser's bands
    };

    void addSection(const std::string& name, const std::string& title,
                    const mux::UIElement& content, Scroll scroll = Scroll::None);
    void setShown(const std::string& name, bool shown);
    [[nodiscard]] bool shown(const std::string& name) const;
    [[nodiscard]] bool anyShown() const;

    std::function<void(const std::string&)> closeRequested;

private:
    mux::Controls::Grid root_{nullptr};
    std::map<std::string, mux::FrameworkElement> sections_;
    std::vector<std::string> order_;
    void relayout();
};

}  // namespace xpcog::winui
