#pragma once

#include "CommandMenus.hpp"
#include "FileTreePane.hpp"
#include "Panes.hpp"
#include "PlaylistTable.hpp"
#include "WinRT.hpp"

#include "xpcog/core/Signal.hpp"
#include "xpcog/core/library/PlaylistEntry.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace xpcog::app {
class Session;
}

namespace xpcog::winui {

/// The player's window: Mica under everything, the title bar drawn by WinUI's
/// TitleBar control with the filter in it, the menu bar, a transport row, the
/// playlist on a content layer, and a status line.
///
/// Layered the way the Windows 11 design guidance asks: the window's own
/// surfaces are transparent so the Mica backdrop shows through, and the one
/// region that holds content -- the playlist -- sits on LayerFillColorDefault,
/// the low-opacity content-layer brush, as a card.
class MainWindow {
public:
    explicit MainWindow(app::Session& session);
    ~MainWindow();

    MainWindow(const MainWindow&)            = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    void activate();

    [[nodiscard]] HWND hwnd() const;

    /// The window has closed. The session should be saved now: the dispatcher
    /// that runs everything is about to stop.
    std::function<void()> closed;

private:
    void build();
    void wireUp();

    [[nodiscard]] mux::Controls::Button transportButton(const wchar_t* glyph,
                                                       const std::string& tooltip,
                                                       std::function<void()> action);

    // --- commands -------------------------------------------------------------
    void onCommand(app::CommandId id);
    [[nodiscard]] bool                enabled(app::CommandId id) const;
    [[nodiscard]] std::optional<bool> checked(app::CommandId id) const;
    [[nodiscard]] static bool         offered(app::CommandId id);
    void refreshCommands();

    /// The selected rows' tracks, top to bottom.
    [[nodiscard]] std::vector<TrackId> selectedTracks() const;
    [[nodiscard]] bool                 selectionHasFiles() const;

    void showPlaylistMenu(const mux::UIElement& target,
                          std::optional<winrt::Windows::Foundation::Point> at);

    // --- the panes ------------------------------------------------------------
    void showFileTree(bool show);
    [[nodiscard]] bool fileTreeShown() const;
    /// The View menu's Info or Lyrics: show the panel at that page, or hide it
    /// when it is already showing that page -- GTK's togglePanel().
    void togglePanel(const std::string& page);
    void showPanelPage(const std::string& page);
    [[nodiscard]] bool panelShown() const;
    void showTool(const std::string& name, bool show);
    /// The track Info and Lyrics describe: the selection's first, or the
    /// playing one, as the follow setting says.
    [[nodiscard]] TrackId panelTrackId() const;
    void refreshPanels();

    // --- what is remembered -------------------------------------------------------
    void persistState();
    void restoreState();

    // --- dialogs --------------------------------------------------------------
    winrt::fire_and_forget openFiles();
    winrt::fire_and_forget openFolder();
    winrt::fire_and_forget openUrl();
    winrt::fire_and_forget savePlaylist(bool selectionOnly);
    winrt::fire_and_forget trashSelected();
    winrt::fire_and_forget showAbout();
    /// A ContentDialog ready to show over this window, with the default button
    /// and the theme set the way every dialog here wants them.
    [[nodiscard]] mux::Controls::ContentDialog dialog(const std::string& title) const;
    void setStatus(const std::string& text);

    // --- what playback reports ------------------------------------------------
    void onTrackChanged(const PlaylistEntry* entry);
    void onPlaybackStateChanged(bool playing, bool paused);
    void onPositionChanged(double seconds, double duration);
    void setClock(double seconds, double duration);

    app::Session& session_;

    mux::Window                          window_{nullptr};
    mux::Controls::TitleBar              titleBar_{nullptr};
    mux::Controls::AutoSuggestBox        filter_{nullptr};
    mux::Controls::FontIcon              playGlyph_{nullptr};
    mux::Controls::Button                playButton_{nullptr};
    mux::Controls::Slider                seek_{nullptr};
    mux::Controls::TextBlock             clock_{nullptr};
    mux::Controls::Slider                volume_{nullptr};
    mux::Controls::TextBlock             status_{nullptr};
    std::unique_ptr<PlaylistTable>       playlist_;
    std::unique_ptr<CommandMenus>        commands_;

    // --- the panes ------------------------------------------------------------
    std::unique_ptr<FileTreePane>        fileTree_;
    mux::Controls::Border                treeCard_{nullptr};
    std::unique_ptr<InfoPane>            info_;
    std::unique_ptr<LyricsPane>          lyrics_;
    mux::Controls::SelectorBar           panelSelector_{nullptr};
    mux::Controls::Border                panelCard_{nullptr};
    /// "info" or "lyrics": which page the side panel shows, whether or not
    /// the panel itself is shown.
    std::string                          panelPage_;
    std::unique_ptr<EqualizerPane>       equalizer_;
    std::unique_ptr<SpeedPane>           speed_;
    std::unique_ptr<ToolsStrip>          tools_;
    mux::FrameworkElement                toolsHost_{nullptr};
    /// The window's last size and place while it was neither maximised nor
    /// minimised -- what to restore it to, which AppWindow does not remember.
    winrt::Windows::Graphics::RectInt32  normalBounds_{};
    bool                                 maximizeOnShow_ = false;

    double duration_ = 0.0;
    /// Set while the code moves a slider, so its ValueChanged is not taken for
    /// the listener's.
    bool settingSeek_   = false;
    bool settingVolume_ = false;
    /// The seek bar's pointer is down: the position stops following playback,
    /// and the seek happens on release.
    bool scrubbing_ = false;
    /// A dialog is open. ContentDialog allows one at a time per window, and a
    /// second ShowAsync throws.
    bool dialogOpen_ = false;

    std::vector<Subscription> subscriptions_;
};

}  // namespace xpcog::winui
