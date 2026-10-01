#pragma once

// A prototype: WinUI 3 content hosted in the wx player through XAML islands.
//
// Built only with XPCOG_WITH_WINUI_ISLAND. What it is finding out is whether the
// Windows App SDK can live inside this build at all -- fetched and projected by
// CMake rather than restored by MSBuild, bootstrapped by an unpackaged process,
// and fed by wx's message loop rather than its own -- and whether WinUI can
// carry the playlist, before anything is decided about porting the interface
// to it. See cmake/XPCogWinAppSdk.cmake.
//
// Nothing here names a WinRT type, so the frame that includes it does not pull
// in the projection. WinUIHost.hpp is the other half, for the islands' own
// sources.

#include "xpcog/core/Signal.hpp"

#include <wx/window.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

class wxAppTraits;

namespace xpcog {
class PlaylistView;
class Settings;
}

namespace xpcog::app {

/// Makes the Windows App Runtime available to this process and starts WinUI on
/// the calling thread, which has to be the interface thread. False, and nothing
/// left half-started, when the runtime is not installed or will not start; the
/// player then runs without the islands.
bool startWinUI();

/// Undoes startWinUI(). After the last island has been destroyed.
void stopWinUI();

[[nodiscard]] bool winUIRunning();

/// Why WinUI is not running, or why the last island failed to build; empty when
/// nothing has gone wrong. Shown in the status bar, because a debug log is
/// compiled out of the builds anyone runs.
[[nodiscard]] wxString winUIFailure();

/// wx's own traits, except that the event loops they make offer each message to
/// WinUI before wx sees it. Without that, keyboard input in an island --
/// accelerators, Tab, focus moves -- does not work.
wxAppTraits* makeWinUIAppTraits();

/// A wx window whose whole client area is a XAML island. Keeps the island the
/// window's size and passes focus across the boundary in both directions; what
/// it shows is the subclass's.
class WinUIIsland : public wxWindow {
public:
    ~WinUIIsland() override;

    /// False when the XAML content could not be built; winUIFailure() says why,
    /// and the window is then an empty one that should be removed.
    [[nodiscard]] bool ok() const;

    struct Host;

protected:
    explicit WinUIIsland(wxWindow* parent);

    /// Shuts the island down after a subclass failed to build its content.
    void fail(const wxString& reason);

    std::unique_ptr<Host> host_;
};

/// A strip with a play/pause button, a volume slider and the switch between
/// the two playlists, wired to the same commands as the wx controls above it.
class WinUITransport : public WinUIIsland {
public:
    WinUITransport(wxWindow* parent, double volume);
    ~WinUITransport() override;

    std::function<void()>       playPauseClicked;
    std::function<void(double)> volumeChanged;
    std::function<void(bool)>   winUIPlaylistToggled;

    /// Moves the slider without reporting it back through volumeChanged.
    void setVolume(double gain);
    void setPlaying(bool playing);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// The playlist as a WinUI ListView over the same PlaylistView the wx list
/// reads: virtualised rows filled straight from the view, no data binding, a
/// header whose columns sort, resize and reorder, and the selection, context
/// menu and file drops the frame's commands work through.
class WinUIPlaylist : public WinUIIsland {
public:
    WinUIPlaylist(wxWindow* parent, PlaylistView& view, Settings& settings);
    ~WinUIPlaylist() override;

    std::function<void(std::size_t row)> rowActivated;
    std::function<void()>                selectionChanged;
    /// A right-click, after the selection has been moved under it when it was
    /// outside -- the wx list's rule, and Cog's.
    std::function<void()> contextMenuRequested;
    std::function<void(std::vector<std::filesystem::path>)> filesDropped;

    /// Selected rows, top to bottom.
    [[nodiscard]] std::vector<std::size_t> selectedRows() const;
    [[nodiscard]] bool                     hasSelection() const;
    void                                   selectAll();
    /// Replaces the selection with one row and brings it into view.
    void selectOnly(std::size_t row);

    /// Brings a row into view, as the wx list does for the playing track.
    void reveal(std::size_t row);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xpcog::app
