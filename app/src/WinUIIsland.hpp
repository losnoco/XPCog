#pragma once

// A prototype: WinUI 3 content hosted in the wx player through a XAML island.
//
// Built only with XPCOG_WITH_WINUI_ISLAND. What it is finding out is whether the
// Windows App SDK can live inside this build at all -- fetched and projected by
// CMake rather than restored by MSBuild, bootstrapped by an unpackaged process,
// and fed by wx's message loop rather than its own -- before anything is decided
// about porting the interface to it. See cmake/XPCogWinAppSdk.cmake.

#include <wx/window.h>

#include <functional>
#include <memory>

class wxAppTraits;

namespace xpcog::app {

/// Makes the Windows App Runtime available to this process and starts WinUI on
/// the calling thread, which has to be the interface thread. False, and nothing
/// left half-started, when the runtime is not installed or will not start; the
/// player then runs without the island.
bool startWinUI();

/// Undoes startWinUI(). After the last island has been destroyed.
void stopWinUI();

[[nodiscard]] bool winUIRunning();

/// Why WinUI is not running, or why the last island failed to build; empty when
/// nothing has gone wrong. Shown in the status bar, because a debug log is
/// compiled out of the builds anyone runs.
[[nodiscard]] wxString winUIFailure();

/// wx's own traits, except that the event loops they make offer each message to
/// WinUI before wx sees it. Without that, keyboard input in the island --
/// accelerators, Tab, focus moves -- does not work.
wxAppTraits* makeWinUIAppTraits();

/// A wx window whose whole client area is a WinUI island: a play/pause button
/// and a volume slider, wired to the same commands as the wx controls beside it.
class WinUIIsland : public wxWindow {
public:
    WinUIIsland(wxWindow* parent, double volume);
    ~WinUIIsland() override;

    /// False when the XAML content could not be built; winUIFailure() says why,
    /// and the window is then an empty one that should be removed.
    [[nodiscard]] bool ok() const;

    std::function<void()>       playPauseClicked;
    std::function<void(double)> volumeChanged;

    /// Moves the slider without reporting it back through volumeChanged.
    void setVolume(double gain);
    void setPlaying(bool playing);

private:
    void build(double volume);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xpcog::app
