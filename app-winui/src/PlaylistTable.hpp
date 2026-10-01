#pragma once

#include "WinRT.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace xpcog {
class PlaylistView;
class Settings;
}  // namespace xpcog

namespace xpcog::winui {

/// The playlist as a WinUI ListView over the session's PlaylistView:
/// virtualised rows filled straight from the view, no data binding, a header
/// whose columns sort, resize and reorder, and the selection, context menu and
/// file drops the window's commands work through.
///
/// Column widths share the wx list's saved setting, xpcog.playlist.columns, so
/// either player opens with the layout the other left.
class PlaylistTable {
public:
    PlaylistTable(PlaylistView& view, Settings& settings);
    ~PlaylistTable();

    PlaylistTable(const PlaylistTable&)            = delete;
    PlaylistTable& operator=(const PlaylistTable&) = delete;

    /// What to put in the window. Transparent: the host gives it its surface.
    [[nodiscard]] mux::UIElement element() const;

    std::function<void(std::size_t row)> rowActivated;
    std::function<void()>                selectionChanged;
    /// A right-click, Shift+F10 or the Menu key, after the selection has been
    /// moved under the pointer when it was outside -- the wx list's rule, and
    /// Cog's. Where to show the menu, relative to `target`; nullopt from the
    /// keyboard, where the menu goes at the focused row.
    std::function<void(const mux::UIElement& target,
                       std::optional<winrt::Windows::Foundation::Point> at)>
        contextMenuRequested;
    std::function<void(std::vector<std::filesystem::path>)> filesDropped;

    /// Selected rows, top to bottom.
    [[nodiscard]] std::vector<std::size_t> selectedRows() const;
    [[nodiscard]] bool                     hasSelection() const;
    void                                   selectAll();
    /// Replaces the selection with one row and brings it into view.
    void selectOnly(std::size_t row);

    /// Brings a row into view.
    void reveal(std::size_t row);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xpcog::winui
